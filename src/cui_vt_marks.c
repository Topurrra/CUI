/* The VT screen's command marks (OSC 133 and 633), progress (OSC 9;4) and copy requests (OSC 52), opt in with
 * CUI_VT_FEATURE_MARKS. Stubs for now: everything returns "nothing". */
#include "cui_internal.h"

#include <limits.h>
#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <time.h>
#endif

double cui_vt_now_ms(void) {
#if defined(_WIN32)
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
#endif
}

int cui_vt_mark_count(const cui_vt *v) { (void)v; return 0; }
bool cui_vt_mark_get(const cui_vt *v, int i, cui_vt_mark *out) { (void)v; (void)i; (void)out; return false; }
int cui_vt_mark_at_row(const cui_vt *v, int row) { (void)v; (void)row; return -1; }
size_t cui_vt_mark_command(const cui_vt *v, int i, char *out, size_t cap) { (void)v; (void)i; if (out && cap) out[0] = 0; return 0; }
size_t cui_vt_mark_output(const cui_vt *v, int i, char *out, size_t cap) { (void)v; (void)i; if (out && cap) out[0] = 0; return 0; }
unsigned cui_vt_mark_seq(const cui_vt *v) { (void)v; return 0; }
void cui_vt_mark_select(cui_vt *v, int i, int what) { (void)v; (void)i; (void)what; }
int cui_vt_mark_selected(const cui_vt *v) { (void)v; return -1; }
int cui_vt_mark_near(const cui_vt *v, int row, int dir) { (void)v; (void)row; (void)dir; return -1; }
int cui_vt_progress(const cui_vt *v, int *percent) { (void)v; if (percent) *percent = 0; return CUI_VT_PROGRESS_NONE; }
size_t cui_vt_take_clipboard(cui_vt *v, char *out, size_t cap) { (void)v; if (out && cap) out[0] = 0; return 0; }
