/*
 * CUI renderers.
 *
 * CUI produces a plain list of vertices, indices and draw commands
 * (cui_draw_data). The renderers that ship with the library:
 *
 *   - OpenGL 3.3 core / OpenGL ES 3.0 (cui_gl_*)
 *   - Direct3D 12 on Windows, Vulkan on Linux and Metal on macOS (cui_gpu_*),
 *     in builds with CUI_D3D12, CUI_VULKAN or CUI_METAL
 *   - A portable software rasterizer (cui_sw_*) that renders into any 32 bit
 *     pixel buffer. It needs no GPU, which makes it useful for tests,
 *     screenshots, servers and as a fallback.
 *
 * Writing your own renderer (WebGPU, sokol, bgfx) takes one shader. See
 * docs/rendering.md.
 */
#ifndef CUI_RENDER_H
#define CUI_RENDER_H

#include "cui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- software renderer ---- */

typedef enum cui_sw_format {
    CUI_SW_RGBA = 0,   /* bytes R, G, B, A in memory */
    CUI_SW_BGRA = 1    /* bytes B, G, R, A in memory (Win32 DIBs, X11, Cairo) */
} cui_sw_format;

typedef struct cui_sw_renderer cui_sw_renderer;

CUI_API cui_sw_renderer *cui_sw_create(void);
CUI_API void cui_sw_destroy(cui_sw_renderer *r);
/* Texture callbacks to pass to cui_create / cui_set_renderer. */
CUI_API cui_renderer_iface cui_sw_iface(cui_sw_renderer *r);
/* Renders draw data into a pixel buffer of width x height. stride is in bytes. */
CUI_API void cui_sw_render(cui_sw_renderer *r, const cui_draw_data *dd, void *pixels,
                           int width, int height, int stride, cui_sw_format format);
/* Number of worker threads (1 = single threaded, default: number of cores up to 8). */
CUI_API void cui_sw_set_threads(cui_sw_renderer *r, int threads);
/* Incremental rendering: when the same buffer is passed again, with what the last
 * call drew still in it, only the parts that changed are drawn again. A frame
 * where only a button's hover changed costs that button. Off by default.
 * cui_sw_last_damage gives the rectangle the last call drew (in pixels), so only
 * that part needs copying to the screen; it returns false when nothing changed. */
CUI_API void cui_sw_set_incremental(cui_sw_renderer *r, bool on);
CUI_API bool cui_sw_last_damage(const cui_sw_renderer *r, int *x, int *y, int *w, int *h);
/* The layer cache (cui_set_layer_cache): how many times a group was drawn into its bitmap so
 * far, and the bytes the bitmaps hold now and held at most. At most 4 groups are cached at
 * once, each up to 4,194,304 pixels and 8,388,608 pixels together (32 MB); a group past that
 * is drawn the plain way. A bitmap is freed on the first frame without its group. */
CUI_API void cui_sw_layer_stats(const cui_sw_renderer *r, int *rasters, size_t *bytes, size_t *peak_bytes);

/* Writes a PNG file (RGBA pixels). Returns false on failure. */
CUI_API bool cui_write_png(const char *path, const void *rgba, int width, int height, int stride);
/* The same PNG in memory: a malloc'd buffer the caller frees with free(), and its size in *len; NULL on failure. */
CUI_API void *cui_png_encode(const void *rgba, int width, int height, int stride, int *len);

/* ---- OpenGL renderer ---- */

typedef void *(*cui_gl_loader)(const char *name);
typedef struct cui_gl_renderer cui_gl_renderer;

/* Create after the GL context is current. `loader` resolves GL functions
 * (wglGetProcAddress, glXGetProcAddress, SDL_GL_GetProcAddress,
 * glfwGetProcAddress, ...). Returns NULL if the context lacks GL 3.3 / ES 3. */
CUI_API cui_gl_renderer *cui_gl_create(cui_gl_loader loader);
CUI_API void cui_gl_destroy(cui_gl_renderer *r);
CUI_API cui_renderer_iface cui_gl_iface(cui_gl_renderer *r);
/* Renders into the currently bound framebuffer. Set clear to false when
 * drawing the UI on top of your own scene. */
CUI_API void cui_gl_render(cui_gl_renderer *r, const cui_draw_data *dd, bool clear);
/* Reads the bound framebuffer into a top-down RGBA buffer. */
CUI_API void cui_gl_read_pixels(cui_gl_renderer *r, int width, int height, void *rgba);
/* True when the OpenGL driver draws on the CPU (Mesa llvmpipe, SwiftShader and the
 * like, as in virtual machines and remote sessions). CUI's own software renderer,
 * which redraws only what changed, is then much cheaper. */
CUI_API bool cui_gl_is_software(const cui_gl_renderer *r);

/* ---- Direct3D 12, Vulkan and Metal (built with CUI_D3D12, CUI_VULKAN, CUI_METAL) ---- */

/* The platform's GPU renderer: the draw data the OpenGL renderer takes (vertices, indices,
 * commands), drawn the same way with the same shader, into the window's own swapchain or
 * layer. One per platform: Direct3D 12 on Windows, Vulkan on Linux, Metal on
 * macOS. */
typedef struct cui_gpu_renderer cui_gpu_renderer;

/* Linux: the window cui_gpu_create takes. The app keeps width and height
 * (pixels) current and keeps it alive as long as the renderer. */
typedef struct cui_x11_window {
    void *display;              /* Display * */
    unsigned long window;       /* Window (X11) */
    int width, height;
} cui_x11_window;

/* window: an HWND, on Linux a cui_x11_window *, on macOS an NSWindow * (its
 * content view gets a CAMetalLayer). NULL when the device or window cannot be set up. CUI_GPU
 * set to part of an adapter's name (e.g. "nvidia") picks the GPU. */
CUI_API cui_gpu_renderer *cui_gpu_create(void *window, bool vsync);
CUI_API void cui_gpu_destroy(cui_gpu_renderer *r);
CUI_API cui_renderer_iface cui_gpu_iface(cui_gpu_renderer *r);
/* Draws and presents. */
CUI_API void cui_gpu_render(cui_gpu_renderer *r, const cui_draw_data *dd);
/* Draws, reads the frame back into rgba (width_px * height_px * 4, top-down) and presents. */
CUI_API bool cui_gpu_render_capture(cui_gpu_renderer *r, const cui_draw_data *dd, void *rgba);

#ifdef __cplusplus
}
#endif

#endif /* CUI_RENDER_H */
