# Integration

`cui_app` is optional. The core of CUI is a pure library: you give it input and a viewport size, it gives you vertices. That makes it easy to put CUI inside a game engine, an existing app or a custom window, on any graphics API.

## The core loop

```c
#include <cui/cui.h>
#include <cui/cui_render.h>

cui_gl_renderer *gl = cui_gl_create(get_proc_address);          /* after your GL context is current */
cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_gl_iface(gl) });

for (;;) {
    /* 1. forward input from your window system */
    cui_input_mouse_move(ui, mouse_x_points, mouse_y_points);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, down);
    cui_input_scroll(ui, 0, wheel_lines);
    cui_input_key(ui, CUI_KEY_ENTER, pressed, mods);
    cui_input_text(ui, utf8_from_ime);

    /* 2. build the UI */
    cui_set_viewport(ui, width_points, height_points, pixels_per_point);
    cui_begin_frame(ui, seconds_since_last_frame);
    my_ui(ui);
    cui_end_frame(ui);

    /* 3. render */
    cui_gl_render(gl, cui_get_draw_data(ui), false);   /* false: draw over your scene */
    swap_buffers();

    /* 4. sleep until something happens (optional) */
    if (!cui_needs_redraw(ui)) wait_for_events(cui_next_wakeup(ui));   /* seconds, or -1 for no timer */
}
```

Coordinates are in points (logical pixels). `pixels_per_point` is 2 on a typical retina screen. Input events can arrive at any time between frames; CUI collects them for the next frame.

### Input reference

| Function | Notes |
|---|---|
| `cui_input_mouse_move(ui, x, y)` | Points, relative to the viewport |
| `cui_input_mouse_button(ui, button, down)` | `CUI_MOUSE_LEFT`, `RIGHT`, `MIDDLE` |
| `cui_input_mouse_leave(ui)` | The pointer left the window |
| `cui_input_scroll(ui, dx, dy)` | In lines. Positive `dy` is the wheel moving up, toward the top of the content. Trackpads: pixels / 44 |
| `cui_input_key(ui, key, down, mods)` | `CUI_KEY_*` and `CUI_MOD_SHIFT`, `CTRL`, `ALT`, `SUPER`. Send key repeats as additional presses |
| `cui_input_text(ui, utf8)` | Typed characters after keyboard layout and IME processing |
| `cui_input_focus(ui, focused)` | Window focus changes |

### Platform services

```c
cui_set_platform(ui, &(cui_platform_iface){
    .user = my_window,
    .set_clipboard = my_set_clipboard,           /* (void *user, const char *utf8) */
    .get_clipboard = my_get_clipboard,           /* returns a string valid until the next call */
    .set_cursor = my_set_cursor,                 /* CUI_CURSOR_ARROW, HAND, TEXT, RESIZE_EW... */
    .set_text_input_rect = my_set_ime_rect,      /* where the IME candidate window should go */
});
```

Useful queries for your app: `cui_wants_keyboard` (a text field has focus), `cui_wants_mouse` (the pointer is over the UI), `cui_get_cursor`.

## Using the built-in backends with your loop

The SDL2 and GLFW backends are complete `cui_app` implementations (`-DCUI_PLATFORM=sdl2` or `glfw`). Read `src/platform/cui_app_sdl2.c` for a compact, real example of the integration above: about 250 lines including the key map.

## Renderers

### OpenGL

`cui_gl_create(loader)` works with OpenGL 3.3 core and OpenGL ES 3.0 contexts (detected automatically). No GL headers or loader library are needed; pass `SDL_GL_GetProcAddress`, `glfwGetProcAddress`, `wglGetProcAddress` or similar. It returns NULL when the context is too old.

`cui_gl_render` sets its own blend, scissor and viewport state and leaves depth testing and face culling off. Restore your state afterwards if your renderer relies on it.

### Software

```c
cui_sw_renderer *sw = cui_sw_create();
cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(sw) });
...
cui_sw_render(sw, cui_get_draw_data(ui), pixels, width, height, stride_bytes, CUI_SW_BGRA);
```

Renders into any 32 bit buffer, using several threads. Output matches the GPU renderer to within a fraction of a percent. Great for servers, tests, thumbnails and embedded framebuffers. `cui_write_png` saves a buffer.

### Your own renderer

You need two things:

1. **Textures.** Implement the three callbacks in `cui_renderer_iface`: create (R8 for the font atlas, RGBA8 for images), update a sub-rectangle, destroy. Return any nonzero handle.
2. **One draw call per command.** For each `cui_draw_cmd` set the scissor rectangle to `clip` (pixels, top left origin), bind `texture` (0 means `font_atlas`) and draw `index_count` indices starting at `index_offset` as triangles. Blend with premultiplied alpha: `src * 1 + dst * (1 - src_alpha)`.

The vertex format and the reference shader are in [rendering.md](rendering.md). Porting them to Vulkan, Metal, Direct3D 11/12, WebGPU, sokol or bgfx is mostly syntax.

## Threads

A `cui_ctx` is not thread safe; use it from one thread. You may create several contexts (for several windows) and use each from its own thread. The software renderer parallelizes internally.

## Memory

All allocations go through `cui_ctx_desc.allocator` if you provide one. Per frame data lives in an arena that is reused, so a steady UI does not allocate. Component state is freed automatically a few seconds after the component disappears.

## Binary size

CUI is built to stay small. Measured with GCC, `-DCMAKE_BUILD_TYPE=MinSizeRel`, stripped, x86-64:

| | Size |
|---|---|
| Whole library (every component, both renderers, X11 backend, fonts) | about 590 KB |
| of which embedded fonts (Inter, Geist, Geist Mono, 168 Lucide icons) | 95 KB, compressed |
| `examples/hello` on Linux, whole executable | about 240 KB |
| `examples/hello.exe` on Windows (MinGW), whole executable | about 380 KB |
| `examples/assistant`, an AI chat app | about 330 KB |
| `examples/gallery`, every component in use | about 690 KB |

What keeps it small:

- The library is compiled with one section per function, so the linker can drop everything your app never calls. Turn that on in your own executable for release builds: `-Wl,--gc-sections` (GCC and Clang on Linux, MinGW), `-Wl,-dead_strip` (macOS), `/OPT:REF /OPT:ICF` (MSVC). The examples already do.
- The embedded fonts are stored as DEFLATE streams (about 48% smaller) and unpacked into memory once, when a context is created (about 1.5 ms).
- Image decoding covers PNG and JPEG only. `-DCUI_IMAGE_ALL_FORMATS=ON` adds BMP, GIF and TGA.

Going smaller:

| Option | Saves |
|---|---|
| `-DCUI_DEFAULT_FONTS=OFF` (load your own fonts with `cui_font_load_file`) | 95 KB plus the decoder |
| `-DCUI_STB_IMAGE=OFF` (upload pixels yourself with `cui_image_create`) | about 24 KB |
| `-DCUI_WITH_APP=OFF` (core only, for use inside your engine) | the window layer and dialogs |
