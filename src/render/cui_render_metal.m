/* Metal renderer (cui_gpu_*, built with CUI_METAL, macOS): the draw data
 * the OpenGL renderer takes (cui_draw_data's vertices, indices and commands), drawn the same
 * way. One pipeline; its fragment function is cui_render_gl.c's fragment shader in the Metal
 * Shading Language, branching on the vertex's mode. Keep the two in step.
 *
 * - A frame's vertices and indices are copied into a shared-storage buffer (one of three in
 *   turn, reused once the frame that read it is done), then a draw per command with its clip
 *   rectangle as the scissor and its texture.
 * - Drawn straight into a BGRA8 UNORM drawable of a CAMetalLayer, which becomes the layer of
 *   the window's content view, premultiplied source over, as the OpenGL renderer blends.
 *   Paced by the layer handing out drawables (with vsync, displaySyncEnabled).
 * Compile with -fobjc-arc. */
#include "cui/cui.h"
#include "cui/cui_render.h"

#if defined(CUI_METAL) && defined(__APPLE__)

#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CUI__TMTL_SLOTS 3           /* vertex memory, reused once its last frame is done */

static const char cui__mtl_msl[] =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct VI {\n"
"  float2 pos [[attribute(0)]]; float2 uv [[attribute(1)]]; float2 local [[attribute(2)]]; float2 half_ [[attribute(3)]];\n"
"  float4 radii [[attribute(4)]]; float2 params [[attribute(5)]]; float4 color [[attribute(6)]]; float4 bcolor [[attribute(7)]];\n"
"};\n"
"struct VO {\n"
"  float4 pos [[position]]; float2 uv; float2 local;\n"
"  float2 half_ [[flat]]; float4 radii [[flat]]; float2 params [[flat]]; float4 color; float4 bcolor [[flat]];\n"
"};\n"
"vertex VO vs_main(VI i [[stage_in]], constant float4 &viewport [[buffer(1)]]) {\n"
"  VO o;\n"
"  o.uv = i.uv; o.local = i.local; o.half_ = i.half_; o.radii = i.radii;\n"
"  o.params = i.params; o.color = i.color; o.bcolor = i.bcolor;\n"
"  o.pos = float4(i.pos.x / viewport.x * 2.0 - 1.0, 1.0 - i.pos.y / viewport.y * 2.0, 0.0, 1.0);\n"
"  return o;\n"
"}\n"
"static float sd_rrect(float2 p, float2 b, float4 r) {\n"
"  float rr = p.x < 0.0 ? (p.y < 0.0 ? r.x : r.w) : (p.y < 0.0 ? r.y : r.z);\n"
"  rr = min(rr, min(b.x, b.y));\n"
"  float2 q = abs(p) - b + rr;\n"
"  return min(max(q.x, q.y), 0.0) + length(max(q, float2(0.0))) - rr;\n"
"}\n"
"static float erf_(float x) {\n"
"  float s = sign(x); x = abs(x);\n"
"  float t = 1.0 / (1.0 + 0.3275911 * x);\n"
"  float y = 1.0 - (((((1.061405429 * t - 1.453152027) * t) + 1.421413741) * t - 0.284496736) * t + 0.254829592) * t * exp(-x * x);\n"
"  return s * y;\n"
"}\n"
"static float bayer8(float2 p) {\n"
"  float2 f = floor(p);\n"
"  int2 q = int2(f - 8.0 * floor(f / 8.0));\n"
"  int a = q.x ^ q.y, y = q.y;\n"
"  int m = ((a & 1) << 5) | ((y & 1) << 4) | ((a & 2) << 2) | ((y & 2) << 1) | ((a & 4) >> 1) | ((y & 4) >> 2);\n"
"  return (float(m) + 0.5) / 64.0;\n"
"}\n"
"fragment float4 ps_main(VO i [[stage_in]], texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]]) {\n"
"  int mode = int(i.params.y + 0.5);\n"
"  float4 c = i.color;\n"
"  if (mode == 4) return float4(c.rgb * c.a, c.a);\n"
"  float4 t = tex.sample(smp, i.uv, level(0.0));\n"   /* (textures have one level) */
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

@interface CUIMetalSlot : NSObject
@property (nonatomic, strong) id<MTLBuffer> vertices, indices;
@property (nonatomic, strong) id<MTLCommandBuffer> reader;   /* the frame that last read it */
@end
@implementation CUIMetalSlot
@end

@interface CUIMetalWindow : NSObject
@property (nonatomic, strong) NSWindow *window;
@property (nonatomic, strong) CAMetalLayer *layer;
@property (nonatomic, strong) id<MTLDevice> device;
@property (nonatomic, strong) id<MTLCommandQueue> queue;
@property (nonatomic, strong) id<MTLRenderPipelineState> pso;
@property (nonatomic, strong) id<MTLSamplerState> sampler;
@property (nonatomic, strong) id<MTLTexture> none;   /* a black texel, for a command with no texture */
@property (nonatomic, strong) NSArray<CUIMetalSlot *> *slots;
/* The textures alive, by their cui_texture (the object's address). A handle that is not here
 * (destroyed, never made) draws the black texel instead of being used as an object. */
@property (nonatomic, strong) NSMutableDictionary<NSNumber *, id<MTLTexture>> *textures;
@property (nonatomic, assign) int slot;
@property (nonatomic, strong) id<MTLCommandBuffer> last;   /* the frame committed last */
@end
@implementation CUIMetalWindow
@end

struct cui_gpu_renderer {
    void *win;                          /* CUIMetalWindow, retained */
    bool readable;                      /* a frame was read back: the drawables stay readable from then on */
};

static CUIMetalWindow *cui__mtl_win(const cui_gpu_renderer *r) { return (__bridge CUIMetalWindow *)r->win; }

/* ---- textures: R8 for the glyph atlas, RGBA8 for images ---- */

static id<MTLTexture> cui__mtl_tex_make(id<MTLDevice> device, int w, int h, cui_texture_format fmt, const void *pixels) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt == CUI_TEXTURE_R8 ? MTLPixelFormatR8Unorm : MTLPixelFormatRGBA8Unorm
                                                                                 width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> t = [device newTextureWithDescriptor:d];
    if (t && pixels) {
        NSUInteger bpp = fmt == CUI_TEXTURE_R8 ? 1 : 4;
        [t replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)w, (NSUInteger)h) mipmapLevel:0 withBytes:pixels bytesPerRow:(NSUInteger)w * bpp];
    }
    return t;
}

static cui_texture cui__mtl_tex_create(void *user, int w, int h, cui_texture_format fmt, const void *pixels) {
    if (w <= 0 || h <= 0) return 0;
    @autoreleasepool {
        CUIMetalWindow *win = cui__mtl_win(user);
        id<MTLTexture> t = cui__mtl_tex_make(win.device, w, h, fmt, pixels);
        if (!t) return 0;
        cui_texture handle = (cui_texture)(uintptr_t)(__bridge void *)t;
        win.textures[@((uintptr_t)handle)] = t;   /* the table keeps it alive */
        return handle;
    }
}

static void cui__mtl_tex_update(void *user, cui_texture tex, int x, int y, int w, int h, const void *pixels, int stride) {
    if (!tex || w <= 0 || h <= 0 || !pixels) return;
    @autoreleasepool {
        id<MTLTexture> t = cui__mtl_win(user).textures[@((uintptr_t)tex)];
        if (!t) return;
        /* replaceRegion writes at once, and is not ordered after frames on the GPU: a frame still
         * drawing may sample this texture, so the copy waits for the last one (no wait if it is done) */
        [cui__mtl_win(user).last waitUntilCompleted];
        NSUInteger bpp = t.pixelFormat == MTLPixelFormatR8Unorm ? 1 : 4;
        [t replaceRegion:MTLRegionMake2D((NSUInteger)x, (NSUInteger)y, (NSUInteger)w, (NSUInteger)h) mipmapLevel:0
               withBytes:pixels bytesPerRow:stride ? (NSUInteger)stride : (NSUInteger)w * bpp];
    }
}

static void cui__mtl_tex_destroy(void *user, cui_texture tex) {
    if (!tex) return;
    /* frames in flight that sample it keep it alive until they are done */
    @autoreleasepool { [cui__mtl_win(user).textures removeObjectForKey:@((uintptr_t)tex)]; }
}

/* ---- the device ---- */

static bool cui__mtl_pipeline(CUIMetalWindow *w) {
    /* one attribute per group of cui_vertex fields, as the OpenGL renderer's */
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    static const struct { MTLVertexFormat format; NSUInteger offset; } attrs[8] = {
        { MTLVertexFormatFloat2, offsetof(cui_vertex, x) }, { MTLVertexFormatFloat2, offsetof(cui_vertex, u) },
        { MTLVertexFormatFloat2, offsetof(cui_vertex, lx) }, { MTLVertexFormatFloat2, offsetof(cui_vertex, hw) },
        { MTLVertexFormatFloat4, offsetof(cui_vertex, r_tl) }, { MTLVertexFormatFloat2, offsetof(cui_vertex, border) },
        { MTLVertexFormatUChar4Normalized, offsetof(cui_vertex, color) }, { MTLVertexFormatUChar4Normalized, offsetof(cui_vertex, border_color) },
    };
    for (NSUInteger i = 0; i < 8; i++) {
        vd.attributes[i].format = attrs[i].format;
        vd.attributes[i].offset = attrs[i].offset;
        vd.attributes[i].bufferIndex = 0;
    }
    vd.layouts[0].stride = sizeof(cui_vertex);
    vd.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
    NSError *error = nil;
    id<MTLLibrary> lib = [w.device newLibraryWithSource:[NSString stringWithUTF8String:cui__mtl_msl] options:nil error:&error];
    if (!lib) { fprintf(stderr, "cui: metal: %s\n", error.localizedDescription.UTF8String); return false; }
    MTLRenderPipelineDescriptor *d = [[MTLRenderPipelineDescriptor alloc] init];
    d.vertexFunction = [lib newFunctionWithName:@"vs_main"];
    d.fragmentFunction = [lib newFunctionWithName:@"ps_main"];
    d.vertexDescriptor = vd;
    /* premultiplied source over */
    MTLRenderPipelineColorAttachmentDescriptor *c = d.colorAttachments[0];
    c.pixelFormat = MTLPixelFormatBGRA8Unorm;
    c.blendingEnabled = YES;
    c.sourceRGBBlendFactor = MTLBlendFactorOne;
    c.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    c.sourceAlphaBlendFactor = MTLBlendFactorOne;
    c.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    w.pso = [w.device newRenderPipelineStateWithDescriptor:d error:&error];
    if (!w.pso) { fprintf(stderr, "cui: metal: %s\n", error.localizedDescription.UTF8String); return false; }
    return true;
}

static bool cui__mtl_device(CUIMetalWindow *w) {
    /* the device CUI_GPU names, else the system's default */
    const char *want = getenv("CUI_GPU");
    if (want && *want) {
        NSString *part = [NSString stringWithUTF8String:want];
        for (id<MTLDevice> d in MTLCopyAllDevices())
            if (!w.device && part && [d.name rangeOfString:part options:NSCaseInsensitiveSearch].location != NSNotFound) w.device = d;
    }
    if (!w.device) w.device = MTLCreateSystemDefaultDevice();
    if (!w.device) return false;
    w.queue = [w.device newCommandQueue];
    if (!w.queue || !cui__mtl_pipeline(w)) return false;
    MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
    sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    w.sampler = [w.device newSamplerStateWithDescriptor:sd];
    static const uint8_t black[4] = { 0, 0, 0, 255 };   /* what OpenGL samples from no texture */
    w.none = cui__mtl_tex_make(w.device, 1, 1, CUI_TEXTURE_RGBA8, black);
    return w.sampler && w.none;
}

/* ---- the window ---- */

void cui_gpu_destroy(cui_gpu_renderer *r) {
    if (!r) return;
    @autoreleasepool {
        CUIMetalWindow *w = (__bridge_transfer CUIMetalWindow *)r->win;
        for (CUIMetalSlot *s in w.slots) [s.reader waitUntilCompleted];
        w = nil;
    }
    free(r);
}

/* window: the NSWindow; its content view shows the Metal layer */
cui_gpu_renderer *cui_gpu_create(void *window, bool vsync) {
    @autoreleasepool {
        NSWindow *nsw = (__bridge NSWindow *)window;
        if (!nsw || ![nsw isKindOfClass:[NSWindow class]] || !nsw.contentView) return NULL;
        CUIMetalWindow *w = [[CUIMetalWindow alloc] init];
        if (!cui__mtl_device(w)) return NULL;
        cui_gpu_renderer *r = calloc(1, sizeof *r);
        if (!r) return NULL;
        w.window = nsw;
        CAMetalLayer *layer = [CAMetalLayer layer];
        layer.device = w.device;
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.framebufferOnly = YES;
        layer.opaque = YES;
        layer.maximumDrawableCount = 3;
        layer.displaySyncEnabled = vsync;
        layer.allowsNextDrawableTimeout = YES;
        /* CUI's colors are sRGB: the system converts to the display's (P3 on a Mac's own) */
        CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        layer.colorspace = srgb;
        CGColorSpaceRelease(srgb);
        layer.contentsScale = nsw.backingScaleFactor;
        /* a resize shows the last frame at the top left until the next is drawn, not stretched */
        layer.contentsGravity = kCAGravityTopLeft;
        layer.actions = @{ @"contents" : [NSNull null], @"bounds" : [NSNull null], @"position" : [NSNull null] };
        nsw.contentView.layer = layer;      /* layer first: the view hosts it, AppKit does not draw into it */
        nsw.contentView.wantsLayer = YES;
        w.layer = layer;
        w.textures = [NSMutableDictionary dictionary];
        w.slots = @[ [[CUIMetalSlot alloc] init], [[CUIMetalSlot alloc] init], [[CUIMetalSlot alloc] init] ];
        r->win = (__bridge_retained void *)w;
        return r;
    }
}

cui_renderer_iface cui_gpu_iface(cui_gpu_renderer *r) {
    cui_renderer_iface i;
    memset(&i, 0, sizeof i);
    if (!r) return i;
    i.user = r;
    i.texture_create = cui__mtl_tex_create;
    i.texture_update = cui__mtl_tex_update;
    i.texture_destroy = cui__mtl_tex_destroy;
    return i;
}

/* ---- frames ---- */

/* a shared-storage buffer of at least `bytes` */
static id<MTLBuffer> cui__mtl_room(id<MTLDevice> device, id<MTLBuffer> have, size_t bytes) {
    if (have.length >= bytes) return have;
    size_t cap = have.length * 2 > 65536 ? have.length * 2 : 65536;
    while (cap < bytes) cap *= 2;
    return [device newBufferWithLength:cap options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined];
}

/* The drawable follows the frame's size in pixels. */
static void cui__mtl_layer(cui_gpu_renderer *r, CUIMetalWindow *w, int fw, int fh, bool capture) {
    CAMetalLayer *layer = w.layer;
    CGSize size = layer.drawableSize;
    if ((int)size.width != fw || (int)size.height != fh) layer.drawableSize = CGSizeMake(fw, fh);
    CGFloat scale = w.window.backingScaleFactor;
    if (layer.contentsScale != scale) layer.contentsScale = scale;
    /* a frame read back is blitted from; once one was, the drawables stay readable */
    if (capture) r->readable = true;
    if (layer.framebufferOnly != !r->readable) layer.framebufferOnly = !r->readable;
    /* while the window is dragged to a new size, the frame joins the Core Animation transaction
     * that resizes the window: drawn for the new size and shown with it, not a refresh later */
    BOOL sync = w.window.inLiveResize;
    if (layer.presentsWithTransaction != sync) layer.presentsWithTransaction = sync;
}

/* The next slot of vertex and index buffers, once its last reader is done. */
static CUIMetalSlot *cui__mtl_slot(CUIMetalWindow *w) {
    w.slot = (w.slot + 1) % CUI__TMTL_SLOTS;
    CUIMetalSlot *slot = w.slots[(NSUInteger)w.slot];
    if (slot.reader) { [slot.reader waitUntilCompleted]; slot.reader = nil; }
    return slot;
}

/* The frame's vertices and indices into the slot; false when there is nothing to draw. */
static bool cui__mtl_upload(CUIMetalWindow *w, CUIMetalSlot *slot, const cui_draw_data *dd) {
    size_t vbytes = sizeof(cui_vertex) * (size_t)dd->vertex_count, ibytes = sizeof(uint32_t) * (size_t)dd->index_count;
    if (dd->index_count <= 0 || dd->vertex_count <= 0) return false;
    slot.vertices = cui__mtl_room(w.device, slot.vertices, vbytes);
    slot.indices = cui__mtl_room(w.device, slot.indices, ibytes);
    if (!slot.vertices || !slot.indices) return false;
    memcpy(slot.vertices.contents, dd->vertices, vbytes);
    memcpy(slot.indices.contents, dd->indices, ibytes);
    return true;
}

/* The pass into the drawable, cleared. */
static id<MTLRenderCommandEncoder> cui__mtl_encoder(id<MTLCommandBuffer> cmd, id<CAMetalDrawable> drawable, const cui_draw_data *dd) {
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = drawable.texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(dd->clear_color.r / 255.0, dd->clear_color.g / 255.0, dd->clear_color.b / 255.0, 1);
    return [cmd renderCommandEncoderWithDescriptor:pass];
}

/* A draw per command (cui_gl_render's counterpart); tw and th are the drawable's size. */
static void cui__mtl_draw(CUIMetalWindow *w, id<MTLRenderCommandEncoder> enc, CUIMetalSlot *slot, const cui_draw_data *dd, int tw, int th) {
    int fw = dd->width_px, fh = dd->height_px;
    float viewport[4] = { (float)fw, (float)fh, 0, 0 };
    MTLViewport vp = { 0, 0, fw, fh, 0, 1 };
    [enc setViewport:vp];
    [enc setCullMode:MTLCullModeNone];
    [enc setRenderPipelineState:w.pso];
    [enc setVertexBuffer:slot.vertices offset:0 atIndex:0];
    [enc setVertexBytes:viewport length:sizeof viewport atIndex:1];
    [enc setFragmentSamplerState:w.sampler atIndex:0];
    void *bound = NULL;
    bool first = true;
    for (int i = 0; i < dd->command_count; i++) {
        const cui_draw_cmd *c = &dd->commands[i];
        if (!c->index_count) continue;
        int x0 = (int)floorf(c->clip.x), y0 = (int)floorf(c->clip.y);
        int x1 = (int)ceilf(c->clip.x + c->clip.w), y1 = (int)ceilf(c->clip.y + c->clip.h);
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > tw) x1 = tw;
        if (y1 > th) y1 = th;
        if (x1 <= x0 || y1 <= y0) continue;
        MTLScissorRect sc = { (NSUInteger)x0, (NSUInteger)y0, (NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0) };
        [enc setScissorRect:sc];
        void *tex = (void *)(uintptr_t)(c->texture ? c->texture : dd->font_atlas);
        if (first || tex != bound) {
            id<MTLTexture> live = tex ? w.textures[@((uintptr_t)tex)] : nil;
            [enc setFragmentTexture:live ? live : w.none atIndex:0];
            bound = tex;
            first = false;
        }
        [enc drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:c->index_count indexType:MTLIndexTypeUInt32
                       indexBuffer:slot.indices indexBufferOffset:(NSUInteger)c->index_offset * sizeof(uint32_t)];
    }
}

/* The frame copied out of the drawable into a buffer (nil if there is none to be had). */
static id<MTLBuffer> cui__mtl_copy_out(CUIMetalWindow *w, id<MTLCommandBuffer> cmd, id<CAMetalDrawable> drawable, int fw, int fh) {
    NSUInteger pitch = (NSUInteger)fw * 4;
    id<MTLBuffer> readback = [w.device newBufferWithLength:pitch * (NSUInteger)fh options:MTLResourceStorageModeShared];
    if (readback) {
        id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
        if (!blit) return nil;   /* (no copy: no capture) */
        [blit copyFromTexture:drawable.texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake((NSUInteger)fw, (NSUInteger)fh, 1)
                     toBuffer:readback destinationOffset:0 destinationBytesPerRow:pitch destinationBytesPerImage:pitch * (NSUInteger)fh];
        [blit endEncoding];
    }
    return readback;
}

/* The copy as RGBA, once the frame has run. */
static void cui__mtl_read_back(id<MTLBuffer> readback, int fw, int fh, void *rgba) {
    NSUInteger pitch = (NSUInteger)fw * 4;
    const uint8_t *src = readback.contents;
    uint8_t *dst = rgba;
    for (int y = 0; y < fh; y++, src += pitch)
        for (int x = 0; x < fw; x++, dst += 4) {
            dst[0] = src[x * 4 + 2]; dst[1] = src[x * 4 + 1]; dst[2] = src[x * 4]; dst[3] = 255;
        }
}

static bool cui__mtl_render(cui_gpu_renderer *r, const cui_draw_data *dd, void *rgba) {
    if (!r || !dd) return false;
    @autoreleasepool {
        CUIMetalWindow *w = cui__mtl_win(r);
        int fw = dd->width_px, fh = dd->height_px;
        if (fw <= 0 || fh <= 0) return false;
        cui__mtl_layer(r, w, fw, fh, rgba != NULL);
        CUIMetalSlot *slot = cui__mtl_slot(w);
        bool any = cui__mtl_upload(w, slot, dd);
        bool drawn = any || dd->index_count <= 0 || dd->vertex_count <= 0;   /* false: the vertices found no room */

        id<CAMetalDrawable> drawable = [w.layer nextDrawable];   /* waits for one to be free: the pacing */
        if (!drawable) return false;
        int tw = (int)drawable.texture.width, th = (int)drawable.texture.height;
        id<MTLCommandBuffer> cmd = [w.queue commandBuffer];
        id<MTLRenderCommandEncoder> enc = cui__mtl_encoder(cmd, drawable, dd);
        if (!enc) return false;   /* (no pass: nothing drawn, nothing to show or capture) */
        if (any) cui__mtl_draw(w, enc, slot, dd, tw, th);
        [enc endEncoding];

        id<MTLBuffer> readback = nil;
        if (rgba && tw == fw && th == fh) readback = cui__mtl_copy_out(w, cmd, drawable, fw, fh);
        if (w.layer.presentsWithTransaction) {
            /* the drawable must be scheduled on the GPU before it is presented in the transaction */
            [cmd commit];
            [cmd waitUntilScheduled];
            [drawable present];
        } else {
            [cmd presentDrawable:drawable];
            [cmd commit];
        }
        slot.reader = cmd;
        w.last = cmd;
        if (!readback) return !rgba;
        [cmd waitUntilCompleted];
        /* (a capture of a frame that was not drawn, or that the GPU did not finish, is none) */
        if (!drawn || cmd.status != MTLCommandBufferStatusCompleted) return false;
        cui__mtl_read_back(readback, fw, fh, rgba);
        return true;
    }
}

void cui_gpu_render(cui_gpu_renderer *r, const cui_draw_data *dd) { cui__mtl_render(r, dd, NULL); }
bool cui_gpu_render_capture(cui_gpu_renderer *r, const cui_draw_data *dd, void *rgba) { return rgba && cui__mtl_render(r, dd, rgba); }

#endif
