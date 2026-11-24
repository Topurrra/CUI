#include <cui/cui.h>
#include <cui/cui_render.h>
#include <stdio.h>
#include <string.h>

static int checks, failed;
#define CHECK(condition) do { checks++; if (!(condition)) { failed++; \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); } } while (0)

typedef struct {
    cui_input_event events[8];
    int count;
    bool consume;
} observer;

static bool observe(cui_ctx *ui, const cui_input_event *event, void *user) {
    (void)ui;
    observer *o = user;
    if (o->count < 8) o->events[o->count++] = *event;
    return o->consume;
}

static void test_pointer_observer(cui_ctx *ui) {
    observer o = {0};
    cui_set_input_hook(ui, observe, &o);
    cui_input_mouse_move(ui, 30, 40);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
    cui_input_mouse_move(ui, 55, 65);
    cui_input_scroll(ui, -1, 2);
    CHECK(cui_mouse_down(ui, CUI_MOUSE_LEFT));
    CHECK(cui_mouse_pos(ui).x == 55 && cui_mouse_pos(ui).y == 65);
    cui_input_focus(ui, false);
    CHECK(!cui_window_focused(ui) && !cui_mouse_down(ui, CUI_MOUSE_LEFT));
    cui_input_mouse_leave(ui);
    CHECK(o.count == 6);
    CHECK(o.events[0].kind == CUI_INPUT_MOUSE_MOVE && o.events[0].position.x == 30);
    CHECK(o.events[1].kind == CUI_INPUT_MOUSE_BUTTON &&
          o.events[1].down && o.events[1].button == CUI_MOUSE_LEFT);
    CHECK(o.events[2].kind == CUI_INPUT_MOUSE_MOVE && o.events[2].position.y == 65);
    CHECK(o.events[3].kind == CUI_INPUT_SCROLL &&
          o.events[3].delta.x == -1 && o.events[3].delta.y == 2);
    CHECK(o.events[4].kind == CUI_INPUT_FOCUS && !o.events[4].focused);
    CHECK(o.events[5].kind == CUI_INPUT_MOUSE_LEAVE);
    o.consume = true;
    cui_input_focus(ui, true);
    CHECK(o.count == 7 && !cui_window_focused(ui));
    cui_set_input_hook(ui, NULL, NULL);
    cui_input_focus(ui, true);
    CHECK(o.count == 7 && cui_window_focused(ui));
}

static bool take_key(cui_ctx *ui, void *user, const cui_input_event *event) {
    int *calls = user;
    CHECK(event->kind == CUI_INPUT_KEY && event->key == CUI_KEY_TAB);
    CHECK(cui_key_down(ui, CUI_KEY_TAB) == event->down);
    CHECK(cui_input_repeated(ui) == (*calls == 1));
    (*calls)++;
    return true;
}

static void test_canvas_dispatch(cui_ctx *ui) {
    cui_begin_frame(ui, 0);
    const cui_canvas_opts opts = { .focusable = true, .component_input = true };
    cui_canvas_ex(ui, "canvas", CUI_PX(100), CUI_PX(80), NULL, NULL, 0, &opts);
    cui_id canvas = cui_last_id(ui);
    cui_request_component_focus(ui, canvas);
    cui_end_frame(ui);
    CHECK(cui_component_has_input(ui, canvas));

    cui_begin_frame(ui, 0);
    int calls = 0;
    cui_input_event tab = { .kind = CUI_INPUT_KEY, .key = CUI_KEY_TAB, .down = true };
    cui_dispatch_input(ui, &tab, take_key, &calls);
    cui_dispatch_input(ui, &tab, take_key, &calls);
    CHECK(cui_focused_id(ui) == canvas && cui_key_down(ui, CUI_KEY_TAB));
    CHECK(!cui_key_pressed(ui, CUI_KEY_TAB) && !cui_key_action_down(ui, CUI_KEY_TAB));
    tab.down = false;
    cui_dispatch_input(ui, &tab, take_key, &calls);
    CHECK(calls == 3 && !cui_key_down(ui, CUI_KEY_TAB));
    cui_end_frame(ui);
    CHECK(!cui_component_has_input(ui, canvas));
}

typedef struct {
    cui_id canvas;
    int key_calls, text_calls;
} canvas_route;

static bool route_canvas(cui_ctx *ui, void *user, const cui_input_event *event) {
    canvas_route *route = user;
    if (!cui_component_has_input(ui, route->canvas)) return false;
    if (event->kind == CUI_INPUT_KEY && event->key == CUI_KEY_ENTER) {
        CHECK(!cui_input_repeated(ui));
        route->key_calls++;
        return true;
    }
    if (event->kind == CUI_INPUT_TEXT) {
        CHECK(event->text[0] == 'a' && event->text[1] == 0);
        route->text_calls++;
        return true;
    }
    return false;
}

static void test_tab_then_input(cui_ctx *ui) {
    const cui_canvas_opts opts = { .focusable = true, .component_input = true };
    canvas_route route = {0};
    cui_begin_frame(ui, 0);
    cui_button(ui, "Before canvas");
    cui_id first = cui_last_id(ui);
    cui_canvas_ex(ui, "canvas", CUI_PX(100), CUI_PX(80), NULL, NULL, 0, &opts);
    route.canvas = cui_last_id(ui);
    cui_end_frame(ui);

    cui_begin_frame(ui, 0);
    cui_input_event tab = { .kind = CUI_INPUT_KEY, .key = CUI_KEY_TAB, .down = true };
    cui_dispatch_input(ui, &tab, route_canvas, &route);
    CHECK(cui_focused_id(ui) == first && !cui_component_has_input(ui, route.canvas));
    tab.down = false;
    cui_dispatch_input(ui, &tab, route_canvas, &route);
    tab.down = true;
    cui_dispatch_input(ui, &tab, route_canvas, &route);
    CHECK(cui_focused_id(ui) == route.canvas && cui_focus_visible(ui, route.canvas));
    CHECK(cui_component_has_input(ui, route.canvas));

    const cui_input_event enter = { .kind = CUI_INPUT_KEY, .key = CUI_KEY_ENTER, .down = true };
    const cui_input_event text = { .kind = CUI_INPUT_TEXT, .text = "a" };
    cui_dispatch_input(ui, &enter, route_canvas, &route);
    cui_dispatch_input(ui, &text, route_canvas, &route);
    CHECK(route.key_calls == 1 && route.text_calls == 1);
    CHECK(cui_key_down(ui, CUI_KEY_ENTER) && !cui_key_action_down(ui, CUI_KEY_ENTER));
    CHECK(!cui_key_pressed(ui, CUI_KEY_ENTER));
    cui_push_disabled(ui, true);
    cui_canvas_ex(ui, "canvas", CUI_PX(100), CUI_PX(80), NULL, NULL, 0, &opts);
    cui_request_component_focus(ui, route.canvas);
    cui_pop_disabled(ui);
    cui_end_frame(ui);
    CHECK(!cui_component_has_input(ui, route.canvas) && cui_focused_id(ui) == 0);
    cui_input_focus(ui, false);
    cui_input_focus(ui, true);
}

static void test_ctrl_tab_release(cui_ctx *ui) {
    cui_begin_frame(ui, 0);
    cui_button(ui, "First");
    cui_id first = cui_last_id(ui);
    cui_button(ui, "Second");
    cui_id second = cui_last_id(ui);
    cui_end_frame(ui);

    cui_begin_frame(ui, 0);
    cui_input_event tab = { .kind = CUI_INPUT_KEY, .key = CUI_KEY_TAB, .down = true };
    cui_dispatch_input(ui, &tab, NULL, NULL);
    CHECK(cui_focused_id(ui) == first);
    tab.down = false;
    cui_dispatch_input(ui, &tab, NULL, NULL);

    tab.mods = CUI_MOD_CTRL;
    tab.down = true;
    cui_dispatch_input(ui, &tab, NULL, NULL);
    tab.down = false;
    cui_dispatch_input(ui, &tab, NULL, NULL);
    CHECK(cui_focused_id(ui) == first && cui_key_pressed(ui, CUI_KEY_TAB));

    const cui_input_event ctrl_release = { .kind = CUI_INPUT_KEY, .key = CUI_KEY_NONE };
    cui_dispatch_input(ui, &ctrl_release, NULL, NULL);
    CHECK(cui_focused_id(ui) == first && cui_key_pressed(ui, CUI_KEY_TAB));

    const cui_input_event key = { .kind = CUI_INPUT_KEY, .key = CUI_KEY_A, .down = true };
    cui_dispatch_input(ui, &key, NULL, NULL);
    CHECK(cui_focused_id(ui) == first && cui_key_pressed(ui, CUI_KEY_A));

    tab.mods = 0;
    tab.down = true;
    cui_dispatch_input(ui, &tab, NULL, NULL);
    CHECK(cui_focused_id(ui) == second);
    cui_end_frame(ui);
    cui_input_focus(ui, false);
    cui_input_focus(ui, true);
}

static cui_id submit_modal_canvas(cui_ctx *ui, bool show_dialog, bool open_cover) {
    cui_button(ui, "Page anchor");
    if (cui_popover_begin(ui, "Page popover", NULL)) {
        cui_button(ui, "Page popover button");
        cui_popover_end(ui);
    }
    const cui_dialog_opts dialog = { .persistent = true, .no_close_button = true };
    if (!show_dialog || !cui_dialog_begin(ui, "Dialog", &show_dialog, &dialog)) return 0;
    const cui_canvas_opts canvas = { .focusable = true, .component_input = true };
    cui_canvas_ex(ui, "Canvas", CUI_PX(100), CUI_PX(40), NULL, NULL, 0, &canvas);
    cui_id id = cui_last_id(ui);
    if (open_cover) cui_popover_open(ui, "Dialog popover");
    if (cui_popover_begin(ui, "Dialog popover", NULL)) {
        cui_button(ui, "Dialog popover button");
        cui_popover_end(ui);
    }
    cui_dialog_end(ui);
    return id;
}

static void test_modal_canvas_with_popovers(cui_ctx *ui) {
    cui_begin_frame(ui, 1.0f / 60);
    cui_popover_open(ui, "Page popover");
    submit_modal_canvas(ui, false, false);
    cui_end_frame(ui);

    cui_begin_frame(ui, 1.0f / 60);
    canvas_route route = { .canvas = submit_modal_canvas(ui, true, false) };
    cui_request_component_focus(ui, route.canvas);
    cui_end_frame(ui);
    CHECK(cui_popover_is_open(ui, "Page popover"));
    CHECK(route.canvas && cui_focused_id(ui) == route.canvas);
    CHECK(cui_component_has_input(ui, route.canvas));

    cui_begin_frame(ui, 1.0f / 60);
    CHECK(cui_component_has_input(ui, route.canvas));
    const cui_input_event text = { .kind = CUI_INPUT_TEXT, .text = "a" };
    cui_dispatch_input(ui, &text, route_canvas, &route);
    CHECK(route.text_calls == 1);
    submit_modal_canvas(ui, true, true);
    cui_end_frame(ui);
    CHECK(cui_focused_id(ui) == route.canvas && !cui_component_has_input(ui, route.canvas));

    cui_begin_frame(ui, 1.0f / 60);
    CHECK(!cui_component_has_input(ui, route.canvas));
    cui_dispatch_input(ui, &text, route_canvas, &route);
    CHECK(route.text_calls == 1);
    cui_end_frame(ui);
}

static void platform_cursor(void *user, cui_cursor cursor) {
    *(cui_cursor *)user = cursor;
}

static void paint_resize_cursor(cui_ctx *ui, cui_rect rect, void *user) {
    (void)rect;
    (void)user;
    cui_set_cursor(ui, CUI_CURSOR_RESIZE_EW);
}

static void test_cursor(cui_ctx *ui) {
    cui_cursor current = CUI_CURSOR_ARROW;
    cui_set_platform(ui, &(cui_platform_iface){ .user = &current, .set_cursor = platform_cursor });
    cui_begin_frame(ui, 0);
    cui_set_cursor(ui, CUI_CURSOR_HAND);
    CHECK(cui_get_cursor(ui) == CUI_CURSOR_HAND);
    cui_canvas(ui, "grip", CUI_PX(40), CUI_PX(40), paint_resize_cursor, NULL, 0);
    cui_end_frame(ui);
    CHECK(current == CUI_CURSOR_RESIZE_EW && cui_get_cursor(ui) == current);
    cui_begin_frame(ui, 0);
    cui_end_frame(ui);
    CHECK(current == CUI_CURSOR_ARROW);
    cui_set_platform(ui, NULL);
}

/* cui_focus_select_all on a field built in the same frame: it gets the keyboard with all its text selected, so typing
 * replaces it (a rename field that opens with the old name) */
static void test_focus_select(cui_ctx *ui) {
    char buf[32] = "old name";
    cui_begin_frame(ui, 1.0f / 60);
    cui_input(ui, "name", buf, sizeof buf);
    cui_end_frame(ui);
    CHECK(cui_focused_id(ui) != cui_make_id(ui, "name"));
    cui_begin_frame(ui, 1.0f / 60);
    cui_focus_select_all(ui, cui_make_id(ui, "name"));
    cui_input(ui, "name", buf, sizeof buf);
    cui_end_frame(ui);
    CHECK(cui_focused_id(ui) == cui_make_id(ui, "name"));
    cui_input_text(ui, "new");
    cui_begin_frame(ui, 1.0f / 60);
    int f = cui_input(ui, "name", buf, sizeof buf);
    cui_end_frame(ui);
    CHECK((f & CUI_CHANGED) && !strcmp(buf, "new"));
    cui_input_key(ui, CUI_KEY_ENTER, true, 0);
    cui_begin_frame(ui, 1.0f / 60);
    f = cui_input(ui, "name", buf, sizeof buf);
    cui_end_frame(ui);
    cui_input_key(ui, CUI_KEY_ENTER, false, 0);
    CHECK((f & CUI_SUBMITTED) && !strcmp(buf, "new"));
}

int main(void) {
    cui_sw_renderer *sw = cui_sw_create();
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(sw) });
    cui_set_viewport(ui, 200, 160, 1);
    test_pointer_observer(ui);
    test_canvas_dispatch(ui);
    test_tab_then_input(ui);
    test_cursor(ui);
    test_ctrl_tab_release(ui);
    test_modal_canvas_with_popovers(ui);
    test_focus_select(ui);
    cui_destroy(ui);
    cui_sw_destroy(sw);
    printf("%d component input checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
