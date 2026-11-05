// Compiles the public headers as C++ and uses the *_ex functions with
// designated initializers (C++20).
#include <cui/cui.h>
#include <cui/cui_app.h>
#include <cui/cui_render.h>

void cpp_ui(cui_ctx *ui) {
    cui_box_begin(ui, nullptr);
    cui_button_opts o{};
    o.variant = CUI_BUTTON_PRIMARY;
    o.icon = CUI_ICON_PLUS;
    if (cui_button_ex(ui, "Add", &o)) {}
    cui_text_opts t{};
    t.color = CUI_COLOR_TEXT_SECONDARY;
    cui_text_ex(ui, "From C++", &t);
    cui_color c = CUI_RGB(0x7747ff);
    cui_size s = CUI_PX(10);
    (void)c; (void)s;
    cui_box_end(ui);
}

int main() {
    cui_sw_renderer *sw = cui_sw_create();
    cui_ctx_desc d{};
    d.renderer = cui_sw_iface(sw);
    cui_ctx *ui = cui_create(&d);
    cui_begin_frame(ui, 1.0f / 60);
    cpp_ui(ui);
    cui_end_frame(ui);
    cui_destroy(ui);
    cui_sw_destroy(sw);
    return 0;
}
