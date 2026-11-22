# Failed performance experiments

Things built to lower the cost of drawing that were measured and did not. They are recorded so they are not built twice. Unless a section says otherwise, the numbers are from the forms benchmark (a five-panel forms app driven by a fixed script of hovers, presses, panel changes, scrolling, dialogs and typing; not part of this repository): the same session in every entry, per-process counters sampled outside the app, the idle desktop as baseline. CPU is percent of one core, GPU percent of the GPU, and "together" is the app plus the system compositor less the baseline.

All of this code was removed on 2026-11-17 and was never committed. What did lower the cost is in [Performance and memory](performance.md#direct3d-12-vulkan-and-metal-against-opengl).

## Instanced shape rendering

**What it was.** A second path through the library. In place of four vertices per shape, CUI wrote one 36-byte record per rect, glyph, shadow, stroke segment, arc and triangle (positions in whole pixels) into mapped GPU memory, batched by layer and kind and drawn with one instanced draw per kind, so a frame was a few dozen draws. Because a kind's shapes draw together, components declared draw layers for whatever had to cover an earlier shape. Lines and arcs became distance fields in the shader. Built for OpenGL, Direct3D 12, Vulkan and Metal.

**Results.** One pass of the 120 s session per row, the display at 60 Hz. First OpenGL with and without it, in the same window:

| Machine | Entry | Frames | App CPU | Compositor CPU | App GPU | App RAM peak MB | App VRAM MB |
|---|---|---|---|---|---|---|---|
| 5950X-6650XT-Win, 1000 x 720 | vertices | 7201 | 3.23 | 6.02 | 1.35 | 97 | 27 |
| | instanced | 7211 | 3.31 | 5.84 | 1.54 | 155 | 23 |
| 12900KF-2080Ti-Win, 1500 x 1080 | vertices | 7122 | 2.16 | 4.34 | 0.95 | 73 | 33 |
| | instanced | 7126 | 2.02 | 3.99 | 1.77 | 123 | 32 |
| HX370-5070-Ubuntu, Wayland, 1000 x 720 | vertices | 7090 | 3.67 | 10.1 | | 136 | 10 |
| | instanced | 7097 | 3.69 | 10.5 | | 189 | 7 |
| M5-M5-macOS, 2000 x 1440 | vertices | 6257 | 8.70 | 43.1 | | 93 | 104 |
| | instanced | 6256 | 8.09 | 43.7 | | 94 | 104 |
| SC8280XP-A690-Arch, Wayland, 1000 x 720 | vertices | 5049 | 2.47 | 43.0 | 0.41 | 57 | 22 |
| | instanced | 5045 | 2.47 | 42.6 | 0.32 | 64 | 22 |

The M5-M5-macOS and HX370-5070-Ubuntu were busy during this run (the idle baseline already had the window server at 43% of a core on one and the GPU at 41% on the other), and the SC8280XP-A690-Arch's rows had the sampler fault described below; both entries of a machine were affected alike.

Then the same API with CUI's vertices and with instances, differing in nothing else:

| Machine | Entry | Frames | App CPU | Compositor CPU | App GPU | App RAM peak MB | App VRAM MB |
|---|---|---|---|---|---|---|---|
| 5950X-6650XT-Win, Direct3D 12 | vertices | 7104 | 1.84 | 0.78 | 3.34 | 67 | 14 |
| | instanced | 7102 | 1.91 | 0.63 | 3.42 | 68 | 14 |
| 5950X-6650XT-Win, Direct3D 12, the other order | instanced | 7099 | 1.89 | 0.58 | 3.31 | 69 | 14 |
| | vertices | 7101 | 1.82 | 0.59 | 3.30 | 67 | 14 |
| M5-M5-macOS, Metal | vertices | 7118 | 5.64 | 15.8 | 2.33 (all) | 90 | 83 |
| | instanced | 7117 | 5.64 | 16.4 | 1.78 (all) | 90 | 80 |
| SC8280XP-A690-Arch, Vulkan on Wayland | vertices | 7107 | 2.91 | 16.5 | 0.67 | 20 | 13 |
| | instanced | 7106 | 2.94 | 16.6 | 0.61 | 19 | 14 |

Baselines for the second table: compositor CPU 0.19 (5950X-6650XT-Win), 0.87 (M5-M5-macOS), 0.33 (SC8280XP-A690-Arch).

**Why it failed.** Level with the vertex path on CPU and GPU on every machine and API; with OpenGL it took about 50 MB more memory on three of the five machines (not looked into). The vertex path already shades every rounded rect, border, shadow, glyph and image per pixel from one quad (its vertices carry the size and radii, and the fragment shader computes the distance). So instancing changed only how the same numbers reach the shader (36 bytes once, not about 250 in four vertices and six indices), how draws are grouped and who tessellates lines and arcs, none of it measurable at this UI size. The earlier gains with the instanced Direct3D 12 and Metal renderers came from how those APIs present a window, which the vertex path gets too ([Performance](performance.md#direct3d-12-vulkan-and-metal-against-opengl)). Against that stood a second path through every drawing function, layers every component had to declare, and rules a reviewer had to check by hand.

Not established: whether it gains with far more shapes than these sessions draw (the gallery's fullest pages are a few thousand), and why OpenGL took more memory with it.

## Partial redraw

**What it was.** The instanced renderers drew and presented only what had changed. Each frame's shape records were hashed in draw order into 32 pixel tiles; differing tiles became one box (four at first), and a frame that changed nothing was neither drawn nor presented. A back buffer two or three frames old was brought up to date by clearing and drawing the boxes it had missed. Direct3D 12 used `Present1` with dirty rectangles on a flip-sequential swapchain, Vulkan `VK_KHR_incremental_present`, and OpenGL drew into a texture it kept. It was off unless asked for.

**Results.** The gallery session (105 s of replayed pointer input over every page), two or three rounds, the display at its own rate:

| Machine, renderer | | App CPU | Compositor CPU | App GPU | Compositor GPU |
|---|---|---|---|---|---|
| HX370-890M-Arch, Vulkan through Xwayland, 1860 x 1230 | off | 3.09 | 6.99 | 4.18 | 5.50 |
| | on | 3.57 | 6.85 | 2.59 | 3.59 |
| 5950X-6650XT-Win, Direct3D 12, 1240 x 820 | off | 3.10 | 0.56 | 3.60 | 0.00 |
| | on | 3.41 | 0.69 | 3.19 | 0.00 |
| 5950X-6650XT-Win, instanced OpenGL | off | 4.28 | 5.45 | 2.23 | 2.36 |
| | on | 4.63 | 5.11 | 1.82 | 2.28 |
| M4Max-M4Max-macOS, Metal, 2480 x 1640 at 120 Hz | off | 16.5 | 47.5 | 6.90 (all) | |
| | on (unchanged frames left out) | 16.8 | 45.6 | 6.89 (all) | |

In that session 14% of the frames changed nothing, and a frame that did drew 40% of the window (21% had really changed; the rest was what an older back buffer had missed). In the forms session (the scroll copy table below) it was level with drawing whole on 5950X-6650XT-Win and half a point less GPU on HX370-890M-Arch.

**Why it was removed.** It did what it was for where the GPU is scarce: 38% less GPU in the app and 35% less in the compositor on an integrated GPU at a high resolution. On a desktop card it was 11 to 18% less of a load that is next to nothing, and everywhere it cost 0.3 to 0.5 points of a core, since the shapes had to stay in CUI's memory to be compared and were then copied. It compared shape records, so it went with the instanced path. The same idea for the vertex path was not tried: the software renderer already hashes tiles from the triangles over them, and a GPU renderer could take its damage the same way.

Not established: what one damage box costs against four (in the forms session a drawn frame covered 43% of the window with one and 30% with four; the cost was not measured), and whether Direct3D 12 needs to draw what an older buffer missed (`Present1` with dirty rectangles appeared to bring the buffers up to date itself).

## Scroll copy

**What it was.** With partial redraw a scroll makes the whole scroll area damage, though most of its pixels only moved. Scroll copy copied the area from the last frame's back buffer, offset by the scroll, and drew only the strip that came into view and what had not moved with the content (scrollbars, hover, popups). Built for Direct3D 12 and for Vulkan on X11 and Wayland.

**Results.** The 90 s session, three interleaved rounds.

| Machine | Entry | App CPU | Compositor CPU | App GPU | Compositor GPU | App VRAM MB | Late, worst ms | CPU together | GPU together |
|---|---|---|---|---|---|---|---|---|---|
| 5950X-6650XT-Win, Direct3D 12, 1000 x 720 | drawn whole | 1.74 | 0.48 | 3.06 | 0.07 | 14.4 | 30 | 2.2 | 3.1 |
| | partial redraw | 1.77 | 0.44 | 2.77 | 0.00 | 14.4 | 18 | 2.2 | 2.8 |
| | scroll copy | 1.63 | 0.57 | 2.68 | 0.00 | 14.4 | 33 | 2.2 | 2.7 |
| HX370-890M-Arch, Vulkan on Wayland, 1500 x 1080 | drawn whole | 1.55 | 6.08 | 3.88 | 1.74 | 27.8 | 17 | 7.4 | 5.6 |
| | partial redraw | 2.05 | 6.41 | 3.31 | 1.74 | 27.0 | 12 | 8.2 | 5.1 |
| | scroll copy | 2.08 | 5.47 | 3.56 | 1.91 | 27.0 | 17 | 7.3 | 5.5 |
| SC8280XP-A690-Arch, Vulkan on Wayland, 1000 x 720 | drawn whole (two rounds) | 2.02 | 40.4 | 0.36 | 0.33 | | 2216 | 42.3 | 0.7 |
| | partial redraw | 2.11 | 37.8 | 0.24 | 0.61 | | 2073 | 39.8 | 0.9 |
| | scroll copy | 2.09 | 37.1 | 0.30 | 0.29 | | 2097 | 39.1 | 0.6 |

Baselines: compositor CPU 0.01 (5950X-6650XT-Win), 0.22 (HX370-890M-Arch), 0.07 (SC8280XP-A690-Arch); compositor GPU 0. The SC8280XP-A690-Arch's rows, here and in the next table, had the sampler fault described at the end: its compositor CPU and lateness are the sampler's, alike in every entry. On X11 (through Xwayland) the comparison gave the same answer, but that path presented a different number of frames with and without the copy, so its rows are not the same work.

**Why it failed.** Level with partial redraw on every machine: the differences are inside the spread between rounds (on HX370-890M-Arch, compositor CPU ranged over 1.6 points for one entry). Three causes, the first two established and the third likely:

- The session scrolls fast, about 120 pixels a frame, a ninth of the viewport, so the strip that comes into view is large in every scrolling frame.
- The copy still touches every pixel of the area, and the copied area is still presented as changed.
- On a desktop card at this size the GPU's cost follows the number of frames, not the pixels in them: drawing whole and drawing a tenth cost about the same.

Not established: whether a slow scroll gains (not measured), and why 20 of 226 frames differed from a full redraw by 1 to 26 pixels at a display scale of 1.5 (none at a scale of 1). The Vulkan path also read a swapchain image after presenting it, which the specification does not allow; it worked on the three drivers tried.

## A scroll surface moved by the compositor

**What it was.** The scroll area's content was drawn into a surface of its own, taller than the viewport, which the system compositor showed through a hole in the window. A scroll was then a new offset for the compositor and no drawing by the app until the content ran out. Built for Direct3D 12 with DirectComposition and for Vulkan with a Wayland subsurface cropped by a viewport.

**Results.** The same session and rounds; the other entries are in the table above.

| Machine | Entry | App CPU | Compositor CPU | App GPU | Compositor GPU | App VRAM MB | Late, worst ms | CPU together | GPU together |
|---|---|---|---|---|---|---|---|---|---|
| 5950X-6650XT-Win, Direct3D 12 | scroll surface, as first built | 1.14 | 2.76 | 1.00 | 1.05 | 29.7 | 12762 | 3.9 | 2.1 |
| | scroll surface, pacing changed (one run) | 1.77 | 5.23 | 0.98 | 1.94 | | 400 | 7.0 | 2.9 |
| HX370-890M-Arch, Vulkan on Wayland | scroll surface | 2.59 | 6.39 | 2.77 | 3.77 | 99.6 | 19 | 8.8 | 6.5 |
| SC8280XP-A690-Arch, Vulkan on Wayland | scroll surface | 2.65 | 38.0 | 0.42 | 0.41 | | 1539 | 40.6 | 0.8 |

On HX370-890M-Arch the compositor's video memory rose from 471 MB to 608 MB with it.

**Why it failed.** The work moves into the compositor and does not go away. The app's GPU share fell (to a third on Windows, by 0.8 of a point on HX370-890M-Arch) and the compositor's rose by more: dwm went from nothing to 1 to 2% of the GPU and from half a percent of a core to 3 to 5, and gnome-shell's GPU share doubled. Add the surface's own memory (15 MB of video memory on Windows; 72 MB in the app plus about 140 MB in the compositor on HX370-890M-Arch) and the CPU to build the rows held past the viewport. On Direct3D 12 its pacing was never settled: the first build ran up to 12 s behind the script, a later one 0.4 s in a clean run and 3.6 s on a busy desktop. Not tried: a scroll the compositor runs by itself, with no app frames during a fling, which is what a surface is for.

## Drawing only the damage on Metal

**What it was.** Partial redraw on Direct3D 12 and Vulkan kept a back buffer's contents and drew only the changed box into it. A second mode did the same on Metal; the default there only left out the frames that changed nothing.

**Results.** The gallery session, three rounds, M4Max-M4Max-macOS, 2480 x 1640 at 120 Hz; the GPU figure is the whole machine's.

| Entry | App CPU | WindowServer CPU | GPU, all |
|---|---|---|---|
| drawn whole | 16.5 | 47.5 | 6.90 |
| unchanged frames left out | 16.8 | 45.6 | 6.89 |
| only the damage drawn | 17.2 | 46.9 | 9.26 |

**Why it failed.** 34% more GPU, not less. Apple's GPUs render in tiles, and a pass that keeps its target's contents loads them into tile memory first, which a clearing pass does not; loading the frame costs more than drawing the UI again. This is the documented behaviour of load actions on these GPUs; it was not confirmed with a GPU trace.

## Vulkan on macOS through MoltenVK

**What it was.** The Vulkan renderer on macOS: a `CAMetalLayer` on the window, a surface from `VK_EXT_metal_surface`, and MoltenVK translating to Metal. The renderer needed no change.

**Results.** One pass of the 120 s session, M5-M5-macOS, 2000 x 1440; the GPU figure is the whole machine's. Baseline WindowServer CPU 1.60.

| Entry | Frames | App CPU | WindowServer CPU | GPU, all | GPU-visible memory MB | CPU together |
|---|---|---|---|---|---|---|
| Metal | 7118 | 5.78 | 15.3 | 1.33 | 82 | 19.5 |
| Vulkan through MoltenVK | 7114 | 9.28 | 19.7 | 10.0 | 119 | 27.4 |

The pictures were byte-identical.

**Why it failed.** A translation layer over Metal costs CPU in the app and the window server and several times the GPU load for the same frames. One possible part of the GPU difference was not tested: the Vulkan renderer asks for readable swapchain images, so MoltenVK's drawables were not framebuffer-only, while the Metal renderer's are. Single pass; the difference is far outside what a pass varies by.

## Vulkan on Windows

**What it was.** The Vulkan renderer on a Win32 window, beside Direct3D 12.

**Results.** One pass of the 120 s session on each machine. Baseline dwm CPU 0.39 (5950X-6650XT-Win) and 0.97 (12900KF-2080Ti-Win).

| Machine | Entry | App CPU | dwm CPU | App GPU | App RAM peak MB | App VRAM MB |
|---|---|---|---|---|---|---|
| 5950X-6650XT-Win, 1000 x 720 | Direct3D 12 | 1.72 | 0.59 | 2.91 | 69 | 14 |
| | Vulkan | 2.85 | 0.60 | 2.35 | 110 | 38 |
| 12900KF-2080Ti-Win, 1500 x 1080 | Direct3D 12 | 3.36 | 7.71 | 3.45 | 86 | 37 |
| | Vulkan | 3.57 | 7.61 | 4.47 | 155 | 64 |

**Why it failed.** The frames and the compositor's cost were the same, but Vulkan took 40 to 70 MB more memory and about 25 MB more video memory on both cards, and 1.1 points more CPU on the AMD card. The GPU share went either way (a point or less, single passes). Nothing gained would pay for a second renderer on Windows, so Direct3D 12 is the Windows renderer and Vulkan is for Linux.

## Writing draw data straight into GPU memory

**What it was.** The renderer lent the draw list a region of its mapped upload buffer before each frame, so vertices and indices were written straight into GPU memory and the per-frame copy from the heap buffer went away. Three lent buffers in rotation, each twice the last frame's size; a frame that outgrew its region moved to a heap buffer and was copied as before. Run on Direct3D 12 and Vulkan; the Metal version was never built.

**Results.** One pass of the 120 s session per row, the same binary with the copy and with direct writes. Every frame of the direct rows was written direct.

| Machine | Entry | Frames | App CPU | Compositor CPU | App GPU | App RAM peak MB | App VRAM MB |
|---|---|---|---|---|---|---|---|
| 5950X-6650XT-Win, Direct3D 12 | Copy | 7100 | 1.91 | 0.68 | 3.29 | 67.1 | 14.4 |
| | Direct | 7099 | 1.90 | 0.56 | 3.33 | 66.7 | 14.4 |
| | Direct, again | 7100 | 1.82 | 0.65 | 3.34 | 66.7 | 14.4 |
| SC8280XP-A690-Arch, Vulkan on Wayland | Copy | 7114 | 2.89 | 16.7 | 0.67 | 30.2 | 13.1 |
| | Direct | 7117 | 2.92 | 17.0 | 0.67 | 30.2 | 14.0 |

**Why it failed.** A frame of the session is about 100 KB of vertices and indices (148 KB at most), so the copy is about 6 MB a second at 60 Hz, which no counter can see. The heap buffer it removes is a few hundred KB, and the rotating GPU buffers that replace it are about as large: 0.4 MB less memory on 5950X-6650XT-Win, nothing on the SC8280XP-A690-Arch. It also put a hook into the draw list for the renderer to reach, for no gain.

## A lag that was the benchmark's

On the SC8280XP-A690-Arch every renderer ran about two seconds behind the session with the compositor at 35 to 45% of a core, and for a day that was taken for a cost of drawing there. It was the sampler: it read the compositor's DRM fdinfo once a second, and on that board's msm driver the read takes about half a second and stalls gnome-shell's rendering. The same binary and session with nothing reading fdinfo: 7106 frames, 35 ms late at worst, compositor at 16.8%. With the read once a second: 5056 frames, 2003 ms late, 43.6%. The sampler now times its first read and, where it is slow, reads the compositor's figures only before and after the app runs. SC8280XP-A690-Arch rows above that show the compositor near 40% predate that.

## Considered and not built

An oversized X11 child window moved with `XMoveWindow`, as a scroll done by the X server. The server moves only the pixels that were on screen: a child window has no storage for what its parent clips, so what comes into view is exposed and has to be presented again, and Vulkan cannot present a strip, only a whole image. The move and the present are not one step, so the strip would flash, and scrollbars and popups could not be drawn over the child from the parent's swapchain.
