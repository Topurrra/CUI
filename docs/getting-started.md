# Getting started

This guide takes you from an empty folder to a working window, then explains the ideas you need to build real screens.

## 1. Get CUI

CUI is a regular CMake project with no external dependencies. You need a C99 compiler and CMake 3.14 or newer:

| System | Install |
|---|---|
| Windows | Visual Studio 2019 or newer with "Desktop development with C++" (it includes CMake), or MinGW-w64 and CMake |
| macOS | `xcode-select --install`, then CMake (`brew install cmake`) |
| Debian, Ubuntu | `sudo apt install build-essential cmake libx11-dev` |
| Fedora | `sudo dnf install gcc cmake libX11-devel` |
| Arch | `sudo pacman -S base-devel cmake libx11` |

To check that everything works, build CUI and run the smallest example:

```sh
git clone https://github.com/yappologistic/CUI.git
cd CUI
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/examples/hello                 # Windows with Visual Studio: build\examples\Release\hello.exe
```

`build/examples/` also holds the gallery, which shows every component, and complete apps (mail, notes, music, a dashboard). If something fails, see [Troubleshooting](#troubleshooting).

### With CMake (recommended)

```cmake
cmake_minimum_required(VERSION 3.14)
project(my_app C)

add_subdirectory(CUI)
add_executable(my_app main.c)
target_link_libraries(my_app PRIVATE cui::cui)
```

Or fetch it at configure time:

```cmake
include(FetchContent)
FetchContent_Declare(cui GIT_REPOSITORY https://github.com/yappologistic/CUI.git GIT_TAG master)
FetchContent_MakeAvailable(cui)
target_link_libraries(my_app PRIVATE cui::cui)
```

On Windows, mark GUI apps with `WIN32` (`add_executable(my_app WIN32 main.c)`) so no console window opens. Keep `int main(void)`; with MSVC add `/ENTRY:mainCRTStartup` to the link options (see `examples/CMakeLists.txt`).

### Without CMake

Compile every `.c` file in `src/`, plus `src/render/*.c`, `src/platform/cui_app.c` and the backend for your platform:

| Platform | Backend file | Link with |
|---|---|---|
| Windows | `src/platform/cui_app_win32.c` | `user32 gdi32 opengl32 shell32 imm32 dwmapi` |
| macOS | `src/platform/cui_app_cocoa.m` (compile with `-fobjc-arc`) | `-framework Cocoa -framework QuartzCore -framework OpenGL` |
| Linux | `src/platform/cui_app_x11.c` | `-lX11 -ldl -lm -lpthread` |

```sh
cc -std=c99 -O2 -Iinclude main.c src/*.c src/render/*.c src/platform/cui_app.c src/platform/cui_app_x11.c \
   -lX11 -ldl -lm -lpthread -o my_app
```

### Options

| CMake option | Default | Meaning |
|---|---|---|
| `CUI_PLATFORM` | `auto` | `win32`, `cocoa`, `x11`, `sdl2` or `glfw` |
| `CUI_WITH_APP` | `ON` | Build the window layer. Turn off to use only the core in your engine |
| `CUI_DEFAULT_FONTS` | `ON` | Embed Inter, Geist and Lucide (about 180 KB) |
| `CUI_STB_IMAGE` | `ON` | PNG and JPEG loading for `cui_image_load` |
| `CUI_BUILD_SHARED` | `OFF` | Build a shared library |
| `CUI_BUILD_EXAMPLES`, `CUI_BUILD_TESTS` | `ON` at top level | |

Cross compiling for Windows from Linux works with the included toolchain file:

```sh
cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake
```

## 2. Your first window

```c
#include <cui/cui.h>
#include <cui/cui_app.h>

int main(void) {
    cui_app *app = cui_app_create(&(cui_app_desc){ .title = "My app", .width = 900, .height = 600 });
    if (!app) return 1;

    while (cui_app_frame(app)) {
        cui_ctx *ui = cui_app_ui(app);
        cui_column(ui, .pad = 24, .gap = 12) {
            cui_heading(ui, "Welcome", 2);
            cui_text(ui, "This text wraps to the width of the window.");
        }
    }
    cui_app_destroy(app);
    return 0;
}
```

`cui_app_frame` presents the previous frame, sleeps until something happens (input, an animation, a timer) and starts the next frame. Everything between two calls describes the screen.

If you prefer callbacks, `cui_app_run` does the loop for you and also gives smooth redraws while the user resizes the window on Windows and macOS:

```c
static void frame(cui_ctx *ui, void *user) { /* ... */ }

int main(void) {
    return cui_app_run(&(cui_app_desc){ .title = "My app" }, frame, NULL);
}
```

## 3. How immediate mode works

You do not create widgets and keep pointers to them. You call a function every frame, and it returns what happened since the last frame:

```c
static bool dark = false;
static float volume = 40;
static char name[64] = "";

if (cui_button(ui, "Save", .variant = CUI_BUTTON_PRIMARY)) save();   /* true once, when clicked */
cui_switch(ui, "Dark mode", &dark);                                   /* toggles your bool */
cui_slider(ui, "Volume", &volume, 0, 100, .show_value = true);        /* writes your float */
if (cui_input(ui, "name", name, sizeof name, .label = "Name") & CUI_CHANGED) validate(name);
```

Your variables are the source of truth. There is nothing to synchronise, and conditional UI is just an `if`.

CUI keeps small amounts of state for you behind the scenes (hover animations, scroll offsets, caret positions), keyed by an id that comes from the label. Two components with the same label in the same scope share an id. If you need two buttons called "Edit", give them different keys with `##`:

```c
cui_button(ui, "Edit##profile");   /* shows "Edit" */
cui_button(ui, "Edit##billing");   /* shows "Edit" */
```

Or wrap repeated items in an id scope, which is what you want in loops:

```c
for (int i = 0; i < count; i++) {
    cui_push_id_int(ui, i);
    if (cui_button(ui, "Delete")) remove_item(i);
    cui_pop_id(ui);
}
```

## 4. Named arguments

Every component that has options takes them as named arguments after the required ones. Anything you leave out has a sensible default.

```c
cui_button(ui, "Export", .icon = CUI_ICON_DOWNLOAD, .size = CUI_SIZE_SM, .tooltip = "Export as CSV");
```

Under the hood this is a C99 compound literal: `cui_button(ui, label, ...)` expands to `cui_button_ex(ui, label, &(cui_button_opts){ ... })`. From C++ (C++11 or later; the headers are `extern "C"`), or if you want to build options at runtime, call the `_ex` function with a filled struct. C++20 designated initializers make that nearly as short.

## 5. Containers

Containers take a block. The block runs once, between the container's begin and end:

```c
cui_card(ui, .gap = 12) {
    cui_label(ui, "Plan");
    cui_row(ui, .gap = 8) {
        cui_badge(ui, "Pro", .tone = CUI_TONE_ACCENT);
        cui_spacer(ui);
        cui_button(ui, "Change plan", .size = CUI_SIZE_SM);
    }
}
```

Do not `return`, `break` or `goto` out of a block: the end would not run. If you need early exits, use the explicit functions (`cui_card_begin` and `cui_card_end`). Overlays use the same shape and only run their block while visible:

```c
static bool open;
if (cui_button(ui, "Delete")) open = true;
cui_dialog(ui, "Delete project?", &open, .description = "This cannot be undone.") {
    cui_dialog_footer(ui) {
        if (cui_button(ui, "Cancel")) open = false;
        if (cui_button(ui, "Delete", .variant = CUI_BUTTON_DANGER)) { delete_project(); open = false; }
    }
}
```

Read [Layout](layout.md) next. It explains sizes, alignment and scrolling in detail.

## 6. Keyboard shortcuts

```c
if (!cui_wants_keyboard(ui)) {                          /* skip while the user types in a field */
    if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_K)) palette_open = true;   /* Ctrl+K, Cmd+K on macOS */
}
```

## 7. Files and links

```c
char path[1024];
if (cui_app_file_dialog(app, &(cui_file_dialog_opts){ .kind = CUI_FILE_OPEN, .filter_name = "Images", .filter = "png;jpg" },
                        path, sizeof path))
    open_image(path);
cui_app_open_url("https://example.com");
```

Native dialogs on Windows and macOS; on Linux `zenity` or `kdialog` when installed. Kinds: `CUI_FILE_OPEN`, `CUI_FILE_SAVE`, `CUI_FILE_FOLDER`. Inside a `cui_app_run` callback, pass `NULL` for the app.

## 8. Screenshots and tests

Any CUI app can save a screenshot and exit, which is handy for docs and visual tests:

```sh
CUI_SCREENSHOT=out.png CUI_SCREENSHOT_FRAMES=60 ./my_app
```

For unit tests, skip the window entirely: create a context with the software renderer, feed input with `cui_input_*` and render into memory. `tests/test_main.c` shows the pattern.

## Run in the browser

CUI compiles to WebAssembly with plain clang: no Emscripten and no downloads. The [live docs](https://yappologistic.github.io/CUI/) are built this way, and your app can be too, unchanged, as long as its `main` hands its frame function to `cui_app_run` (as in the example above) instead of running its own loop. You need clang with the `wasm32` target, `wasm-ld`, `llvm-ar` and python3 (on Debian or Ubuntu: `sudo apt install clang lld llvm python3`).

```sh
web/tools/build.sh app path/to/my_app.c    # more .c files may follow
web/tools/serve.sh                          # then open http://localhost:8765/#/app/my_app
```

The app runs on the docs site's page, beside its source, with the theme, renderer and width controls. `web/site/` is plain static files, so any web server can host it. The page draws with WebGL2, or with CUI's software renderer. Two things of the app layer do nothing in the browser yet: `cui_app_file_dialog` (it returns false) and `cui_app_open_url`. [`web/README.md`](../web/README.md) explains the pieces.

Android and iOS are not supported yet. Each needs its own platform layer (an `android_native_app_glue` or UIKit window with touch input and an OpenGL ES or Metal renderer). The core already handles touch-sized layouts, phone widths and the software renderer, so it is a contained piece of work; until then, [Integration](integration.md) shows how to drive CUI from a window and renderer you provide.

## Troubleshooting

| Problem | Fix |
|---|---|
| CMake says `Could NOT find X11` | Install the X11 headers: `libx11-dev` (Debian, Ubuntu), `libX11-devel` (Fedora), `libx11` (Arch) |
| `undefined reference to XOpenDisplay` (building without CMake) | Put `-lX11 -ldl -lm -lpthread` after the source files on the command line |
| No window on Wayland without XWayland | Configure with `-DCUI_PLATFORM=sdl2` or `-DCUI_PLATFORM=glfw` (install `libsdl2-dev` or `libglfw3-dev`) |
| Black or empty window, OpenGL errors (virtual machines, remote desktop, old drivers) | CUI falls back to its software renderer when OpenGL 3.3 cannot start; to force it, run with `CUI_RENDERER=software` |
| Everything looks too small or too large | Run with `CUI_SCALE=1.5` (or any scale) to override what the display reports |
| A console window opens next to the app (Windows) | Use `add_executable(my_app WIN32 main.c)`, and with MSVC link with `/ENTRY:mainCRTStartup` |
| `cui_app_cocoa.m` fails to compile (macOS, without CMake) | Compile it with `-fobjc-arc` and link the Cocoa, QuartzCore and OpenGL frameworks |
| Text is missing | With `CUI_DEFAULT_FONTS=OFF` there are no built-in fonts: load your own (see [Theming](theming.md)) |

Still stuck? [Open an issue](https://github.com/yappologistic/CUI/issues) with your system, compiler, the CMake command and its full output.

## Environment variables

| Variable | Effect |
|---|---|
| `CUI_RENDERER=software` | Use the software renderer even when OpenGL is available |
| `CUI_RENDERER=opengl` | Keep OpenGL even when it is drawn on the CPU (without it, X11 and Win32 apps on a machine with no GPU switch to the software renderer, which costs several times less there) |
| `CUI_SCALE=2` | Override the display scale |
| `CUI_THEME=dark` | Start in the dark (or `light`) theme, whatever the app asks for |
| `CUI_ACCENT=blue` | Start with another accent: `neutral`, `violet`, `blue`, `green`, `amber`, `orange`, `coral`, `rose` |
| `CUI_SCREENSHOT=file.png` | Save a screenshot after `CUI_SCREENSHOT_FRAMES` frames (default 90) and quit |
