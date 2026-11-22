/* A Present notification for a private XCB queue must not start an idle CUI
 * frame. Mesa uses such a queue for swaps on DRI3. Ask the server for its
 * refresh counter so the same socket wakeup can be tested on Xvfb.
 * With the GL, Xlib-XCB and XCB Present development packages installed:
 * xvfb-run -a ctest --test-dir build -V -R '^cui_test_x11_idle$' */
#define _POSIX_C_SOURCE 200809L
#include <cui/cui_app.h>
#include <GL/glx.h>
#include <X11/Xlib-xcb.h>
#include <xcb/present.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double monotonic_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* The subscription also delivers Pixmap completions, with independent serials. */
static xcb_present_complete_notify_event_t *read_msc(xcb_connection_t *conn, xcb_special_event_t *queue,
                                                     uint32_t serial, bool wait) {
    for (;;) {
        xcb_present_complete_notify_event_t *event = (void *)(wait ? xcb_wait_for_special_event(conn, queue)
                                                                   : xcb_poll_for_special_event(conn, queue));
        if (!event) return NULL;
        if (event->event_type == XCB_PRESENT_COMPLETE_NOTIFY &&
            event->kind == XCB_PRESENT_COMPLETE_KIND_NOTIFY_MSC && event->serial == serial) return event;
        free(event);
    }
}

static int check_wait(cui_app *app, xcb_connection_t *conn, xcb_window_t win, xcb_special_event_t *queue) {
    /* Query the current refresh counter, then arrange a second notification
     * several refreshes ahead so it arrives while the app is asleep. */
    xcb_present_notify_msc(conn, win, 1, 0, 0, 0);
    xcb_flush(conn);
    xcb_present_complete_notify_event_t *first = read_msc(conn, queue, 1, true);
    if (!first) {
        fprintf(stderr, "failed to read the initial Present notification\n");
        return 1;
    }
    /* Another Present completion can precede ours on the same queue. */
    xcb_present_notify_msc(conn, win, 3, first->msc + 2, 0, 0);
    xcb_present_notify_msc(conn, win, 2, first->msc + 4, 0, 0);
    free(first);
    xcb_flush(conn);

    const float wait = 0.5f;
    cui_request_wakeup(cui_app_ui(app), wait);
    double start = monotonic_seconds();
    bool running = cui_app_frame(app);
    double elapsed = monotonic_seconds() - start;
    xcb_present_complete_notify_event_t *second = read_msc(conn, queue, 2, false);
    bool delivered = second != NULL;
    free(second);
    printf("private Present notification: %s, next frame after %.1f ms (timer %.0f ms)\n",
           delivered ? "received" : "missing", elapsed * 1000, wait * 1000);
    return running && delivered && elapsed >= wait - 0.001 ? 0 : 1;
}

int main(void) {
    cui_app *app = cui_app_create(&(cui_app_desc){
        .title = "X11 idle wait", .width = 128, .height = 96,
        .hidden = true, .renderer = CUI_RENDERER_OPENGL
    });
    Display *dpy = glXGetCurrentDisplay();
    if (!app || cui_app_is_software(app) || !dpy) {
        fprintf(stderr, "skip: an X11 display with GLX is required\n");
        if (app) cui_app_destroy(app);
        return 77;
    }
    xcb_connection_t *conn = XGetXCBConnection(dpy);
    const xcb_query_extension_reply_t *present = xcb_get_extension_data(conn, &xcb_present_id);
    if (!present || !present->present) {
        fprintf(stderr, "skip: X11 Present is unavailable\n");
        cui_app_destroy(app);
        return 77;
    }
    /* Finish startup and the first GL swap before measuring an empty frame. */
    for (int i = 0; i < 3; i++) {
        cui_request_redraw(cui_app_ui(app));
        if (!cui_app_frame(app)) { cui_app_destroy(app); return 1; }
    }
    XSync(dpy, True);
    xcb_present_event_t id = xcb_generate_id(conn);
    xcb_special_event_t *queue = xcb_register_for_special_xge(conn, &xcb_present_id, id, NULL);
    int result = 1;
    if (queue) {
        xcb_window_t win = (xcb_window_t)(uintptr_t)cui_app_native_window(app);
        xcb_generic_error_t *error = xcb_request_check(conn,
            xcb_present_select_input_checked(conn, id, win, XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY));
        if (!error) result = check_wait(app, conn, win, queue);
        else fprintf(stderr, "could not select Present events (X11 error %u)\n", error->error_code);
        free(error);
        xcb_unregister_for_special_event(conn, queue);
    }
    cui_app_destroy(app);
    return result;
}
