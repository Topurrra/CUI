/* Vulkan renderer (cui_gpu_*, built with CUI_VULKAN; Linux, on the X11 window): the draw data the OpenGL renderer takes (cui_draw_data's vertices, indices and
 * commands), drawn the same way. One pipeline; its fragment shader is cui_render_gl.c's
 * (cui_render_vulkan.frag), branching on the vertex's mode. Keep the two in step.
 *
 * - Vulkan 1.3 (dynamic rendering) with a timeline semaphore. CUI_GPU picks the device,
 *   (the validation layer is switched on outside the program, with VK_LOADER_LAYERS_ENABLE).
 * - Two command buffers in turn, each reused once the timeline value of its last submission
 *   has passed: texture uploads go in as CUI makes them, the frame's draws after.
 * - A frame's vertices and indices are copied into a mapped buffer (one per command buffer),
 *   then a draw per command with its clip rectangle as the scissor and its texture's
 *   descriptor set (a set per texture, written once when the texture is made).
 * - Drawn straight into a UNORM swapchain image, premultiplied source over, as the OpenGL
 *   renderer blends. Paced by acquiring the image (FIFO with vsync). */
#include "../cui_internal.h"
#include "cui/cui_render.h"

#if defined(CUI_VULKAN) && defined(__linux__)

#define VK_USE_PLATFORM_XLIB_KHR
#include <X11/Xlib.h>
#include <vulkan/vulkan.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "generated/cui_render_vulkan_spirv.h"

#define CUI__TV_FRAMES 2            /* command buffers (and upload buffers) in flight */
#define CUI__TV_IMAGES 3            /* swapchain images asked for */
#define CUI__TV_MAX_IMAGES 16
#define CUI__TV_TEX_CAP 4096        /* textures alive at once */

typedef struct cui__vk_buffer {     /* a mapped buffer */
    VkBuffer buffer;
    VkDeviceMemory memory;
    uint8_t *ptr;
    size_t cap, used;
} cui__vk_buffer;

typedef struct cui__vk_tex {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkDescriptorSet set;
    int bpp;                        /* 0: the slot is free */
} cui__vk_tex;

/* what goes once the GPU is past `value` */
typedef struct cui__vk_later {
    uint64_t value;
    VkBuffer buffer; VkImage image; VkImageView view; VkDeviceMemory memory; VkDescriptorSet set;
    int slot;                       /* a texture slot to free, or -1 */
} cui__vk_later;

struct cui_gpu_renderer {
    cui_x11_window *xwin;
    bool vsync;
    VkInstance instance;
    VkPhysicalDevice physical;
    VkPhysicalDeviceMemoryProperties mem;
    VkDevice device;
    uint32_t family;
    VkQueue queue;
    VkCommandPool pool;
    VkCommandBuffer cmd[CUI__TV_FRAMES];
    uint64_t cmd_value[CUI__TV_FRAMES];
    cui__vk_buffer upload[CUI__TV_FRAMES];
    cui__vk_buffer kept;            /* a captured frame whose copy was not known to be done: freed at teardown */
    int frame;                     /* the command buffer being recorded */
    bool open;
    VkSemaphore timeline;
    uint64_t value;                 /* the last submission's */
    cui__vk_later *later; int later_count, later_cap;
    VkSampler sampler;
    VkDescriptorPool set_pool;
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    cui__vk_tex *tex;               /* by cui_texture - 1 */
    int tex_count, tex_cap;
    int none;                       /* a black texel, for a command whose texture is gone */
    VkSurfaceKHR surface;
    VkFormat format;
    VkSwapchainKHR swapchain;
    VkExtent2D extent;
    uint32_t image_count, image;
    VkImage images[CUI__TV_MAX_IMAGES];
    VkImageView views[CUI__TV_MAX_IMAGES];
    VkSemaphore present_sem[CUI__TV_MAX_IMAGES];
    VkSemaphore acquire_sem[CUI__TV_FRAMES];
    bool stale;
    bool lost;                      /* a submission failed: nothing more is drawn */
    bool can_capture;               /* the swapchain images can be copied from */
};

/* ---- memory ---- */

static uint32_t cui__vk_memory_type(cui_gpu_renderer *r, uint32_t bits, VkMemoryPropertyFlags want) {
    const VkPhysicalDeviceMemoryProperties *mp = &r->mem;
    /* host memory when mapped, video memory otherwise */
    bool host = (want & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp->memoryTypes[i].propertyFlags & want) == want &&
            (!host || !(mp->memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))) return i;
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp->memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

static bool cui__vk_buffer_make(cui_gpu_renderer *r, cui__vk_buffer *b, size_t cap, VkBufferUsageFlags usage) {
    VkDevice dev = r->device;
    cui__vk_buffer made = { 0 };
    VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    info.size = cap;
    info.usage = usage;
    if (vkCreateBuffer(dev, &info, NULL, &made.buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, made.buffer, &req);
    VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = cui__vk_memory_type(r, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (alloc.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(dev, &alloc, NULL, &made.memory) != VK_SUCCESS) {
        vkDestroyBuffer(dev, made.buffer, NULL);
        return false;
    }
    if (vkBindBufferMemory(dev, made.buffer, made.memory, 0) != VK_SUCCESS ||
        vkMapMemory(dev, made.memory, 0, VK_WHOLE_SIZE, 0, (void **)&made.ptr) != VK_SUCCESS) {
        vkDestroyBuffer(dev, made.buffer, NULL);
        vkFreeMemory(dev, made.memory, NULL);
        return false;
    }
    made.cap = cap;
    *b = made;
    return true;
}

static void cui__vk_buffer_free(cui_gpu_renderer *r, cui__vk_buffer *b) {
    if (b->buffer) vkDestroyBuffer(r->device, b->buffer, NULL);
    if (b->memory) vkFreeMemory(r->device, b->memory, NULL);
    memset(b, 0, sizeof *b);
}

/* ---- submissions ---- */

/* True when nothing runs on the device any more (it is idle, or lost). */
static bool cui__vk_idle_or_gone(cui_gpu_renderer *r) {
    VkResult res = vkDeviceWaitIdle(r->device);
    return res == VK_SUCCESS || res == VK_ERROR_DEVICE_LOST;
}

/* Waits until the submission that signals `value` is done. False when that is not known (the
 * wait failed and the device could not be made idle): what the submission uses must stay. */
static bool cui__vk_wait(cui_gpu_renderer *r, uint64_t value) {
    if (value == 0) return true;
    if (r->lost) return cui__vk_idle_or_gone(r);   /* (a submission failed: the timeline does not reach it) */
    VkSemaphoreWaitInfo info = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
    info.semaphoreCount = 1;
    info.pSemaphores = &r->timeline;
    info.pValues = &value;
    if (vkWaitSemaphores(r->device, &info, UINT64_MAX) != VK_SUCCESS) {   /* (nothing more is drawn) */
        r->lost = true;
        return cui__vk_idle_or_gone(r);
    }
    return true;
}

static void cui__vk_later_free(cui_gpu_renderer *r, const cui__vk_later *l) {
    VkDevice dev = r->device;
    if (l->set) vkFreeDescriptorSets(dev, r->set_pool, 1, &l->set);
    if (l->view) vkDestroyImageView(dev, l->view, NULL);
    if (l->image) vkDestroyImage(dev, l->image, NULL);
    if (l->buffer) vkDestroyBuffer(dev, l->buffer, NULL);
    if (l->memory) vkFreeMemory(dev, l->memory, NULL);
    if (l->slot >= 0) r->tex[l->slot].bpp = 0;
}

/* the objects whose submission is done (all of them when idle); their texture slots are free again */
static void cui__vk_collect(cui_gpu_renderer *r, bool all) {
    uint64_t done = UINT64_MAX;
    if (!all && vkGetSemaphoreCounterValue(r->device, r->timeline, &done) != VK_SUCCESS) return;   /* not known: nothing is freed yet */
    int n = 0;
    for (int i = 0; i < r->later_count; i++) {
        if (r->later[i].value <= done) cui__vk_later_free(r, &r->later[i]);
        else r->later[n++] = r->later[i];
    }
    r->later_count = n;
}

/* frees l after what is recorded so far has run */
static void cui__vk_release_later(cui_gpu_renderer *r, cui__vk_later l) {
    l.value = r->value + 1;
    if (r->later_count == r->later_cap) {
        int cap = r->later_cap ? r->later_cap * 2 : 64;
        cui__vk_later *grown = realloc(r->later, (size_t)cap * sizeof *grown);
        if (!grown) return;   /* out of memory: commands recorded and not yet run may use it, so it is left alive */
        r->later = grown;
        r->later_cap = cap;
    }
    r->later[r->later_count++] = l;
}

/* The open command buffer, begun on the next one once its last submission is done. Null when
 * the renderer has stopped or the buffer will not begin: the caller records nothing. */
static VkCommandBuffer cui__vk_cmd(cui_gpu_renderer *r) {
    if (r->lost) return VK_NULL_HANDLE;
    if (!r->open) {
        int f = r->frame;
        if (!cui__vk_wait(r, r->cmd_value[f]) || r->lost) return VK_NULL_HANDLE;   /* (its last submission may still run: it is not reset) */
        VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkResetCommandBuffer(r->cmd[f], 0) != VK_SUCCESS || vkBeginCommandBuffer(r->cmd[f], &begin) != VK_SUCCESS) {
            r->lost = true;
            return VK_NULL_HANDLE;
        }
        r->upload[f].used = 0;
        r->open = true;
    }
    return r->cmd[r->frame];
}

/* submits the open command buffer, waiting for `wait` (at the color output) and signalling
 * `signal` (binary semaphores, either may be null) and the timeline */
static void cui__vk_submit(cui_gpu_renderer *r, VkSemaphore wait, VkSemaphore signal) {
    if (!r->open) return;
    int f = r->frame;
    if (vkEndCommandBuffer(r->cmd[f]) != VK_SUCCESS) r->lost = true;
    if (r->lost) {   /* nothing of it runs, and nothing waits for it */
        r->open = false;
        return;
    }
    uint64_t value = ++r->value;
    VkSemaphore signals[2] = { r->timeline, signal };
    uint64_t values[2] = { value, 0 };
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    uint64_t wait_value = 0;
    VkTimelineSemaphoreSubmitInfo tl = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
    tl.waitSemaphoreValueCount = wait ? 1 : 0;
    tl.pWaitSemaphoreValues = &wait_value;
    tl.signalSemaphoreValueCount = signal ? 2 : 1;
    tl.pSignalSemaphoreValues = values;
    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.pNext = &tl;
    submit.waitSemaphoreCount = wait ? 1 : 0;
    submit.pWaitSemaphores = &wait;
    submit.pWaitDstStageMask = &stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &r->cmd[f];
    submit.signalSemaphoreCount = signal ? 2 : 1;
    submit.pSignalSemaphores = signals;
    if (vkQueueSubmit(r->queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) r->lost = true;   /* nothing is drawn from here on */
    r->cmd_value[f] = value;
    r->frame = (f + 1) % CUI__TV_FRAMES;
    r->open = false;
    cui__vk_collect(r, false);
}

static void cui__vk_idle(cui_gpu_renderer *r) {
    cui__vk_submit(r, VK_NULL_HANDLE, VK_NULL_HANDLE);
    cui__vk_wait(r, r->value);
}

/* the stages and accesses of an image in a layout, as a barrier's source or destination */
static void cui__vk_layout_use(VkImageLayout layout, bool dst, VkPipelineStageFlags *stage, VkAccessFlags *access) {
    switch (layout) {
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        *access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | (dst ? VK_ACCESS_COLOR_ATTACHMENT_READ_BIT : 0);
        break;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: *stage = VK_PIPELINE_STAGE_TRANSFER_BIT; *access = VK_ACCESS_TRANSFER_WRITE_BIT; break;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: *stage = VK_PIPELINE_STAGE_TRANSFER_BIT; *access = dst ? VK_ACCESS_TRANSFER_READ_BIT : 0; break;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: *stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT; *access = dst ? VK_ACCESS_SHADER_READ_BIT : 0; break;
    /* undefined, present: nothing to wait for or make visible (the acquire semaphore waits at
     * the color output, and present waits on its semaphore) */
    default: *stage = dst ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT; *access = 0; break;
    }
}

static void cui__vk_barrier(cui_gpu_renderer *r, VkImage image, VkImageLayout from, VkImageLayout to) {
    VkPipelineStageFlags src_stage, dst_stage;
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    cui__vk_layout_use(from, false, &src_stage, &b.srcAccessMask);
    cui__vk_layout_use(to, true, &dst_stage, &b.dstAccessMask);
    /* (from undefined to a color attachment: after the acquire, which waits at the color output) */
    if (from == VK_IMAGE_LAYOUT_UNDEFINED && to == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) src_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    VkCommandBuffer cmd = cui__vk_cmd(r);
    if (cmd) vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

/* room in the open command buffer's upload buffer, valid until its submission is done */
static uint8_t *cui__vk_upload(cui_gpu_renderer *r, size_t bytes, size_t align, VkBuffer *buffer, size_t *offset) {
    if (!cui__vk_cmd(r)) return NULL;
    cui__vk_buffer *u = &r->upload[r->frame];
    size_t at = (u->used + align - 1) & ~(align - 1);
    if (!u->buffer || at + bytes > u->cap) {
        size_t cap = u->cap ? u->cap * 2 : (size_t)1 << 20;
        while (cap < bytes) cap *= 2;
        cui__vk_buffer grown;
        if (!cui__vk_buffer_make(r, &grown, cap, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT)) return NULL;
        if (u->buffer) {   /* what the open buffer already uses from it stays */
            cui__vk_later l = { 0 };
            l.buffer = u->buffer; l.memory = u->memory; l.slot = -1;
            cui__vk_release_later(r, l);
        }
        *u = grown;
        at = 0;
    }
    u->used = at + bytes;
    *buffer = u->buffer;
    *offset = at;
    return u->ptr + at;
}

/* ---- textures: R8 for the glyph atlas, RGBA8 for images ---- */

static bool cui__vk_tex_upload(cui_gpu_renderer *r, cui__vk_tex *t, int x, int y, int w, int h, const void *pixels, int stride, bool fresh) {
    size_t row = (size_t)w * (size_t)t->bpp, from = stride ? (size_t)stride : row;
    VkBuffer src;
    size_t offset;
    uint8_t *p = cui__vk_upload(r, row * (size_t)h, 16, &src, &offset);
    if (!p) return false;   /* no memory for the pixels */
    for (int k = 0; k < h; k++) memcpy(p + (size_t)k * row, (const uint8_t *)pixels + (size_t)k * from, row);
    cui__vk_barrier(r, t->image, fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy copy = { 0 };
    copy.bufferOffset = offset;
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageOffset.x = x; copy.imageOffset.y = y;
    copy.imageExtent.width = (uint32_t)w; copy.imageExtent.height = (uint32_t)h; copy.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cui__vk_cmd(r), src, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    cui__vk_barrier(r, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

static cui_texture cui__vk_tex_create(void *user, int w, int h, cui_texture_format fmt, const void *pixels) {
    cui_gpu_renderer *r = user;
    if (w <= 0 || h <= 0 || r->lost) return 0;
    cui__vk_collect(r, false);   /* (slots whose textures are done with) */
    int slot = -1;
    for (int i = 0; i < r->tex_count; i++) if (!r->tex[i].bpp && !r->tex[i].image) { slot = i; break; }
    if (slot < 0) {
        if (r->tex_count == CUI__TV_TEX_CAP) return 0;
        if (r->tex_count == r->tex_cap) {
            int cap = r->tex_cap ? r->tex_cap * 2 : 16;
            void *n = realloc(r->tex, sizeof(*r->tex) * (size_t)cap);
            if (!n) return 0;
            r->tex = n;
            r->tex_cap = cap;
        }
        slot = r->tex_count++;
    }
    VkDevice dev = r->device;
    cui__vk_tex made = { 0 };
    VkFormat format = fmt == CUI_TEXTURE_R8 ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
    VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent.width = (uint32_t)w; info.extent.height = (uint32_t)h; info.extent.depth = 1;
    info.mipLevels = 1; info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    memset(&r->tex[slot], 0, sizeof r->tex[slot]);
    if (vkCreateImage(dev, &info, NULL, &made.image) != VK_SUCCESS) return 0;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, made.image, &req);
    VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = cui__vk_memory_type(r, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (alloc.memoryTypeIndex == UINT32_MAX) alloc.memoryTypeIndex = cui__vk_memory_type(r, req.memoryTypeBits, 0);
    VkImageViewCreateInfo view = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    view.image = made.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    VkDescriptorSetAllocateInfo sa = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    sa.descriptorPool = r->set_pool;
    sa.descriptorSetCount = 1;
    sa.pSetLayouts = &r->set_layout;
    if (alloc.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(dev, &alloc, NULL, &made.memory) != VK_SUCCESS ||
        vkBindImageMemory(dev, made.image, made.memory, 0) != VK_SUCCESS || vkCreateImageView(dev, &view, NULL, &made.view) != VK_SUCCESS ||
        vkAllocateDescriptorSets(dev, &sa, &made.set) != VK_SUCCESS) {
        if (made.view) vkDestroyImageView(dev, made.view, NULL);
        vkDestroyImage(dev, made.image, NULL);
        if (made.memory) vkFreeMemory(dev, made.memory, NULL);
        return 0;
    }
    /* its own set, written once: no frame in flight reads a set that changes */
    VkDescriptorImageInfo image = { VK_NULL_HANDLE, made.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    write.dstSet = made.set;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(dev, 1, &write, 0, NULL);
    made.bpp = fmt == CUI_TEXTURE_R8 ? 1 : 4;
    r->tex[slot] = made;
    if (pixels && !cui__vk_tex_upload(r, &r->tex[slot], 0, 0, w, h, pixels, 0, true)) {
        /* no texture without its pixels: the caller is told, and can make it again (no command uses it yet) */
        cui__vk_later gone = { 0 };
        gone.image = made.image; gone.view = made.view; gone.memory = made.memory; gone.set = made.set; gone.slot = slot;
        memset(&r->tex[slot], 0, sizeof r->tex[slot]);
        cui__vk_later_free(r, &gone);
        return 0;
    }
    if (!pixels && !cui__vk_cmd(r)) {   /* (stopped: no texture) */
        cui__vk_later gone = { 0 };
        gone.image = made.image; gone.view = made.view; gone.memory = made.memory; gone.set = made.set; gone.slot = slot;
        memset(&r->tex[slot], 0, sizeof r->tex[slot]);
        cui__vk_later_free(r, &gone);
        return 0;
    }
    if (!pixels) cui__vk_barrier(r, made.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return (cui_texture)(slot + 1);
}

static void cui__vk_tex_update(void *user, cui_texture tex, int x, int y, int w, int h, const void *pixels, int stride) {
    cui_gpu_renderer *r = user;
    int slot = (int)tex - 1;
    if (slot < 0 || slot >= r->tex_count || !r->tex[slot].image || w <= 0 || h <= 0 || !pixels || r->lost) return;
    cui__vk_tex_upload(r, &r->tex[slot], x, y, w, h, pixels, stride, false);
}

static void cui__vk_tex_destroy(void *user, cui_texture tex) {
    cui_gpu_renderer *r = user;
    int slot = (int)tex - 1;
    if (slot < 0 || slot >= r->tex_count || !r->tex[slot].image) return;
    /* draws already recorded may sample it: it goes, and its slot is free, once they have run */
    cui__vk_tex *t = &r->tex[slot];
    cui__vk_later l = { 0 };
    l.image = t->image; l.view = t->view; l.memory = t->memory; l.set = t->set; l.slot = slot;
    cui__vk_release_later(r, l);
    t->image = VK_NULL_HANDLE; t->view = VK_NULL_HANDLE; t->memory = VK_NULL_HANDLE; t->set = VK_NULL_HANDLE;
}

/* ---- the instance and the device ---- */

static bool cui__vk_instance(cui_gpu_renderer *r) {
    const char *exts[2] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_XLIB_SURFACE_EXTENSION_NAME };
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "cui";
    app.pEngineName = "cui";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo info = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = 2;
    info.ppEnabledExtensionNames = exts;
    return vkCreateInstance(&info, NULL, &r->instance) == VK_SUCCESS;
}

static VkResult cui__vk_surface(cui_gpu_renderer *r) {
    VkXlibSurfaceCreateInfoKHR info = { VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR };
    info.dpy = (Display *)r->xwin->display;
    info.window = (Window)r->xwin->window;
    return vkCreateXlibSurfaceKHR(r->instance, &info, NULL, &r->surface);
}

/* a device that draws to the surface with what the renderer needs: CUI_GPU's, else a discrete GPU, else any */
static bool cui__vk_pick(cui_gpu_renderer *r) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(r->instance, &n, NULL);
    VkPhysicalDevice *pds = calloc(n ? n : 1, sizeof *pds);
    if (!pds) return false;
    vkEnumeratePhysicalDevices(r->instance, &n, pds);
    const char *want = getenv("CUI_GPU");
    if (want && !*want) want = NULL;
    int best = -1, best_score = -1;
    uint32_t best_family = 0;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(pds[i], &props);
        if (props.apiVersion < VK_API_VERSION_1_3) continue;
        VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
        VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &f12 };
        VkPhysicalDeviceFeatures2 f = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f13 };
        vkGetPhysicalDeviceFeatures2(pds[i], &f);
        if (!f13.dynamicRendering || !f12.timelineSemaphore) continue;
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &qn, NULL);
        VkQueueFamilyProperties qs[16];
        if (qn > 16) qn = 16;
        vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &qn, qs);
        int family = -1;
        for (uint32_t q = 0; q < qn && family < 0; q++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(pds[i], q, r->surface, &present);
            if ((qs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) family = (int)q;
        }
        if (family < 0) continue;
        int score = want ? (cui__contains_ci(props.deviceName, want) ? 3 : 0)
                  : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1 : 0;
        if (score > best_score) { best = (int)i; best_score = score; best_family = (uint32_t)family; }
    }
    if (best >= 0) {
        r->physical = pds[best];
        r->family = best_family;
        if (want) {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(pds[best], &props);
            fprintf(stderr, "cui: GPU %s\n", props.deviceName);
        }
    }
    free(pds);
    return best >= 0;
}

static VkShaderModule cui__vk_module(cui_gpu_renderer *r, const uint32_t *code, size_t size) {
    VkShaderModuleCreateInfo info = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    info.codeSize = size;
    info.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(r->device, &info, NULL, &m);
    return m;
}

static bool cui__vk_pipeline(cui_gpu_renderer *r) {
    VkShaderModule vs = cui__vk_module(r, cui__vk_vs, sizeof cui__vk_vs), fs = cui__vk_module(r, cui__vk_fs, sizeof cui__vk_fs);
    if (vs && fs) {
        VkPipelineShaderStageCreateInfo stages[2] = {
            { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL },
            { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL },
        };
        /* one attribute per group of cui_vertex fields, as the OpenGL renderer's */
        VkVertexInputBindingDescription binding = { 0, sizeof(cui_vertex), VK_VERTEX_INPUT_RATE_VERTEX };
        VkVertexInputAttributeDescription attrs[] = {
            { 0, 0, VK_FORMAT_R32G32_SFLOAT,       (uint32_t)offsetof(cui_vertex, x) },
            { 1, 0, VK_FORMAT_R32G32_SFLOAT,       (uint32_t)offsetof(cui_vertex, u) },
            { 2, 0, VK_FORMAT_R32G32_SFLOAT,       (uint32_t)offsetof(cui_vertex, lx) },
            { 3, 0, VK_FORMAT_R32G32_SFLOAT,       (uint32_t)offsetof(cui_vertex, hw) },
            { 4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, (uint32_t)offsetof(cui_vertex, r_tl) },
            { 5, 0, VK_FORMAT_R32G32_SFLOAT,       (uint32_t)offsetof(cui_vertex, border) },
            { 6, 0, VK_FORMAT_R8G8B8A8_UNORM,      (uint32_t)offsetof(cui_vertex, color) },
            { 7, 0, VK_FORMAT_R8G8B8A8_UNORM,      (uint32_t)offsetof(cui_vertex, border_color) },
        };
        VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &binding;
        vi.vertexAttributeDescriptionCount = (uint32_t)(sizeof attrs / sizeof attrs[0]);
        vi.pVertexAttributeDescriptions = attrs;
        VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        /* premultiplied source over */
        VkPipelineColorBlendAttachmentState blend = { 0 };
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cb.attachmentCount = 1;
        cb.pAttachments = &blend;
        VkDynamicState dyn_states[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dyn = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dyn.dynamicStateCount = 2;
        dyn.pDynamicStates = dyn_states;
        VkPipelineRenderingCreateInfo rendering = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &r->format;
        VkGraphicsPipelineCreateInfo info = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        info.pNext = &rendering;
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &vi;
        info.pInputAssemblyState = &ia;
        info.pViewportState = &vp;
        info.pRasterizationState = &rs;
        info.pMultisampleState = &ms;
        info.pDepthStencilState = &ds;
        info.pColorBlendState = &cb;
        info.pDynamicState = &dyn;
        info.layout = r->layout;
        vkCreateGraphicsPipelines(r->device, VK_NULL_HANDLE, 1, &info, NULL, &r->pipeline);
    }
    if (vs) vkDestroyShaderModule(r->device, vs, NULL);
    if (fs) vkDestroyShaderModule(r->device, fs, NULL);
    return r->pipeline != VK_NULL_HANDLE;
}

static bool cui__vk_device(cui_gpu_renderer *r) {
    if (!cui__vk_pick(r)) { fprintf(stderr, "cui: no Vulkan 1.3 device draws to this window\n"); return false; }
    vkGetPhysicalDeviceMemoryProperties(r->physical, &r->mem);
    float priority = 1;
    VkDeviceQueueCreateInfo queue = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    queue.queueFamilyIndex = r->family;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    f12.timelineSemaphore = VK_TRUE;
    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &f12 };
    f13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceFeatures2 features = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f13 };
    const char *ext = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo info = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    info.pNext = &features;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue;
    info.enabledExtensionCount = 1;
    info.ppEnabledExtensionNames = &ext;
    if (vkCreateDevice(r->physical, &info, NULL, &r->device) != VK_SUCCESS) return false;
    VkDevice dev = r->device;
    vkGetDeviceQueue(dev, r->family, 0, &r->queue);

    VkSemaphoreTypeCreateInfo type = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo sem = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type };
    if (vkCreateSemaphore(dev, &sem, NULL, &r->timeline) != VK_SUCCESS) return false;
    VkCommandPoolCreateInfo pool = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = r->family;
    if (vkCreateCommandPool(dev, &pool, NULL, &r->pool) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo cba = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cba.commandPool = r->pool;
    cba.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cba.commandBufferCount = CUI__TV_FRAMES;
    if (vkAllocateCommandBuffers(dev, &cba, r->cmd) != VK_SUCCESS) return false;

    /* sampler: linear, clamp (textures have one level) */
    VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(dev, &si, NULL, &r->sampler) != VK_SUCCESS) return false;

    /* set 0: the command's texture with the sampler; the viewport is a push constant */
    VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, CUI__TV_TEX_CAP };
    VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pi.maxSets = CUI__TV_TEX_CAP;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &size;
    if (vkCreateDescriptorPool(dev, &pi, NULL, &r->set_pool) != VK_SUCCESS) return false;
    VkDescriptorSetLayoutBinding binding = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, &r->sampler };
    VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.bindingCount = 1;
    li.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(dev, &li, NULL, &r->set_layout) != VK_SUCCESS) return false;
    VkPushConstantRange push = { VK_SHADER_STAGE_VERTEX_BIT, 0, 2 * sizeof(float) };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &r->set_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(dev, &pli, NULL, &r->layout) != VK_SUCCESS) return false;

    /* a UNORM format (blending in sRGB): BGRA, else RGBA */
    VkSurfaceFormatKHR fmts[64];
    uint32_t n = 64;
    VkResult listed = vkGetPhysicalDeviceSurfaceFormatsKHR(r->physical, r->surface, &n, fmts);
    if (listed != VK_SUCCESS && listed != VK_INCOMPLETE) n = 0;
    for (uint32_t i = 0; i < n && !r->format; i++) if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) r->format = fmts[i].format;
    for (uint32_t i = 0; i < n && !r->format; i++) if (fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) r->format = fmts[i].format;
    if (!r->format || !cui__vk_pipeline(r)) return false;
    for (int i = 0; i < CUI__TV_FRAMES; i++) {
        VkSemaphoreCreateInfo s = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        if (vkCreateSemaphore(dev, &s, NULL, &r->acquire_sem[i]) != VK_SUCCESS) return false;
    }
    static const uint8_t black[4] = { 0, 0, 0, 255 };   /* what OpenGL samples from no texture */
    r->none = (int)cui__vk_tex_create(r, 1, 1, CUI_TEXTURE_RGBA8, black) - 1;
    cui__vk_idle(r);
    return r->none >= 0 && !r->lost;   /* (a renderer that stopped while starting is none: the app draws with OpenGL) */
}

/* ---- the window ---- */

/* The swapchain's views and its presentation semaphores: a semaphore a failed presentation left
 * signalled must not meet the next swapchain's images. The caller has made the device idle. */
static void cui__vk_views_free(cui_gpu_renderer *r) {
    for (uint32_t i = 0; i < r->image_count; i++) { vkDestroyImageView(r->device, r->views[i], NULL); r->views[i] = VK_NULL_HANDLE; }
    for (int i = 0; i < CUI__TV_MAX_IMAGES; i++) if (r->present_sem[i]) { vkDestroySemaphore(r->device, r->present_sem[i], NULL); r->present_sem[i] = VK_NULL_HANDLE; }
    r->image_count = 0;
}

/* (re)makes the swapchain; the caller has made the device idle */
static bool cui__vk_swapchain(cui_gpu_renderer *r) {
    VkDevice dev = r->device;
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(r->physical, r->surface, &caps) != VK_SUCCESS) return false;
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {   /* the surface leaves the size to the app */
        extent.width = (uint32_t)(r->xwin->width > 0 ? r->xwin->width : 0);
        extent.height = (uint32_t)(r->xwin->height > 0 ? r->xwin->height : 0);
    }
    if (extent.width < caps.minImageExtent.width) extent.width = caps.minImageExtent.width;
    if (extent.height < caps.minImageExtent.height) extent.height = caps.minImageExtent.height;
    if (extent.width > caps.maxImageExtent.width) extent.width = caps.maxImageExtent.width;
    if (extent.height > caps.maxImageExtent.height) extent.height = caps.maxImageExtent.height;
    if (extent.width == 0 || extent.height == 0) return false;   /* minimized */
    /* vsync: FIFO; else tearing (immediate), or mailbox */
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!r->vsync) {
        VkPresentModeKHR modes[16];
        uint32_t n = 16;
        vkGetPhysicalDeviceSurfacePresentModesKHR(r->physical, r->surface, &n, modes);
        for (uint32_t i = 0; i < n; i++) {
            if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
            if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR && mode == VK_PRESENT_MODE_FIFO_KHR) mode = VK_PRESENT_MODE_MAILBOX_KHR;
        }
    }
    uint32_t count = caps.minImageCount > CUI__TV_IMAGES ? caps.minImageCount : CUI__TV_IMAGES;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    /* reading a frame back (screenshots) needs the images as a copy's source, which a surface may not allow */
    r->can_capture = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha)) alpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha)) alpha = (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha & (~caps.supportedCompositeAlpha + 1));
    VkSwapchainCreateInfoKHR info = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    info.surface = r->surface;
    info.minImageCount = count;
    info.imageFormat = r->format;
    info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (r->can_capture ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = alpha;
    info.presentMode = mode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = r->swapchain;
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    if (vkCreateSwapchainKHR(dev, &info, NULL, &sc) != VK_SUCCESS) {
        /* the old one is retired even so, and may not be handed in again: the next try starts from none */
        cui__vk_views_free(r);
        if (r->swapchain) vkDestroySwapchainKHR(dev, r->swapchain, NULL);
        r->swapchain = VK_NULL_HANDLE;
        r->extent.width = r->extent.height = 0;
        r->stale = true;
        return false;
    }
    cui__vk_views_free(r);
    if (r->swapchain) vkDestroySwapchainKHR(dev, r->swapchain, NULL);
    r->swapchain = sc;
    r->extent = extent;
    r->image_count = 0;
    uint32_t n = 0;
    vkGetSwapchainImagesKHR(dev, sc, &n, NULL);
    /* (a driver may make more images than asked for; more than there is room for cannot be drawn into) */
    if (n == 0 || n > CUI__TV_MAX_IMAGES || vkGetSwapchainImagesKHR(dev, sc, &n, r->images) != VK_SUCCESS) { r->stale = true; return false; }
    bool made = true;
    for (uint32_t i = 0; i < n; i++) {
        VkImageViewCreateInfo v = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        v.image = r->images[i];
        v.viewType = VK_IMAGE_VIEW_TYPE_2D;
        v.format = r->format;
        v.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        v.subresourceRange.levelCount = 1;
        v.subresourceRange.layerCount = 1;
        if (vkCreateImageView(dev, &v, NULL, &r->views[i]) != VK_SUCCESS) made = false;
        if (!r->present_sem[i]) {
            VkSemaphoreCreateInfo s = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            if (vkCreateSemaphore(dev, &s, NULL, &r->present_sem[i]) != VK_SUCCESS) made = false;
        }
    }
    r->image_count = n;
    r->stale = !made;
    return made;
}

void cui_gpu_destroy(cui_gpu_renderer *r) {
    if (!r) return;
    VkDevice dev = r->device;
    if (dev) {
        if (r->timeline) cui__vk_idle(r);
        if (!cui__vk_idle_or_gone(r)) {   /* (presents too: their semaphores go below) */
            /* Not known to be done with anything: what the device may still use is left alone,
             * and only the memory on this side is given back. */
            free(r->later);
            free(r->tex);
            free(r);
            return;
        }
        cui__vk_views_free(r);
        if (r->swapchain) vkDestroySwapchainKHR(dev, r->swapchain, NULL);
        for (int i = 0; i < CUI__TV_MAX_IMAGES; i++) if (r->present_sem[i]) vkDestroySemaphore(dev, r->present_sem[i], NULL);
        for (int i = 0; i < CUI__TV_FRAMES; i++) if (r->acquire_sem[i]) vkDestroySemaphore(dev, r->acquire_sem[i], NULL);
        for (int i = 0; i < r->tex_count; i++) cui__vk_tex_destroy(r, (cui_texture)(i + 1));
        cui__vk_collect(r, true);
        if (r->pipeline) vkDestroyPipeline(dev, r->pipeline, NULL);
        if (r->layout) vkDestroyPipelineLayout(dev, r->layout, NULL);
        if (r->set_layout) vkDestroyDescriptorSetLayout(dev, r->set_layout, NULL);
        if (r->set_pool) vkDestroyDescriptorPool(dev, r->set_pool, NULL);
        if (r->sampler) vkDestroySampler(dev, r->sampler, NULL);
        for (int i = 0; i < CUI__TV_FRAMES; i++) cui__vk_buffer_free(r, &r->upload[i]);
        cui__vk_buffer_free(r, &r->kept);
        if (r->pool) vkDestroyCommandPool(dev, r->pool, NULL);
        if (r->timeline) vkDestroySemaphore(dev, r->timeline, NULL);
    }
    if (r->surface) vkDestroySurfaceKHR(r->instance, r->surface, NULL);
    if (dev) vkDestroyDevice(dev, NULL);
    if (r->instance) vkDestroyInstance(r->instance, NULL);
    free(r->later);
    free(r->tex);
    free(r);
}

cui_gpu_renderer *cui_gpu_create(void *window, bool vsync) {
    if (!window) return NULL;
    cui_gpu_renderer *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->xwin = window;
    r->vsync = vsync;
    if (!cui__vk_instance(r)) { cui_gpu_destroy(r); return NULL; }
    if (cui__vk_surface(r) != VK_SUCCESS) { r->surface = VK_NULL_HANDLE; cui_gpu_destroy(r); return NULL; }
    if (!cui__vk_device(r)) { cui_gpu_destroy(r); return NULL; }
    /* The swapchain now, while the app can still draw with OpenGL instead. (A window without a
     * size yet has none to make: it comes with the first frame.) */
    r->stale = true;
    if (r->xwin->width > 0 && r->xwin->height > 0 && !cui__vk_swapchain(r)) { cui_gpu_destroy(r); return NULL; }
    return r;
}

cui_renderer_iface cui_gpu_iface(cui_gpu_renderer *r) {
    cui_renderer_iface i;
    memset(&i, 0, sizeof i);
    if (!r) return i;
    i.user = r;
    i.texture_create = cui__vk_tex_create;
    i.texture_update = cui__vk_tex_update;
    i.texture_destroy = cui__vk_tex_destroy;
    return i;
}

/* ---- frames ---- */

static bool cui__vk_resized(const cui_gpu_renderer *r) {
    int w = r->xwin->width, h = r->xwin->height;
    return r->stale || (w > 0 && h > 0 && ((uint32_t)w != r->extent.width || (uint32_t)h != r->extent.height));
}

/* What one frame's phases share */
typedef struct cui__vk_frame {
    VkCommandBuffer cmd;
    VkSemaphore acquire;
    VkImage image;
    cui__vk_buffer readback;   /* the frame copied out, when it was asked for */
} cui__vk_frame;

/* The image to draw into; the swapchain is made again when the window's size changed. */
static bool cui__vk_acquire(cui_gpu_renderer *r, cui__vk_frame *f) {
    VkDevice dev = r->device;
    f->cmd = cui__vk_cmd(r);
    if (!f->cmd) return false;
    f->acquire = r->acquire_sem[r->frame];
    bool acquired = false;
    for (int attempt = 0; attempt < 2 && !acquired; attempt++) {
        if (cui__vk_resized(r)) {
            cui__vk_idle(r);
            if (r->lost || vkDeviceWaitIdle(dev) != VK_SUCCESS) {   /* (not known to be done with the old images: they stay) */
                r->lost = true;
                return false;
            }
            if (!cui__vk_swapchain(r)) break;
            f->cmd = cui__vk_cmd(r);
            if (!f->cmd) return false;
            f->acquire = r->acquire_sem[r->frame];
        }
        VkResult res = vkAcquireNextImageKHR(dev, r->swapchain, UINT64_MAX, f->acquire, VK_NULL_HANDLE, &r->image);
        if (res == VK_SUCCESS || res == VK_SUBOPTIMAL_KHR) { acquired = true; r->stale = res == VK_SUBOPTIMAL_KHR; }
        else r->stale = true;
    }
    if (!acquired) {   /* (minimized) what is recorded still goes */
        cui__vk_submit(r, VK_NULL_HANDLE, VK_NULL_HANDLE);
        return false;
    }
    f->image = r->images[r->image];
    return true;
}

/* Into the swapchain image: clear, then a draw per command (cui_gl_render's counterpart). */
static bool cui__vk_draw(cui_gpu_renderer *r, const cui__vk_frame *f, const cui_draw_data *dd) {
    VkCommandBuffer cmd = f->cmd;
    /* the frame's vertices and indices */
    size_t vbytes = sizeof(cui_vertex) * (size_t)dd->vertex_count, ibytes = sizeof(uint32_t) * (size_t)dd->index_count;
    VkBuffer vbuf = VK_NULL_HANDLE, ibuf = VK_NULL_HANDLE;
    size_t vat = 0, iat = 0;
    uint8_t *v = dd->index_count ? cui__vk_upload(r, vbytes, 16, &vbuf, &vat) : NULL;
    uint8_t *ix = v ? cui__vk_upload(r, ibytes, 16, &ibuf, &iat) : NULL;   /* (a buffer outgrown between the two lives until this frame has run) */
    bool drawn = !dd->index_count || (v && ix);

    cui__vk_barrier(r, f->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo color = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    color.imageView = r->views[r->image];
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color.float32[0] = dd->clear_color.r / 255.0f;
    color.clearValue.color.float32[1] = dd->clear_color.g / 255.0f;
    color.clearValue.color.float32[2] = dd->clear_color.b / 255.0f;
    color.clearValue.color.float32[3] = 1;
    VkRenderingInfo rendering = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    rendering.renderArea.extent = r->extent;
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &rendering);
    if (v && ix) {
        memcpy(v, dd->vertices, vbytes);
        memcpy(ix, dd->indices, ibytes);
        int W = (int)r->extent.width, H = (int)r->extent.height;
        VkViewport vp = { 0, 0, (float)dd->width_px, (float)dd->height_px, 0, 1 };
        float viewport[2] = { (float)dd->width_px, (float)dd->height_px };
        VkDeviceSize voffset = vat;
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipeline);
        vkCmdBindVertexBuffers(cmd, 0, 1, &vbuf, &voffset);
        vkCmdBindIndexBuffer(cmd, ibuf, iat, VK_INDEX_TYPE_UINT32);
        vkCmdPushConstants(cmd, r->layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof viewport, viewport);
        int bound = -1;
        for (int i = 0; i < dd->command_count; i++) {
            const cui_draw_cmd *c = &dd->commands[i];
            if (!c->index_count) continue;
            int x0 = (int)floorf(c->clip.x), y0 = (int)floorf(c->clip.y);
            int x1 = (int)ceilf(c->clip.x + c->clip.w), y1 = (int)ceilf(c->clip.y + c->clip.h);
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > W) x1 = W;
            if (y1 > H) y1 = H;
            if (x1 <= x0 || y1 <= y0) continue;
            VkRect2D sc = { { x0, y0 }, { (uint32_t)(x1 - x0), (uint32_t)(y1 - y0) } };
            vkCmdSetScissor(cmd, 0, 1, &sc);
            cui_texture t = c->texture ? c->texture : dd->font_atlas;
            int slot = (int)t - 1;
            if (slot < 0 || slot >= r->tex_count || !r->tex[slot].image) slot = r->none;
            if (slot != bound) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->layout, 0, 1, &r->tex[slot].set, 0, NULL);
                bound = slot;
            }
            vkCmdDrawIndexed(cmd, (uint32_t)c->index_count, 1, (uint32_t)c->index_offset, 0, 0);
        }
    }
    vkCmdEndRendering(cmd);
    return drawn;   /* false: the vertices found no room, and only the cleared window is shown */
}

/* The frame copied out, if asked, and the image made ready to present. */
static void cui__vk_copy_out(cui_gpu_renderer *r, cui__vk_frame *f, bool wanted) {
    if (wanted && r->can_capture && cui__vk_buffer_make(r, &f->readback, (size_t)r->extent.width * r->extent.height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
        cui__vk_barrier(r, f->image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy region = { 0 };
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent.width = r->extent.width;
        region.imageExtent.height = r->extent.height;
        region.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(f->cmd, f->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, f->readback.buffer, 1, &region);
        /* waiting for the submission orders the read after the copy, and this makes what the copy wrote the host's to read */
        VkBufferMemoryBarrier host = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host.buffer = f->readback.buffer;
        host.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(f->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &host, 0, NULL);
        cui__vk_barrier(r, f->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    } else {
        cui__vk_barrier(r, f->image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    }
}

static void cui__vk_present(cui_gpu_renderer *r, const cui__vk_frame *f) {
    VkSemaphore present = r->present_sem[r->image];
    cui__vk_submit(r, f->acquire, present);
    if (r->lost) return;   /* (nothing was submitted: there is nothing to present) */
    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &present;
    pi.swapchainCount = 1;
    pi.pSwapchains = &r->swapchain;
    pi.pImageIndices = &r->image;
    if (vkQueuePresentKHR(r->queue, &pi) != VK_SUCCESS) r->stale = true;
}

/* Waits for the copy and writes it as RGBA; false if one was asked for and could not be made. */
static bool cui__vk_read_back(cui_gpu_renderer *r, cui__vk_frame *f, const cui_draw_data *dd, void *rgba) {
    bool ok = !rgba;
    if (f->readback.buffer) {
        if (!cui__vk_wait(r, r->value)) {   /* the copy may still be writing it: it goes when the renderer does */
            r->kept = f->readback;          /* (a failed wait stops the renderer, so there is only ever one) */
            memset(&f->readback, 0, sizeof f->readback);
            return false;
        }
        int w = (int)r->extent.width, h = (int)r->extent.height;
        if (!r->lost && w == dd->width_px && h == dd->height_px) {
            const uint8_t *src = f->readback.ptr;
            uint8_t *dst = rgba;
            bool bgra = r->format == VK_FORMAT_B8G8R8A8_UNORM;
            for (size_t i = 0, n = (size_t)w * (size_t)h; i < n; i++, src += 4, dst += 4) {
                dst[0] = src[bgra ? 2 : 0]; dst[1] = src[1]; dst[2] = src[bgra ? 0 : 2]; dst[3] = 255;
            }
            ok = true;
        }
        cui__vk_buffer_free(r, &f->readback);
    }
    return ok;
}

static bool cui__vk_render(cui_gpu_renderer *r, const cui_draw_data *dd, void *rgba) {
    if (!r || !dd || r->lost) return false;
    cui__vk_frame f = { 0 };
    if (!cui__vk_acquire(r, &f)) return false;
    bool drawn = cui__vk_draw(r, &f, dd);
    cui__vk_copy_out(r, &f, rgba != NULL);
    cui__vk_present(r, &f);
    return cui__vk_read_back(r, &f, dd, rgba) && drawn;   /* (a capture of a frame that was not drawn is none) */
}

void cui_gpu_render(cui_gpu_renderer *r, const cui_draw_data *dd) { cui__vk_render(r, dd, NULL); }
bool cui_gpu_render_capture(cui_gpu_renderer *r, const cui_draw_data *dd, void *rgba) { return rgba && cui__vk_render(r, dd, rgba); }

#endif
