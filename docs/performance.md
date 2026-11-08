# Performance and memory

CUI is meant to stay small and quiet in long running apps. An idle window should cost nothing, a busy one should spend its time on what changed, and memory should stay flat over hours of use. This page describes how that is checked and what the numbers are.

## How it is measured

`tests/soak.c` drives an app the way a person would. It includes the app's source with `cui_app_run` intercepted, so the app runs unmodified with its own data. The simulated person:

- moves the pointer to a control the app drew in the last frame and clicks it, sometimes twice;
- types into whatever field has focus, including accents, emoji and Arabic, and uses Backspace, select all and Enter;
- scrolls what scrolls and drags sliders, splitters and cards;
- presses Escape, Tab, arrows, Page Down and Home;
- resizes the window between phone, tablet and desktop sizes and switches the theme;
- leaves the app alone for a few seconds now and then.

It runs in two ways:

- **Headless** (`cui_soak_<app> --minutes 60`): software rendering, a counting allocator on the context, and frames advanced 1/60 s each as fast as they run, so an hour of use takes a few minutes. It reports library memory over time, allocations per frame, frame build times, the slowest frames and what came before them, and memory left after `cui_destroy` (which fails the run). Every 1,500 frames it also checks the incremental software frame against a full repaint (a difference fails the run).
- **Windowed** (`--window`, add `--sw` for the software renderer): a real X11 window, OpenGL or software, driven by real input events through XTest from another thread. It reports frames per second, CPU time, resident memory and heap. `--idle` leaves the window alone to show what waiting costs.

Other options: `--storm` (a new window size and display scale every 10 frames), `--csv` (a sample every 10 s), and `--sites` / `--deep` (where live memory and steady-state allocations come from; build with `-fno-omit-frame-pointer`).

A 30 second headless run of the Notes example is part of `ctest`.

## Results

All numbers come from Linux on a shared cloud machine (x86-64, `-O2`). Times vary with the machine; memory and allocation counts do not.

### An hour of use, headless

Each app ran 60 simulated minutes with the software renderer at 1280×800 and the window changing size along the way. "Library memory" is everything CUI allocated through the context allocator: state, buffers, fonts and the glyph atlas, not the app's own data or textures.

| App | Frames built | Library memory after the first minute | Peak | Allocations per frame | Incremental checks | Bytes left after `cui_destroy` |
|---|---|---|---|---|---|---|
| Notes | 62,649 | 1.59 to 1.97 MB | 1.97 MB | 0.036 | 41, 0 differ | 0 |
| Mail | 61,408 | 1.66 to 5.03 MB | 6.05 MB | 0.025 | 40, 0 differ | 0 |
| Settings | 54,129 | 1.66 to 4.86 MB | 5.87 MB | 0.026 | 36, 0 differ | 0 |
| Tracker | 52,841 | 2.32 to 2.34 MB | 2.34 MB | 0.012 | 35, 0 differ | 0 |
| Music | 172,497 | 1.90 to 2.34 MB | 2.34 MB | 0.007 | 114, 0 differ | 0 |
| Finance | 68,928 | 2.18 to 5.32 MB | 6.33 MB | 0.009 | 45, 0 differ | 0 |
| Dashboard | 62,239 | 2.21 to 5.26 MB | 6.27 MB | 0.010 | 41, 0 differ | 0 |
| Assistant | 82,313 | 2.18 to 2.26 MB | 2.26 MB | 0.018 | 54, 0 differ | 0 |
| Gallery (every component page) | 98,827 | 1.94 to 8.04 MB | 9.12 MB | 0.122 | 65, 0 differ | 0 |
| Studio: IDE with file browser, editor and terminal | 56,476 | 1.96 to 5.14 MB | 6.15 MB | 0.021 | 37, 0 differ | 0 |
| Chat: 20,000 messages per channel, emoji | 65,913 | 1.92 to 2.43 MB | 2.63 MB | 0.127 | 43, 0 differ | 0 |
| Pulse: live market screen, always animating | 216,000 | 2.24 to 2.43 MB | 2.43 MB | 0.017 | 144, 0 differ | 0 |
| Mixer: meters and knobs, always animating | 127,398 | 1.77 to 2.72 MB | 2.72 MB | 0.006 | 84, 0 differ | 0 |

The ranges are what memory moves between, not growth: the top end comes with large windows, zoom and a full glyph atlas, and memory falls back when the window gets smaller and the atlas compacts. Tracker, Assistant and Pulse moved by less than 200 KB over the hour. Every run was repeated with a different seed and gave the same picture.

### A real window

The same person driving a real X11 window through XTest for 8 minutes, at 1280×800. CPU is the share of one core over the whole run, including the idle spells.

| App | Software renderer: CPU | Software: resident memory | OpenGL on llvmpipe: CPU | OpenGL: resident memory |
|---|---|---|---|---|
| Notes | 2.8% | 10.8 MB | 26.6% | 104 MB |
| Mail | 4.2% | 11.8 MB | 29.0% | 105 MB |
| Studio | 2.4% | 13.1 MB | 34.5% | 107 MB |

Resident memory stayed within 0.5 MB of its value at the first minute in every run. The machine has no GPU, so OpenGL is drawn on the CPU by Mesa's llvmpipe, which costs about ten times as much as CUI's own renderer and loads about 90 MB of driver. The default renderer therefore picks software when the OpenGL driver reports a software rasterizer (llvmpipe, softpipe, SwiftShader, Microsoft Basic Render and others); `CUI_RENDERER=opengl` overrides it. With a real GPU OpenGL is the better choice and stays the default.

A window left alone for 90 seconds costs 0.1 to 0.3% of a core: it draws only when the caret blinks or a timer is due.

### Speed

- In the real window runs a frame's build (the app's code plus layout and drawing into the draw list) took 0.02 to 0.04 ms on average and about 0.1 ms at the 99th percentile. The headless runs, which shared the machine with a dozen others, measured 0.06 to 1.0 ms on average.
- A full software repaint of the Mail example at 1280×800 takes 219 million instructions, down from 547 million; Studio 299 million, down from 709 million. After the first frame the software renderer redrew 4 to 15% of the pixels per frame in most apps; the always-animating Mixer and Pulse redrew 22% and 42%.
- `cui_create` takes about 2 ms and the first frame about 2.4 ms.
- The terminal parses 328 MB of output per second and keeps 0.2 ms frames while 2 MB arrives every frame.
- The emoji picker builds a frame in about 2 ms.

### Checks

- AddressSanitizer and UndefinedBehaviorSanitizer runs of Chat and Pulse, and Valgrind runs of Notes and Mixer, report no errors.
- The Gallery under `--storm` (window size and display scale changing every 10 frames) peaked at 22 MB and stayed stable.
- In the browser, ten rounds through all 45 component pages of the site (450 live previews created and destroyed, with the pointer moving over each) left one WebAssembly instance, no stray canvases, a JS heap of 3.4 MB, and no errors.

### What the soak found and fixed

- The Gallery's image compare page created two textures every time it was shown and never freed them. They now live in state and are freed with it.
- Paging through the data grid left an entry in the state table for every cell ever shown (15,000 after an hour). Entries that hold only a rectangle now go after 30 frames, hover fades take no state until hovered, and the table packs itself, so it stays near 1,700.
- Buttons and links took state on first draw. They now take it only when touched, which cut Chat's allocations per frame from 0.27 to 0.09.
- The glyph atlas and per-frame buffers only ever grew. Both now shrink back after a burst (a large window, a zoom, a long page).
- OpenGL through llvmpipe used ten times the CPU of the software renderer, so the default now picks software in that case.

## What keeps it low

- **Nothing happens without a reason.** With no input and nothing animating, `cui_needs_redraw` is false and the loop sleeps. Components that wait (the caret, tooltips, toasts) set `cui_next_wakeup`, and `cui_request_wakeup` does the same for an app's own timers.
- **Only what changed is drawn** by the software renderer: 64 pixel tiles are hashed from the triangles over them, and only tiles whose hash changed, or whose triangles sample a part of a texture that was written, are drawn again. A frame with no change draws nothing.
- **State lives as long as it is useful.** Every node with an id keeps last frame's rectangle; such entries go after 30 frames unused, state with data after about five seconds. Hover fades and buttons take no state until they are touched, and the table packs itself after a burst.
- **Buffers come back down.** The glyph atlas moves the glyphs drawn lately into a smaller atlas when they fit; the emoji texture goes when no emoji has been drawn for 600 frames; and the per-frame buffers (vertices, indices, commands, nodes, the frame arena) shrink when they have been four times bigger than needed for 600 frames.
- **Steady frames allocate almost nothing.** After warm-up the apps above make a few hundredths of an allocation per frame, mostly new state for things that came into view.
