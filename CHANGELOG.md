# Changelog

What changed in CUI, newest first. Every component mentioned here runs live in the docs.

## 2026-11-07 · Text, code and workspace

### Added

- **Docking**: drag panels by their tabs to stack them or split any side, with drop zones and a preview while dragging, resizable dividers, closing and reopening in place, and a layout that saves and loads as a short readable string. It builds on the existing document tabs and split view.
- **Tree table and path bar**: an expandable tree with columns on top of the data grid, for file explorers, scene graphs and debugger variables. It handles large trees, loads folders lazily, and follows the usual tree keys. The path bar collapses long paths, offers sibling menus and switches to a text field for typing a path.
- **Timeline editor**: grouped tracks with keyframes, a time ruler with in and out markers, a scrubbable playhead, clips, box selection, snapping to frames and zoom around the pointer. Its keys are curve editor keys, so the selected track's curve can be edited right beside it.
- **Markdown view**: headings, emphasis, links, nested and task lists, quotes, tables and fenced code with the existing highlighter. It renders partial text sensibly while an answer streams in, reports link clicks and task toggles instead of acting on them, and AI chat messages can render their body as markdown.
- **Code editor**: an editable code block with syntax highlighting, a line number gutter, selection, undo and redo, auto-indent, bracket matching, find (Ctrl+F) and go to line (Ctrl+G). Only the lines in view are laid out, so long files stay fast.
- **Diff viewer**: split and inline modes, word-level marks inside changed lines, folded unchanged stretches, next and previous change, and syntax colors. The diff runs only when a text changes, and `cui_diff_compute` gives the same diff without any UI.

### Changed

- **Component pages** in the docs are now organized into Usage, Behavior, Keyboard, Options and Related, with an "On this page" index. The options table is generated from `cui.h`, keys are listed on their own, related components are linked, screenshots sit behind a toggle, source links open the exact lines on GitHub, and every live preview can switch between full, tablet and phone widths.
- **The components index** has a filter box, category chips, "New" labels and a compact list view.
- A focused editor can keep the Tab key for indenting; Esc gives Tab back to focus movement.
- **Data grid**: columns can declare a priority and hide on narrow widths, rows can select without a checkbox column, a cursor set from code scrolls into view, and truncated cells show their full text on hover.
- **Changelog**: this page, also on the docs site.

### Fixed

- The data grid's sticky header no longer disappears once the rows scroll past it.
- A scroll area squeezed to zero width no longer produces invalid values in its edge fade.

## 2026-11-06 · Tools and a new landing page

### Added

- **App chrome**: a menu bar with full keyboard access, a toolbar that collapses into a "more" menu, a status bar, and editor-style document tabs that close, reorder and scroll.
- **Tool panels**: a property inspector with typed editors in folding groups, and a log console over a caller-owned ring buffer with level filters, search and follow.
- **Node editor**: a pannable, zoomable canvas of nodes, typed pins and curved links, with box selection, a minimap and ordinary components inside nodes.
- **Flame graph**: a profiler timeline that stays fast with 100,000 spans, with zoom around the pointer, hover timings and zoom to a span.
- **Hex viewer**: synced offset, hex and ASCII columns, virtualized for large files, with highlighted ranges, copy and optional editing.
- **Curve and gradient editors**: keyframes with bezier handles and a color ramp with stops. `cui_curve_eval` and `cui_gradient_eval` sample exactly what the editors draw.
- **Arabic-script shaping and bidirectional text**, optional, contributed in pull request #1 by omid3098.

### Changed

- **A new landing page** for the live docs: a lighter hero ending in the C mark, a tabbed live showcase, a bento of live components, a theme playground, guide cards and motion throughout, all off with reduced motion.
- The phone navigation fades the edge it scrolls past instead of cutting a word in half.
- A truncated label no longer shows its tooltip through a menu, popover or dialog open above it.
- The smallest complete app now measures about 250 KB stripped.

## 2026-11-05 · A calmer design and the live docs

### Changed

- **A more timeless look**: one concentric radius scale, quieter edges, flat solid controls, and shared motion, state, radius, edge and icon tokens.
- **Responsive layout**: rows wrap, grids pick their column count, and components adapt to narrow widths.
- Keyboard focus behaves after a click, modifier keys release correctly, and avatar stacks are tidier.

### Added

- **Live docs** on GitHub Pages: every component running in the browser, compiled to WebAssembly, with its C source and docs beside it. The site has a raster C mark and favicon.
- A shorter, code-first README.
- A contributing guide with disclosure and evidence rules for people and AI agents, and issue and pull request forms.

## 2026-11-05 · 1.0

The first public version.

### Added

- **The core**: layout, draw list, text with kerning and font fallback, themes with light and dark and eight accents, an OpenGL renderer and a software renderer.
- **Backends** for Win32, Cocoa, X11, SDL2 and GLFW, with native file dialogs, URL opening and OS file drag and drop.
- **Components** for actions, inputs, pickers, selection, navigation, overlays, feedback, data display, a virtualized data grid, a command palette, kanban, week calendar and gantt charts, a dozen kinds of chart, conversation and media components, text effects and AI interface components.
- **Example apps**: gallery, dashboard, settings, notes, mail, tracker, music, finance and an AI assistant, each one C file.
- **Tests and CI** on Linux, Windows and macOS: unit, interaction and render tests, a randomized input fuzzer, and AddressSanitizer and UndefinedBehaviorSanitizer builds.
- **Documentation** with screenshots rendered by the tests, so visual changes and their docs move together.

### Changed

- Contrast meets WCAG AA, wrapped text avoids lone last words, and tabular figures line up.
- The library stays small: compressed fonts and dead code stripping, so apps carry only what they use.
