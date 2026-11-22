/* Direct3D 12 renderer (cui_gpu_*, built with CUI_D3D12): the draw data
 * the OpenGL renderer takes (cui_draw_data's vertices, indices and commands), drawn the same
 * way. One pipeline; its pixel shader is cui_render_gl.c's fragment shader in HLSL, branching
 * on the vertex's mode. Keep the two in step.
 *
 * - A frame's vertices and indices are copied into an upload buffer (one per command
 *   allocator, reused once that allocator's submission is done), then a draw per command with
 *   its clip rectangle as the scissor and its texture's descriptor.
 * - Drawn straight into a UNORM back buffer of a flip-model swapchain, premultiplied source
 *   over, as the OpenGL renderer blends.
 * - Paced by a waitable swapchain (3 buffers, at most 2 frames queued): after presenting, the
 *   frame waits until a buffer is free. */
#include "../cui_internal.h"
#include "cui/cui_render.h"

#if defined(CUI_D3D12) && defined(_WIN32)

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CUI__T12_FRAMES 2           /* command allocators (and upload buffers) in flight */
#define CUI__T12_BUFFERS 3          /* back buffers */
#define CUI__T12_TEX_CAP 4096       /* textures alive at once */

static const char cui__d3d12_hlsl[] =
"cbuffer Globals : register(b0) { float2 u_viewport; };\n"
"Texture2D u_tex : register(t0);\n"
"SamplerState u_smp : register(s0);\n"
"struct VI {\n"
"  float2 pos : POSITION; float2 uv : TEXCOORD0; float2 local : TEXCOORD1; float2 half_ : TEXCOORD2;\n"
"  float4 radii : TEXCOORD3; float2 params : TEXCOORD4; float4 color : COLOR0; float4 bcolor : COLOR1;\n"
"};\n"
"struct VO {\n"
"  float4 pos : SV_Position; float2 uv : TEXCOORD0; float2 local : TEXCOORD1;\n"
"  nointerpolation float2 half_ : TEXCOORD2; nointerpolation float4 radii : TEXCOORD3;\n"
"  nointerpolation float2 params : TEXCOORD4; float4 color : COLOR0; nointerpolation float4 bcolor : COLOR1;\n"
"};\n"
"VO vs_main(VI i) {\n"
"  VO o;\n"
"  o.uv = i.uv; o.local = i.local; o.half_ = i.half_; o.radii = i.radii;\n"
"  o.params = i.params; o.color = i.color; o.bcolor = i.bcolor;\n"
"  o.pos = float4(i.pos.x / u_viewport.x * 2.0 - 1.0, 1.0 - i.pos.y / u_viewport.y * 2.0, 0.0, 1.0);\n"
"  return o;\n"
"}\n"
"float sd_rrect(float2 p, float2 b, float4 r) {\n"
"  float rr = p.x < 0.0 ? (p.y < 0.0 ? r.x : r.w) : (p.y < 0.0 ? r.y : r.z);\n"
"  rr = min(rr, min(b.x, b.y));\n"
"  float2 q = abs(p) - b + rr;\n"
"  return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - rr;\n"
"}\n"
"float erf_(float x) {\n"
"  float s = sign(x); x = abs(x);\n"
"  float t = 1.0 / (1.0 + 0.3275911 * x);\n"
"  float y = 1.0 - (((((1.061405429 * t - 1.453152027) * t) + 1.421413741) * t - 0.284496736) * t + 0.254829592) * t * exp(-x * x);\n"
"  return s * y;\n"
"}\n"
"float bayer8(float2 p) {\n"
"  float2 f = floor(p);\n"
"  int2 q = int2(f - 8.0 * floor(f / 8.0));\n"
"  int a = q.x ^ q.y, y = q.y;\n"
"  int m = ((a & 1) << 5) | ((y & 1) << 4) | ((a & 2) << 2) | ((y & 2) << 1) | ((a & 4) >> 1) | ((y & 4) >> 2);\n"
"  return (float(m) + 0.5) / 64.0;\n"
"}\n"
"float4 ps_main(VO i) : SV_Target {\n"
"  int mode = int(i.params.y + 0.5);\n"
"  float4 c = i.color;\n"
"  if (mode == 4) return float4(c.rgb * c.a, c.a);\n"
"  float4 t = u_tex.SampleLevel(u_smp, i.uv, 0);\n"   /* (textures have one level) */
"  if (mode == 0) { float a = c.a * t.r; return float4(c.rgb * a, a); }\n"
"  float d = sd_rrect(i.local, i.half_, i.radii);\n"
"  if (mode == 1) {\n"
"    float outer = clamp(0.5 - d, 0.0, 1.0);\n"
"    float bw = i.params.x;\n"
"    float inner = bw > 0.0 ? clamp(0.5 - (d + bw), 0.0, 1.0) : outer;\n"
"    float fa = c.a * inner, ba = i.bcolor.a * (outer - inner);\n"
"    return float4(c.rgb * fa + i.bcolor.rgb * ba, fa + ba);\n"
"  } else if (mode == 2) {\n"
"    float s = max(i.params.x, 0.01);\n"
"    float a = c.a * 0.5 * (1.0 - erf_(d / (s * 1.41421356)));\n"
"    return float4(c.rgb * a, a);\n"
"  } else if (mode == 5) {\n"
"    float th = bayer8(i.uv);\n"
"    float k = th >= i.params.x && th < c.a ? i.bcolor.a * clamp(0.5 - d, 0.0, 1.0) : 0.0;\n"
"    return float4(i.bcolor.rgb * k, k);\n"
"  }\n"
"  float cov = clamp(0.5 - d, 0.0, 1.0);\n"
"  float k = c.a * cov;\n"
"  return float4(t.rgb * t.a * c.rgb * k, t.a * k);\n"
"}\n";

typedef struct cui__d3d12_buffer {      /* a mapped buffer */
    ID3D12Resource *res;
    uint8_t *ptr;
    D3D12_GPU_VIRTUAL_ADDRESS va;
    size_t cap, used;
} cui__d3d12_buffer;

typedef struct cui__d3d12_tex { ID3D12Resource *res; int bpp; DXGI_FORMAT fmt; } cui__d3d12_tex;
typedef struct cui__d3d12_later { IUnknown *obj; uint64_t fence; int slot; } cui__d3d12_later;

struct cui_gpu_renderer {
    HWND hwnd;
    bool vsync, tearing;
    IDXGIFactory4 *factory;
    ID3D12Device *device;
    ID3D12CommandQueue *queue;
    ID3D12CommandAllocator *alloc[CUI__T12_FRAMES];
    uint64_t alloc_fence[CUI__T12_FRAMES];
    cui__d3d12_buffer upload[CUI__T12_FRAMES];
    ID3D12GraphicsCommandList *list;
    bool open;
    int frame;                        /* the allocator being recorded */
    ID3D12Fence *fence;
    uint64_t fence_value;             /* the last value the queue was told to signal: everything waited for is at or below it */
    HANDLE event;
    ID3D12DescriptorHeap *srv_heap, *rtv_heap;
    D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu, rtv_cpu;
    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu;
    UINT srv_inc, rtv_inc;
    ID3D12RootSignature *root;
    ID3D12PipelineState *pso;
    cui__d3d12_tex *tex;                /* by cui_texture - 1; the descriptor at the same index */
    int tex_count, tex_cap;
    int none;                         /* a black texel, for a command whose texture is gone */
    cui__d3d12_later *later; int later_count, later_cap;   /* released once their fence passes */
    IDXGISwapChain3 *swapchain;
    UINT flags;
    HANDLE waitable;
    ID3D12Resource *buffers[CUI__T12_BUFFERS];
    int w, h;
    bool lost;                        /* the device was removed (a driver update, a GPU reset): nothing more is drawn */
};

#define CUI__REL(p) do { if (p) { (p)->lpVtbl->Release(p); (p) = NULL; } } while (0)

/* ---- submissions ---- */

/* The device is gone (a driver update, a GPU reset, the adapter taken away) or a call it cannot
 * go on without failed. Nothing recreates it: the window keeps its last picture, and the calls
 * that would use the device are not made. */
static void cui__d3d12_lose(cui_gpu_renderer *r) {
    if (!r->lost) fprintf(stderr, "cui: Direct3D 12 stopped (the device was lost, or it ran out of memory); the window is not drawn any more\n");
    r->lost = true;
}

static void cui__d3d12_wait(cui_gpu_renderer *r, uint64_t value) {
    if (value == 0 || r->fence->lpVtbl->GetCompletedValue(r->fence) >= value) return;
    if (SUCCEEDED(r->fence->lpVtbl->SetEventOnCompletion(r->fence, value, r->event)) && WaitForSingleObject(r->event, INFINITE) == WAIT_OBJECT_0) return;
    /* the event was not set up: look at the fence itself (a removed device completes every value) */
    while (r->fence->lpVtbl->GetCompletedValue(r->fence) < value) Sleep(1);
}

/* the objects whose fence has passed (all of them when idle); their texture slots are free again */
static void cui__d3d12_collect(cui_gpu_renderer *r, bool all) {
    uint64_t done = r->fence->lpVtbl->GetCompletedValue(r->fence);
    int n = 0;
    for (int i = 0; i < r->later_count; i++) {
        cui__d3d12_later *l = &r->later[i];
        if (all || l->fence <= done) {
            if (l->obj) l->obj->lpVtbl->Release(l->obj);
            if (l->slot >= 0) r->tex[l->slot].bpp = 0;
        } else {
            r->later[n++] = *l;
        }
    }
    r->later_count = n;
}

/* releases obj (and frees the texture slot) after what is recorded so far has run */
static void cui__d3d12_release_later(cui_gpu_renderer *r, IUnknown *obj, int slot) {
    if (r->later_count == r->later_cap) {
        int cap = r->later_cap ? r->later_cap * 2 : 64;
        cui__d3d12_later *grown = realloc(r->later, (size_t)cap * sizeof *grown);
        if (!grown) return;   /* out of memory: commands recorded and not yet run may use it, so it is left alive */
        r->later = grown;
        r->later_cap = cap;
    }
    cui__d3d12_later l = { obj, r->fence_value + 1, slot };
    r->later[r->later_count++] = l;
}

/* The open command list, opened on the next allocator once its last submission is done. NULL
 * when the renderer has stopped or the list will not open: the caller records nothing. */
static ID3D12GraphicsCommandList *cui__d3d12_cmd(cui_gpu_renderer *r) {
    if (r->lost) return NULL;
    if (!r->open) {
        int f = r->frame;
        cui__d3d12_wait(r, r->alloc_fence[f]);
        if (FAILED(r->alloc[f]->lpVtbl->Reset(r->alloc[f])) || FAILED(r->list->lpVtbl->Reset(r->list, r->alloc[f], NULL))) {
            cui__d3d12_lose(r);
            return NULL;
        }
        r->upload[f].used = 0;
        r->open = true;
    }
    return r->list;
}

static void cui__d3d12_submit(cui_gpu_renderer *r) {
    if (!r->open) return;
    ID3D12GraphicsCommandList *list = r->list;
    if (r->lost || FAILED(list->lpVtbl->Close(list))) {   /* nothing of it runs, and nothing waits for it */
        cui__d3d12_lose(r);
        r->open = false;
        return;
    }
    r->queue->lpVtbl->ExecuteCommandLists(r->queue, 1, (ID3D12CommandList **)&list);
    r->open = false;
    if (FAILED(r->queue->lpVtbl->Signal(r->queue, r->fence, r->fence_value + 1))) {   /* a value the fence would never reach is not waited for */
        cui__d3d12_lose(r);
        return;
    }
    r->alloc_fence[r->frame] = ++r->fence_value;
    r->frame = (r->frame + 1) % CUI__T12_FRAMES;
    r->open = false;
    cui__d3d12_collect(r, false);
}

static void cui__d3d12_idle(cui_gpu_renderer *r) {
    cui__d3d12_submit(r);
    cui__d3d12_wait(r, r->fence_value);
}

/* Texture work is recorded into the open list and runs with the next frame. A window that
 * shows no frames (minimized) would let it pile up without end: past this much, it is run now.
 * Only between texture calls, never while a frame is recorded. */
static void cui__d3d12_flush_backlog(cui_gpu_renderer *r) {
    bool uploads = r->open && r->upload[r->frame].used > ((size_t)16 << 20);
    if (r->lost || (!uploads && r->later_count < 256)) return;
    cui__d3d12_cmd(r);
    cui__d3d12_submit(r);
}

static void cui__d3d12_barrier(cui_gpu_renderer *r, ID3D12Resource *res, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b = { 0 };
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    ID3D12GraphicsCommandList *list = cui__d3d12_cmd(r);
    if (list) list->lpVtbl->ResourceBarrier(list, 1, &b);
}

/* a mapped buffer of `cap` bytes in a heap of that type */
static bool cui__d3d12_buffer_make(cui_gpu_renderer *r, cui__d3d12_buffer *b, size_t cap, D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES heap = { type };
    D3D12_RESOURCE_DESC desc = { 0 };
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = cap;
    desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_RESOURCE_STATES state = type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST;
    cui__d3d12_buffer made = { 0 };
    if (FAILED(r->device->lpVtbl->CreateCommittedResource(r->device, &heap, D3D12_HEAP_FLAG_NONE, &desc, state, NULL, &IID_ID3D12Resource, (void **)&made.res)))
        return false;
    D3D12_RANGE none = { 0, 0 };
    if (FAILED(made.res->lpVtbl->Map(made.res, 0, type == D3D12_HEAP_TYPE_READBACK ? NULL : &none, (void **)&made.ptr))) { CUI__REL(made.res); return false; }
    made.va = made.res->lpVtbl->GetGPUVirtualAddress(made.res);
    made.cap = cap;
    *b = made;
    return true;
}

/* room in the open list's upload buffer, valid until its submission is done */
static uint8_t *cui__d3d12_upload(cui_gpu_renderer *r, size_t bytes, size_t align, D3D12_GPU_VIRTUAL_ADDRESS *va, ID3D12Resource **res, size_t *offset) {
    if (!cui__d3d12_cmd(r)) return NULL;
    cui__d3d12_buffer *u = &r->upload[r->frame];
    size_t at = (u->used + align - 1) & ~(align - 1);
    if (!u->res || at + bytes > u->cap) {
        size_t cap = u->cap ? u->cap * 2 : (size_t)1 << 20;
        while (cap < bytes) cap *= 2;
        cui__d3d12_buffer grown;
        if (!cui__d3d12_buffer_make(r, &grown, cap, D3D12_HEAP_TYPE_UPLOAD)) return NULL;
        if (u->res) cui__d3d12_release_later(r, (IUnknown *)u->res, -1);   /* what the list already uses from it stays */
        *u = grown;
        at = 0;
    }
    u->used = at + bytes;
    if (va) *va = u->va + at;
    if (res) *res = u->res;
    if (offset) *offset = at;
    return u->ptr + at;
}

static D3D12_CPU_DESCRIPTOR_HANDLE cui__d3d12_rtv_at(cui_gpu_renderer *r, int i) { D3D12_CPU_DESCRIPTOR_HANDLE h = { r->rtv_cpu.ptr + (SIZE_T)i * r->rtv_inc }; return h; }

/* ---- textures: R8 for the glyph atlas, RGBA8 for images ---- */

static bool cui__d3d12_tex_upload(cui_gpu_renderer *r, cui__d3d12_tex *t, int x, int y, int w, int h, const void *pixels, int stride, bool fresh) {
    UINT pitch = (UINT)((w * t->bpp + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1));
    ID3D12Resource *src = NULL;
    size_t offset = 0;
    uint8_t *p = cui__d3d12_upload(r, (size_t)pitch * (size_t)h, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, NULL, &src, &offset);
    if (!p) return false;   /* no memory for the pixels */
    size_t row = (size_t)w * (size_t)t->bpp, from = stride ? (size_t)stride : row;
    for (int k = 0; k < h; k++) memcpy(p + (size_t)k * pitch, (const uint8_t *)pixels + (size_t)k * from, row);
    if (!fresh) cui__d3d12_barrier(r, t->res, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst = { 0 }, from_loc = { 0 };
    dst.pResource = t->res;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from_loc.pResource = src;
    from_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from_loc.PlacedFootprint.Offset = offset;
    from_loc.PlacedFootprint.Footprint.Format = t->fmt;
    from_loc.PlacedFootprint.Footprint.Width = (UINT)w;
    from_loc.PlacedFootprint.Footprint.Height = (UINT)h;
    from_loc.PlacedFootprint.Footprint.Depth = 1;
    from_loc.PlacedFootprint.Footprint.RowPitch = pitch;
    r->list->lpVtbl->CopyTextureRegion(r->list, &dst, (UINT)x, (UINT)y, 0, &from_loc, NULL);
    cui__d3d12_barrier(r, t->res, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return true;
}

static cui_texture cui__d3d12_tex_create(void *user, int w, int h, cui_texture_format fmt, const void *pixels) {
    cui_gpu_renderer *r = user;
    if (w <= 0 || h <= 0 || r->lost) return 0;
    cui__d3d12_flush_backlog(r);
    int slot = -1;
    for (int i = 0; i < r->tex_count; i++) if (!r->tex[i].bpp) { slot = i; break; }
    if (slot < 0) {
        if (r->tex_count == CUI__T12_TEX_CAP) return 0;
        if (r->tex_count == r->tex_cap) {
            int cap = r->tex_cap ? r->tex_cap * 2 : 16;
            void *n = realloc(r->tex, sizeof(*r->tex) * (size_t)cap);
            if (!n) return 0;
            r->tex = n;
            r->tex_cap = cap;
        }
        slot = r->tex_count++;
        memset(&r->tex[slot], 0, sizeof r->tex[slot]);
    }
    cui__d3d12_tex *t = &r->tex[slot];
    t->fmt = fmt == CUI_TEXTURE_R8 ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC desc = { 0 };
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = (UINT64)w; desc.Height = (UINT)h;
    desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.Format = t->fmt;
    desc.SampleDesc.Count = 1;
    if (FAILED(r->device->lpVtbl->CreateCommittedResource(r->device, &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                                                          &IID_ID3D12Resource, (void **)&t->res))) { t->res = NULL; return 0; }
    t->bpp = fmt == CUI_TEXTURE_R8 ? 1 : 4;
    D3D12_CPU_DESCRIPTOR_HANDLE at = { r->srv_cpu.ptr + (SIZE_T)slot * r->srv_inc };
    r->device->lpVtbl->CreateShaderResourceView(r->device, t->res, NULL, at);
    if (pixels && !cui__d3d12_tex_upload(r, t, 0, 0, w, h, pixels, 0, true)) {
        /* no texture without its pixels: the caller is told, and can make it again (no command uses it yet) */
        CUI__REL(t->res);
        t->bpp = 0;
        return 0;
    }
    if (!pixels && !cui__d3d12_cmd(r)) {   /* (stopped: no texture) */
        CUI__REL(t->res);
        t->bpp = 0;
        return 0;
    }
    if (!pixels) cui__d3d12_barrier(r, t->res, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return (cui_texture)(slot + 1);
}

static void cui__d3d12_tex_update(void *user, cui_texture tex, int x, int y, int w, int h, const void *pixels, int stride) {
    cui_gpu_renderer *r = user;
    int slot = (int)tex - 1;
    if (slot < 0 || slot >= r->tex_count || !r->tex[slot].res || w <= 0 || h <= 0 || !pixels || r->lost) return;
    cui__d3d12_flush_backlog(r);
    cui__d3d12_tex_upload(r, &r->tex[slot], x, y, w, h, pixels, stride, false);
}

static void cui__d3d12_tex_destroy(void *user, cui_texture tex) {
    cui_gpu_renderer *r = user;
    int slot = (int)tex - 1;
    if (slot < 0 || slot >= r->tex_count || !r->tex[slot].res) return;
    cui__d3d12_flush_backlog(r);
    /* draws already recorded may sample it: it goes, and its slot is free, once they have run */
    cui__d3d12_release_later(r, (IUnknown *)r->tex[slot].res, slot);
    r->tex[slot].res = NULL;
}

/* ---- the device ---- */

static ID3DBlob *cui__d3d12_compile(const char *entry, const char *target) {
    ID3DBlob *blob = NULL, *errors = NULL;
    if (FAILED(D3DCompile(cui__d3d12_hlsl, sizeof cui__d3d12_hlsl - 1, "cui_d3d12", NULL, NULL, entry, target,
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors)) && errors)
        fprintf(stderr, "cui: %s\n", (const char *)errors->lpVtbl->GetBufferPointer(errors));
    CUI__REL(errors);
    return blob;
}

static bool cui__d3d12_pipeline(cui_gpu_renderer *r) {
    ID3D12Device *dev = r->device;
    /* b0 root constants (the viewport), t0 a table of one texture, s0 static (linear, clamp) */
    D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
    D3D12_ROOT_PARAMETER params[2];
    memset(params, 0, sizeof params);
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = 4;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler = { 0 };
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rs = { 2, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
    ID3DBlob *blob = NULL, *errors = NULL;
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors))) {
        if (errors) fprintf(stderr, "cui: %s\n", (const char *)errors->lpVtbl->GetBufferPointer(errors));
        CUI__REL(errors);
        return false;
    }
    dev->lpVtbl->CreateRootSignature(dev, 0, blob->lpVtbl->GetBufferPointer(blob), blob->lpVtbl->GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&r->root);
    CUI__REL(blob);
    if (!r->root) return false;

    /* one element per group of cui_vertex fields, as the OpenGL renderer's attributes */
    D3D12_INPUT_ELEMENT_DESC elements[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0, (UINT)offsetof(cui_vertex, x),            D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, (UINT)offsetof(cui_vertex, u),            D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT,       0, (UINT)offsetof(cui_vertex, lx),           D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 2, DXGI_FORMAT_R32G32_FLOAT,       0, (UINT)offsetof(cui_vertex, hw),           D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, (UINT)offsetof(cui_vertex, r_tl),         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 4, DXGI_FORMAT_R32G32_FLOAT,       0, (UINT)offsetof(cui_vertex, border),       D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM,     0, (UINT)offsetof(cui_vertex, color),        D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    1, DXGI_FORMAT_R8G8B8A8_UNORM,     0, (UINT)offsetof(cui_vertex, border_color), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
    ID3DBlob *vs = cui__d3d12_compile("vs_main", "vs_5_0"), *ps = cui__d3d12_compile("ps_main", "ps_5_0");
    if (vs && ps) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d;
        memset(&d, 0, sizeof d);
        d.pRootSignature = r->root;
        d.VS.pShaderBytecode = vs->lpVtbl->GetBufferPointer(vs);
        d.VS.BytecodeLength = vs->lpVtbl->GetBufferSize(vs);
        d.PS.pShaderBytecode = ps->lpVtbl->GetBufferPointer(ps);
        d.PS.BytecodeLength = ps->lpVtbl->GetBufferSize(ps);
        d.SampleMask = 0xffffffff;
        d.InputLayout.pInputElementDescs = elements;
        d.InputLayout.NumElements = (UINT)(sizeof elements / sizeof elements[0]);
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.NumRenderTargets = 1;
        d.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        /* premultiplied source over */
        D3D12_RENDER_TARGET_BLEND_DESC *rt = &d.BlendState.RenderTarget[0];
        rt->BlendEnable = TRUE;
        rt->SrcBlend = D3D12_BLEND_ONE;
        rt->DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        rt->BlendOp = D3D12_BLEND_OP_ADD;
        rt->SrcBlendAlpha = D3D12_BLEND_ONE;
        rt->DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        rt->BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rt->LogicOp = D3D12_LOGIC_OP_NOOP;
        rt->RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        dev->lpVtbl->CreateGraphicsPipelineState(dev, &d, &IID_ID3D12PipelineState, (void **)&r->pso);
    }
    CUI__REL(vs);
    CUI__REL(ps);
    return r->pso != NULL;
}

/* whether an adapter's name (UTF-16) holds CUI_GPU's value, ignoring case */
static bool cui__d3d12_wanted(const WCHAR *name, const char *want) {
    char utf8[384];   /* DXGI names are at most 128 UTF-16 units */
    if (!want || !*want || !WideCharToMultiByte(CP_UTF8, 0, name, -1, utf8, sizeof utf8, NULL, NULL)) return false;
    return cui__contains_ci(utf8, want);
}

static bool cui__d3d12_device(cui_gpu_renderer *r) {
    if (FAILED(CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void **)&r->factory))) return false;
    IDXGIFactory5 *f5 = NULL;
    if (SUCCEEDED(r->factory->lpVtbl->QueryInterface(r->factory, &IID_IDXGIFactory5, (void **)&f5))) {
        BOOL tear = FALSE;
        if (SUCCEEDED(f5->lpVtbl->CheckFeatureSupport(f5, DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tear, sizeof tear))) r->tearing = tear != FALSE;
        CUI__REL(f5);
    }
    /* the adapter CUI_GPU names, else the first hardware one that takes a device */
    IDXGIAdapter1 *a = NULL;
    const char *want = getenv("CUI_GPU");
    for (int pass = want && *want ? 0 : 1; pass < 2 && !r->device; pass++)
        for (UINT i = 0; !r->device && r->factory->lpVtbl->EnumAdapters1(r->factory, i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_ADAPTER_DESC1 d;
            a->lpVtbl->GetDesc1(a, &d);
            bool fits = pass == 0 ? cui__d3d12_wanted(d.Description, want) : !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE);
            if (fits) D3D12CreateDevice((IUnknown *)a, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&r->device);
            if (fits && r->device && pass == 0) fprintf(stderr, "cui: GPU %ls\n", d.Description);   /* (as the Vulkan renderer says which it took) */
            CUI__REL(a);
        }
    if (!r->device) return false;
    ID3D12Device *dev = r->device;
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    if (FAILED(dev->lpVtbl->CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&r->queue))) return false;
    for (int i = 0; i < CUI__T12_FRAMES; i++)
        if (FAILED(dev->lpVtbl->CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&r->alloc[i]))) return false;
    if (FAILED(dev->lpVtbl->CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, r->alloc[0], NULL, &IID_ID3D12GraphicsCommandList, (void **)&r->list))) return false;
    r->list->lpVtbl->Close(r->list);
    if (FAILED(dev->lpVtbl->CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&r->fence))) return false;
    r->event = CreateEventA(NULL, FALSE, FALSE, NULL);

    D3D12_DESCRIPTOR_HEAP_DESC sh = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, CUI__T12_TEX_CAP, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
    D3D12_DESCRIPTOR_HEAP_DESC rh = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, CUI__T12_BUFFERS, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
    if (FAILED(dev->lpVtbl->CreateDescriptorHeap(dev, &sh, &IID_ID3D12DescriptorHeap, (void **)&r->srv_heap)) ||
        FAILED(dev->lpVtbl->CreateDescriptorHeap(dev, &rh, &IID_ID3D12DescriptorHeap, (void **)&r->rtv_heap))) return false;
    r->srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(r->srv_heap, &r->srv_cpu);
    r->srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(r->srv_heap, &r->srv_gpu);
    r->rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(r->rtv_heap, &r->rtv_cpu);
    r->srv_inc = dev->lpVtbl->GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    r->rtv_inc = dev->lpVtbl->GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    if (!cui__d3d12_pipeline(r)) return false;
    static const uint8_t black[4] = { 0, 0, 0, 255 };   /* what OpenGL samples from no texture */
    r->none = (int)cui__d3d12_tex_create(r, 1, 1, CUI_TEXTURE_RGBA8, black) - 1;
    cui__d3d12_idle(r);
    /* a renderer that stopped while starting, or whose device went away, is none: the app draws with OpenGL */
    return r->none >= 0 && !r->lost && SUCCEEDED(dev->lpVtbl->GetDeviceRemovedReason(dev));
}

/* ---- the window ---- */

static bool cui__d3d12_targets(cui_gpu_renderer *r) {
    for (int i = 0; i < CUI__T12_BUFFERS; i++) {
        if (FAILED(r->swapchain->lpVtbl->GetBuffer(r->swapchain, (UINT)i, &IID_ID3D12Resource, (void **)&r->buffers[i]))) { r->buffers[i] = NULL; return false; }
        r->device->lpVtbl->CreateRenderTargetView(r->device, r->buffers[i], NULL, cui__d3d12_rtv_at(r, i));
    }
    D3D12_RESOURCE_DESC d;
    r->buffers[0]->lpVtbl->GetDesc(r->buffers[0], &d);
    r->w = (int)d.Width; r->h = (int)d.Height;
    return true;
}



static bool cui__d3d12_swapchain(cui_gpu_renderer *r) {
    DXGI_SWAP_CHAIN_DESC1 desc = { 0 };
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = CUI__T12_BUFFERS;
    desc.Scaling = DXGI_SCALING_NONE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Flags = r->flags;
    IDXGISwapChain1 *sc1 = NULL;
    IDXGIFactory4 *f = r->factory;
    if (FAILED(f->lpVtbl->CreateSwapChainForHwnd(f, (IUnknown *)r->queue, r->hwnd, &desc, NULL, NULL, &sc1)) ||
        FAILED(sc1->lpVtbl->QueryInterface(sc1, &IID_IDXGISwapChain3, (void **)&r->swapchain))) { CUI__REL(sc1); r->swapchain = NULL; return false; }
    CUI__REL(sc1);
    f->lpVtbl->MakeWindowAssociation(f, r->hwnd, DXGI_MWA_NO_ALT_ENTER);
    /* frames queued ahead of the display: 2 (1 misses refreshes at 240 Hz) */
    r->swapchain->lpVtbl->SetMaximumFrameLatency(r->swapchain, 2);
    r->waitable = r->swapchain->lpVtbl->GetFrameLatencyWaitableObject(r->swapchain);
    if (!cui__d3d12_targets(r)) return false;
    WaitForSingleObjectEx(r->waitable, 1000, TRUE);
    return true;
}

void cui_gpu_destroy(cui_gpu_renderer *r) {
    if (!r) return;
    if (r->queue && r->fence) cui__d3d12_idle(r);
    /* Stopped on a device that is still there (before or during that idle): work may be running
     * that no fence value accounts for. What it uses is left alone; only the memory on this side
     * is given back. */
    if (r->lost && r->device && SUCCEEDED(r->device->lpVtbl->GetDeviceRemovedReason(r->device))) {
        free(r->later);
        free(r->tex);
        free(r);
        return;
    }
    if (r->queue && r->fence) cui__d3d12_collect(r, true);
    free(r->later);
    for (int i = 0; i < CUI__T12_BUFFERS; i++) CUI__REL(r->buffers[i]);
    if (r->waitable) CloseHandle(r->waitable);
    CUI__REL(r->swapchain);
    for (int i = 0; i < r->tex_count; i++) CUI__REL(r->tex[i].res);
    free(r->tex);
    CUI__REL(r->pso);
    CUI__REL(r->root);
    for (int i = 0; i < CUI__T12_FRAMES; i++) { CUI__REL(r->alloc[i]); CUI__REL(r->upload[i].res); }
    CUI__REL(r->list);
    CUI__REL(r->srv_heap); CUI__REL(r->rtv_heap);
    CUI__REL(r->fence);
    if (r->event) CloseHandle(r->event);
    CUI__REL(r->queue); CUI__REL(r->device); CUI__REL(r->factory);
    free(r);
}

cui_gpu_renderer *cui_gpu_create(void *window, bool vsync) {
    if (!window) return NULL;
    cui_gpu_renderer *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->hwnd = (HWND)window;
    r->vsync = vsync;
    if (!cui__d3d12_device(r)) { cui_gpu_destroy(r); return NULL; }
    r->flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (r->tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    if (!cui__d3d12_swapchain(r)) { cui_gpu_destroy(r); return NULL; }
    return r;
}

cui_renderer_iface cui_gpu_iface(cui_gpu_renderer *r) {
    cui_renderer_iface i;
    memset(&i, 0, sizeof i);
    if (!r) return i;
    i.user = r;
    i.texture_create = cui__d3d12_tex_create;
    i.texture_update = cui__d3d12_tex_update;
    i.texture_destroy = cui__d3d12_tex_destroy;
    return i;
}

/* ---- frames ---- */

/* What one frame's phases share */
typedef struct cui__d3d12_frame {
    ID3D12GraphicsCommandList *cmd;
    int bb;                                   /* the back buffer drawn into */
    cui__d3d12_buffer readback;               /* the frame copied out, when it was asked for */
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;    /* its layout */
} cui__d3d12_frame;

/* The back buffer to draw into, cleared; the buffers follow the window's size once the GPU is done with them. */
static bool cui__d3d12_acquire(cui_gpu_renderer *r, cui__d3d12_frame *f, const cui_draw_data *dd) {
    RECT rc;
    GetClientRect(r->hwnd, &rc);
    int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    if (cw <= 0 || ch <= 0) return false;   /* minimized */
    if (cw != r->w || ch != r->h) {
        cui__d3d12_idle(r);
        for (int i = 0; i < CUI__T12_BUFFERS; i++) CUI__REL(r->buffers[i]);
        if (FAILED(r->swapchain->lpVtbl->ResizeBuffers(r->swapchain, 0, (UINT)cw, (UINT)ch, DXGI_FORMAT_UNKNOWN, r->flags)) || !cui__d3d12_targets(r)) {
            for (int i = 0; i < CUI__T12_BUFFERS; i++) CUI__REL(r->buffers[i]);
            r->w = r->h = 0;   /* (no back buffers: asked for again with the next frame, unless the device is gone) */
            if (FAILED(r->device->lpVtbl->GetDeviceRemovedReason(r->device))) cui__d3d12_lose(r);
            return false;
        }
    }
    f->cmd = cui__d3d12_cmd(r);
    if (!f->cmd) return false;
    f->bb = (int)r->swapchain->lpVtbl->GetCurrentBackBufferIndex(r->swapchain);
    cui__d3d12_barrier(r, r->buffers[f->bb], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = cui__d3d12_rtv_at(r, f->bb);
    float clear[4] = { dd->clear_color.r / 255.0f, dd->clear_color.g / 255.0f, dd->clear_color.b / 255.0f, 1 };
    f->cmd->lpVtbl->OMSetRenderTargets(f->cmd, 1, &rtv, FALSE, NULL);
    f->cmd->lpVtbl->ClearRenderTargetView(f->cmd, rtv, clear, 0, NULL);
    return true;
}

/* The frame's vertices and indices, then a draw per command (cui_gl_render's counterpart). */
/* False when the frame's vertices found no room: only the cleared window is shown. */
static bool cui__d3d12_draw(cui_gpu_renderer *r, const cui__d3d12_frame *f, const cui_draw_data *dd) {
    ID3D12GraphicsCommandList *cmd = f->cmd;
    size_t vbytes = sizeof(cui_vertex) * (size_t)dd->vertex_count, ibytes = sizeof(uint32_t) * (size_t)dd->index_count;
    D3D12_GPU_VIRTUAL_ADDRESS vva = 0, iva = 0;
    uint8_t *v = dd->index_count ? cui__d3d12_upload(r, vbytes, 16, &vva, NULL, NULL) : NULL;
    uint8_t *ix = v ? cui__d3d12_upload(r, ibytes, 16, &iva, NULL, NULL) : NULL;
    if (!dd->index_count) return true;
    if (!v || !ix) return false;
    memcpy(v, dd->vertices, vbytes);
    memcpy(ix, dd->indices, ibytes);
    int W = dd->width_px, H = dd->height_px;
    D3D12_VIEWPORT vp = { 0, 0, (float)W, (float)H, 0, 1 };
    D3D12_VERTEX_BUFFER_VIEW vbv = { vva, (UINT)vbytes, sizeof(cui_vertex) };
    D3D12_INDEX_BUFFER_VIEW ibv = { iva, (UINT)ibytes, DXGI_FORMAT_R32_UINT };
    float viewport[4] = { (float)W, (float)H, 0, 0 };
    cmd->lpVtbl->RSSetViewports(cmd, 1, &vp);
    cmd->lpVtbl->SetDescriptorHeaps(cmd, 1, &r->srv_heap);
    cmd->lpVtbl->SetGraphicsRootSignature(cmd, r->root);
    cmd->lpVtbl->SetPipelineState(cmd, r->pso);
    cmd->lpVtbl->IASetPrimitiveTopology(cmd, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->lpVtbl->IASetVertexBuffers(cmd, 0, 1, &vbv);
    cmd->lpVtbl->IASetIndexBuffer(cmd, &ibv);
    cmd->lpVtbl->SetGraphicsRoot32BitConstants(cmd, 0, 4, viewport, 0);
    int bound = -1;
    for (int i = 0; i < dd->command_count; i++) {
        const cui_draw_cmd *c = &dd->commands[i];
        if (!c->index_count) continue;
        int x0 = (int)floorf(c->clip.x), y0 = (int)floorf(c->clip.y);
        int x1 = (int)ceilf(c->clip.x + c->clip.w), y1 = (int)ceilf(c->clip.y + c->clip.h);
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > r->w) x1 = r->w;
        if (y1 > r->h) y1 = r->h;
        if (x1 <= x0 || y1 <= y0) continue;
        D3D12_RECT sc = { x0, y0, x1, y1 };
        cmd->lpVtbl->RSSetScissorRects(cmd, 1, &sc);
        cui_texture t = c->texture ? c->texture : dd->font_atlas;
        int slot = (int)t - 1;
        if (slot < 0 || slot >= r->tex_count || !r->tex[slot].res) slot = r->none;
        if (slot != bound) {
            D3D12_GPU_DESCRIPTOR_HANDLE at = { r->srv_gpu.ptr + (UINT64)slot * r->srv_inc };
            cmd->lpVtbl->SetGraphicsRootDescriptorTable(cmd, 1, at);
            bound = slot;
        }
        cmd->lpVtbl->DrawIndexedInstanced(cmd, c->index_count, 1, c->index_offset, 0, 0);
    }
    return true;
}

/* The frame copied out, if asked, and the back buffer made ready to present. */
static void cui__d3d12_copy_out(cui_gpu_renderer *r, cui__d3d12_frame *f, bool wanted) {
    ID3D12Resource *target = r->buffers[f->bb];
    memset(&f->fp, 0, sizeof f->fp);
    if (wanted) {
        D3D12_RESOURCE_DESC d;
        UINT64 size = 0;
        target->lpVtbl->GetDesc(target, &d);
        r->device->lpVtbl->GetCopyableFootprints(r->device, &d, 0, 1, 0, &f->fp, NULL, NULL, &size);
        if (cui__d3d12_buffer_make(r, &f->readback, (size_t)size, D3D12_HEAP_TYPE_READBACK)) {
            cui__d3d12_barrier(r, target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION dst = { 0 }, src = { 0 };
            dst.pResource = f->readback.res;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint = f->fp;
            src.pResource = target;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            f->cmd->lpVtbl->CopyTextureRegion(f->cmd, &dst, 0, 0, 0, &src, NULL);
            cui__d3d12_barrier(r, target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
        }
    }
    if (!f->readback.res) cui__d3d12_barrier(r, target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
}

static void cui__d3d12_present(cui_gpu_renderer *r) {
    cui__d3d12_submit(r);
    if (r->lost) return;   /* (the frame was not submitted: there is nothing to present) */
    bool tear = !r->vsync && r->tearing;
    HRESULT shown = r->swapchain->lpVtbl->Present(r->swapchain, r->vsync ? 1 : 0, tear ? DXGI_PRESENT_ALLOW_TEARING : 0);
    if (shown == DXGI_ERROR_DEVICE_REMOVED || shown == DXGI_ERROR_DEVICE_RESET) cui__d3d12_lose(r);
    /* so that idle waits for the present too (a resize releases the back buffers) */
    if (SUCCEEDED(r->queue->lpVtbl->Signal(r->queue, r->fence, r->fence_value + 1))) r->fence_value++;
    else cui__d3d12_lose(r);
}

/* Waits for the copy and writes it as RGBA; false if one was asked for and could not be made. */
static bool cui__d3d12_read_back(cui_gpu_renderer *r, cui__d3d12_frame *f, const cui_draw_data *dd, void *rgba) {
    bool ok = !rgba;
    if (f->readback.res && r->lost) {   /* the copy may still be writing it: it is neither read nor released */
        f->readback.res = NULL;
        return false;
    }
    if (f->readback.res) {
        cui__d3d12_wait(r, r->fence_value);
        if ((int)f->fp.Footprint.Width == dd->width_px && (int)f->fp.Footprint.Height == dd->height_px) {
            const uint8_t *src = f->readback.ptr + f->fp.Offset;
            uint8_t *dst = rgba;
            for (int y = 0; y < dd->height_px; y++, src += f->fp.Footprint.RowPitch)
                for (int x = 0; x < dd->width_px; x++, dst += 4) {
                    dst[0] = src[x * 4 + 2]; dst[1] = src[x * 4 + 1]; dst[2] = src[x * 4]; dst[3] = 255;
                }
            ok = true;
        }
        CUI__REL(f->readback.res);
    }
    return ok;
}

static bool cui__d3d12_render(cui_gpu_renderer *r, const cui_draw_data *dd, void *rgba) {
    if (!r || !dd || r->lost) return false;
    cui__d3d12_frame f = { 0 };
    if (!cui__d3d12_acquire(r, &f, dd)) return false;
    bool drawn = cui__d3d12_draw(r, &f, dd);
    cui__d3d12_copy_out(r, &f, rgba != NULL);
    cui__d3d12_present(r);
    bool ok = cui__d3d12_read_back(r, &f, dd, rgba) && drawn && !r->lost;   /* (a capture of a frame that was not drawn, or not finished, is none) */
    /* wait until the swapchain can take another frame */
    WaitForSingleObjectEx(r->waitable, 1000, TRUE);
    return ok;
}

void cui_gpu_render(cui_gpu_renderer *r, const cui_draw_data *dd) { cui__d3d12_render(r, dd, NULL); }
bool cui_gpu_render_capture(cui_gpu_renderer *r, const cui_draw_data *dd, void *rgba) { return rgba && cui__d3d12_render(r, dd, rgba); }

#endif
