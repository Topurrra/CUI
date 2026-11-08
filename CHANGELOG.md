# Changelog

What changed in CUI, newest first. Every component mentioned here runs live in the docs.

## Unreleased · phones and tablets

### Added

- **Terminal**: output over an input line. It reads ANSI colors (16, 256 and 24-bit), bold, dim, underline and inverse, `\r` progress bars, tabs and clear sequences, and drops the escapes it does not draw. Lines wrap at the panel width, only the rows in view are drawn, and it follows new output. It has drag-to-copy without the colors, command history, Ctrl+C to interrupt and Ctrl+L to clear. The app owns a fixed ring buffer, so writing never allocates.
- **File browser and file picker**: a folder view with sorting, natural name order, a filter, back, forward and up, a typeable path bar, places, type-ahead and keys. The open, save and folder dialog is drawn by CUI. Listing goes through a callback (the local disk by default), and big folders are read once and built only where they show.
- **Image viewer and cropper**: zoom around the pointer, pan, double-click for actual pixels, keys, eased motion and a zoom pill. The cropper has corner and edge handles, aspect lock, a round mask for avatars, thirds lines and the size while dragging.
- **Guided tour**: a spotlight that glides from control to control with a card saying what each one does. It has Back, Next and Done, progress dots and keys, and can show a centered card for steps without a target. The spotlighted control can still be tried, and the button text can be translated.
- **Month calendar**: a month grid with events. Multi-day and all-day events are bars that share lanes across a week; timed events show in time order with a dot and the time, and a full day says "+N more" and opens a list. It supports keyboard navigation across months, double-click to add, localized month and day names, any first weekday, and a phone layout with colored dots and an agenda of the selected day.
- **Radial gauge**: a value on a dial with colored bands, labelled ticks, a target marker and a needle that springs to new values, or a filled arc without the needle.
- **Candlestick chart**: open, high, low and close with volume, overlay lines and a last price tag. It zooms around the pointer, pans by dragging, follows new candles, and shows a crosshair with each candle's values. Zoomed out, candles merge per pixel column, so 100,000 of them build in about a millisecond.
- **Knob, fader and level meter**: the controls of audio, synth and mixer panels. Knobs turn by dragging up and down with a fine mode on Shift, can be bipolar or logarithmic, and reset on double-click. Faders have a labelled scale and an optional meter beside the track. Level meters have peak hold, a falling bar, a clip light and LED segments. The wheel only turns a control that has focus.

### Changed

- Truncated right-to-left text puts its ellipsis on the left, where the text ends (with text shaping).
- **Every component page fits a phone.** Layouts that used to need more than a phone's width now rearrange instead of shrinking the preview. The calendar, countdown, radar chart, activity rings and donut legend scale down. The waffle legend moves under its grid, and the banner puts its action under the message. The player drops its previous button, and the stepper keeps only the current label. Pagination collapses to the first, current and last pages, and the status bar keeps icons and drops labels. Toolbars, path bars and inspectors fold, and the dock shrinks its icons and stops magnifying.
- **Docking** shows every panel as a tab of one stack when the dockspace is too narrow for panes side by side. The saved layout is untouched, and the panes come back when there is room.
- **Segmented controls and button groups** tighten their padding before a label truncates.
- **Sankey, funnel and heatmaps** keep their labels readable on narrow charts. Sankey labels stack over their values, funnel step rates shorten, heatmaps label every other row, and the calendar heatmap drops its Less and More words.
- **Gantt charts** use a slimmer task list on phones, so the timeline keeps most of the width. Tables fade at the edge that has more columns.
- Prompt suggestions and radio cards drop to fewer columns before their text gets cramped. The command palette's key hints wrap to a second line.
- Signatures scale down with a narrower pad instead of being cut off.
- The docs site lays out each preview at the width it has before measuring it, so components that adapt are shown at full size.

### Fixed

- Joined wheel pickers in the demo sit flush, so the selection band runs across all three.
- A demo sankey read past the end of its link array.

## 2026-11-07 · 1.1: text, code and workspace

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
- The README lists every component group, including the new app chrome, code and tool panels, and leads its showcase with docking.
- Version 1.1.0, with about 160 components.

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
