# Platforms

A build has one window backend, set with `-DCUI_PLATFORM=` (`auto` is Win32 on Windows, Cocoa on macOS and X11 on Linux), and optionally one GPU renderer. `cui_app_backend` returns the backend's name.

| System | Window backend | Renderers | Window position |
|---|---|---|---|
| Windows | `win32` | OpenGL, software, Direct3D 12 (`CUI_D3D12`) | read and set |
| macOS | `cocoa` | OpenGL, software, Metal (`CUI_METAL`) | read and set |
| Linux, X11 | `x11` | OpenGL (GLX), software, Vulkan (`CUI_VULKAN`) | read and set |
| Wayland | `wayland` (SDL2 2.0.22+) | OpenGL, software | compositor controlled |
| Any | `sdl2`, `glfw` | OpenGL, software | as the library gives it |

The GPU renderers are built with CMake only, not in the single-file distribution. `CUI_RENDERER=gpu` picks the GPU renderer at run time; [Performance](performance.md#direct3d-12-vulkan-and-metal-against-opengl) compares them.

`wayland` uses the SDL2 implementation (`cui_app_backend` returns `sdl2`) but requires the Wayland video driver. Use `sdl2` instead if automatic selection or X11 fallback is desired.
