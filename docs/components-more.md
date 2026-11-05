# More components

The second half of the library: richer actions and inputs, pickers, disclosure, data views, more charts, motion and AI interface pieces. Everything follows the same rules as the [core components](components.md): named options, keyboard support, light and dark, reduced motion. Each family has a page in the gallery (`./build/examples/gallery`, section *Extended*).

## Actions

### Split button

![Split button](images/components/split-button.png)

```c
static const char *more[] = { "Save as draft", "Save and publish", "Schedule..." };
switch (cui_split_button(ui, "Save", more, 3, .icon = CUI_ICON_SAVE)) {
case 0: save(); break;              /* the main part */
case 1: save_draft(); break;        /* menu item 0 */
case 2: publish(); break;
case 3: schedule(); break;
}
```

Returns 0 for the main action, 1 + i for menu item i, -1 otherwise. Options: `variant` (primary by default), `size`, `icon`, `item_icons`, `disabled`. Down arrow on the chevron opens the menu.

### Toggle group

![Toggle group](images/components/toggle-group.png)

```c
static bool format[4];          /* bold, italic, underline, code */
const char *labels[] = { "Bold", "Italic", "Underline", "Code" };
const char *icons[] = { CUI_ICON_BOLD, CUI_ICON_ITALIC, CUI_ICON_UNDERLINE, CUI_ICON_CODE };
cui_toggle_group(ui, "format", labels, 4, format, .icons = icons, .icon_only = true);
```

Each item flips its own `bool`. With `.exclusive = true` exactly one stays on, like a radio group. With `icon_only` the labels become tooltips. Arrow keys move between items.

### Confirm button

```c
if (cui_confirm_button(ui, "Delete project", .icon = CUI_ICON_TRASH_2)) delete_project();
```

The first click morphs the button into a red "Click to confirm" with a countdown line; the second click fires. It disarms after `timeout` seconds (3), on Esc, or when you click elsewhere. Options: `confirm_label`, `icon`, `variant`, `size`, `timeout`.

### Status button

![Morphing buttons](images/components/morph-buttons.png)

```c
static cui_status status;
if (cui_status_button(ui, "Save changes", status, .success_label = "Saved")) { status = CUI_STATUS_BUSY; start_save(); }
/* later: status = CUI_STATUS_SUCCESS (or CUI_STATUS_ERROR), then back to CUI_STATUS_IDLE */
```

Shows the life of the work it started: idle, a spinner while busy, a check or an error. The width springs between states and the content cross-fades. Clicks only count while idle.

### Reactions

```c
static cui_reaction re[] = { { CUI_ICON_THUMBS_UP, 12, true }, { CUI_ICON_HEART, 4, false } };
int changed = cui_reactions(ui, "reactions", re, 2);   /* index that toggled, or -1 */
```

Clicking toggles `active` and adjusts `count`; the number rolls up or down and the icon pops.

### Speed dial

```c
static const cui_speed_action acts[] = { { CUI_ICON_FILE_TEXT, "New note" }, { CUI_ICON_IMAGE, "Upload image" } };
int pick = cui_speed_dial(ui, "create", acts, 2, .tooltip = "Create");
```

A round floating action button. Clicking fans the actions out with a stagger while the plus morphs into a close icon. Esc or a click elsewhere folds them. `placement` controls the direction (above and right aligned by default).

### Theme switcher and toggle

```c
static int mode = -1;                       /* -1 picks up the current theme */
cui_theme_switcher(ui, &mode, true);        /* light, dark, system */
cui_theme_toggle(ui);                       /* one icon button: sun and moon morph */
```

Light and dark are applied for you. For "system" (mode 2), read the OS setting (for example with `cui_app_desc.follow_system_theme`) and call `cui_set_theme`.

### User menu

![User menu](images/components/user-menu.png)

```c
cui_user_menu(ui, "Ada Lovelace", .detail = "ada@analytical.engine", .status = "online") {
    cui_menu_item(ui, "Profile", .icon = CUI_ICON_USER);
    cui_menu_item(ui, "Log out", .icon = CUI_ICON_LOG_OUT);
}
```

An avatar, a name and a detail line that open an account menu with a header. Full width for sidebars; `.compact = true` shows just the avatar for headers. Options: `detail`, `status`, `avatar` (a texture), `compact`, `placement`, `width`.

### Swipe actions

```c
static const cui_swipe_action done[] = { { CUI_ICON_CHECK, "Done", CUI_TONE_SUCCESS } };
static const cui_swipe_action more[] = { { CUI_ICON_ARCHIVE, "Archive", CUI_TONE_INFO }, { CUI_ICON_TRASH_2, "Delete", CUI_TONE_DANGER } };
cui_swipe_item_begin(ui, "mail-42", done, 1, more, 2);
cui_text(ui, "Quarterly report", .width = CUI_FILL);
int action = cui_swipe_item_end(ui);   /* 0 = left action 0, 100 + i = right action i, -1 = none */
```

Drag a row sideways, or swipe with two fingers on a trackpad. Past half the reveal width it snaps open; past 60% of the row it fires the first action on that side. Click the row or anywhere else to close it.

## Inputs

### Inline edit

![Inline edit](images/components/inline-edit.png)

```c
static char title[128] = "Q3 roadmap";
if (cui_inline_edit(ui, "title", title, sizeof title, .font = CUI_FONT_DISPLAY, .size = 26) & CUI_CHANGED) rename(title);
```

Looks like text, with a pencil on hover. Click (or Enter when focused) to edit in the same type with the text selected. Enter or clicking away saves, Esc restores the old text. Options: `font`, `size`, `placeholder`, `width`.

### Scrub field

![Scrub fields](images/components/scrub.png)

```c
static float opacity = 80;
cui_scrub(ui, "Opacity", &opacity, 0, 100, .suffix = "%");
```

Drag sideways anywhere on the field to change the value; hold Shift for steps ten times finer. A click without dragging lets you type a number. Arrow keys step when focused. A hairline along the bottom shows where the value sits in its range. Options: `step` (range / 100), `format`, `suffix`, `icon` (instead of the label), `width`.

### Money field

```c
static double amount = 1249.5;
cui_money_input(ui, "amount", &amount, .label = "Amount", .code = "USD");                       /* $ 1,249.50  USD */
cui_money_input(ui, "salary", &salary, .symbol = "\xe2\x82\xac", .code = "EUR", .decimals = -1); /* whole euros */
```

Shows grouped digits at rest and plain digits while you type, keeps at most `decimals` places (2; -1 for none) and ignores anything that is not a number. Options: `label`, `symbol` ("$"), `code`, `decimals`, `width`, `hint`.

### Prefix and suffix

Every text field takes muted text before and after the value:

```c
cui_input(ui, "site", site, sizeof site, .prefix = "https://");
cui_input(ui, "weight", weight, sizeof weight, .suffix = "kg");
```

### Shortcut recorder

![Shortcut recorder](images/components/shortcut-recorder.png)

```c
static cui_key_combo palette_key = { CUI_MOD_PRIMARY, CUI_KEY_K };
if (cui_shortcut_recorder(ui, "palette-key", &palette_key)) save_keymap();
if (cui_shortcut(ui, palette_key.mods, palette_key.key)) open_palette();
```

Click and press a combination. While recording, the field shows the modifiers you hold with a pulsing dot, and app shortcuts stay quiet. Backspace clears, Esc or Tab cancels. `cui_shortcut_label(ui, combo)` gives the text ("Ctrl Shift K"), `cui_key_name(key)` one key.

## Pickers

### Multi-select

![Multi-select](images/components/multi-select.png)

```c
static bool chosen[6];
const char *langs[] = { "C", "C++", "Rust", "Zig", "Go", "Swift" };
cui_multi_select(ui, "langs", langs, 6, chosen, .label = "Languages", .searchable = true);
```

The field shows chips (with remove buttons) for up to `max_chips` items and "+N" for the rest. The list keeps itself open while you tick items and has a select all / clear action. Up, Down and Space or Enter work in the list.

### Date range picker

![Date range picker](images/components/date-range.png)

```c
static cui_date_range trip;
cui_date_range_picker(ui, "trip", &trip, .label = "Dates");
cui_range_calendar(ui, "inline", &trip, 2);   /* the calendar on its own, one or two months */
```

Click a start and an end (in either order); while you choose the end, the band follows the pointer. Presets (today, last 7 days, this month...) sit on the left unless `.no_presets = true`. `cui_date_to_days` and `cui_date_from_days` convert dates to day numbers for arithmetic.

### Time picker

![Time picker](images/components/time-picker.png)

```c
static int hour = 9, minute = 30;
cui_time_picker(ui, "start", &hour, &minute);
cui_time_picker(ui, "end", &hour2, &minute2, .twelve_hour = true, .minute_step = 15);
```

Hour and minute columns (and AM / PM) that open scrolled to the current value. `hour` is always 0 to 23.

### Wheel picker and time dial

![Wheels and dial](images/components/wheels.png)

```c
cui_wheel_picker(ui, "month", month_names, 12, &month, .width = CUI_PX(128), .loop = true);
cui_time_dial(ui, "alarm", &hour, &minute, 232);
```

Wheels are drums of options: drag with momentum, scroll or use the arrow keys, and they snap to an item. Set `loop` for values that wrap. Put several side by side with `join_left` / `join_right` to share one selection band, as in a date reel.

The time dial sets hours on two rings (1 to 12 outside, 13 to 00 inside), then switches to minutes when you let go. The hand springs the short way round. Click the hour or minute at the top to choose which one you set.

### Ruler and histogram sliders

![Ruler and histogram](images/components/ruler-histogram.png)

```c
cui_ruler_slider(ui, "Height", &height, 120, 220, .unit = "cm");
cui_histogram_slider(ui, "Price", bins, 24, &lo, &hi, 0, 240, .format = "$%.0f");
```

The ruler is a tape that slides under a fixed marker; throw it and it glides, then snaps to `step`. Options: `step`, `major_every` (labelled ticks), `spacing` (points per step), `format`, `unit`, `width`. The histogram slider is a range slider with the distribution it filters drawn above it; bars inside the range light up.

## Capture

![Dropzone, signature and avatar upload](images/components/capture.png)

### Dropzone

```c
int n = cui_dropzone(ui, "attachments", .description = "PNG, JPG or PDF, up to 25 MB", .accept = "png;jpg;pdf");
if (n > 0) for (int i = 0; i < n; i++) attach(cui_dropped_file(ui, i));
if (n < 0) { char path[1024]; if (cui_app_file_dialog(NULL, &(cui_file_dialog_opts){ 0 }, path, sizeof path)) attach(path); }
```

Files dragged from the desktop or a file manager highlight the zone ("Release to add") and land on it; files that do not match `accept` are left out and the zone says so. `.compact = true` makes a one line version. Drag and drop works with every built-in backend; the live hover highlight appears where the platform reports drags in progress (macOS and X11), elsewhere the drop itself still lands.

If you use your own window layer, feed drops with `cui_input_file_drop(ui, paths, count, x, y)` and hovering with `cui_input_file_hover(ui, true, x, y)`. Files dropped outside any dropzone are available to the app through `cui_dropped_file_count` and `cui_dropped_file` for that frame.

### Avatar upload

```c
static cui_texture photo;
if (cui_avatar_upload(ui, "photo", "Ada Lovelace", &photo, 88) < 0) browse_for_photo();
```

Drop an image on the avatar to replace it (the previous texture is destroyed). Hover shows a camera; the pencil badge marks it as editable.

### Signature pad

```c
cui_signature_pad(ui, "signature", .height = 180);
const cui_vec2 *pts; const int *ends; int strokes;
int n = cui_signature_points(ui, "signature", &pts, &ends, &strokes);   /* points relative to the pad */
```

Stroke width follows speed, so signatures look inked rather than drawn with a fixed pen. A Clear button appears once something is drawn; `cui_signature_clear` does the same from code.
