/* Headless direction, mirror, and pointer regression; optional FONT OUT_PREFIX. */
#include "../src/cui_internal.h"
#include <cui/cui_render.h>

#define W 480
#define H 700
#define CHECK(x, msg) do { if (!(x)) { fprintf(stderr, "%s at %d\n", msg, __LINE__); exit(1); } } while (0)
static int clicked[6];
static cui_id button_ids[6];
static bool dark;
#ifdef CUI_TEXT_SHAPING
static const bool shaping = true;
#else
static const bool shaping = false;
#endif
static bool enabled = true, checked = true;
static int segment;
static char input[64];
static const char *search_placeholder = "جست‌وجو در کارها";
static const char *titles[] = {"سلام دنیا", "Review changes", "امروز ۳ تا Timer دارم", "Timer: سلام ۱۲۳ دنیا."};
static const char *lines[] = {"سه تا کار مونده", "Ready for review", "زمان رو بررسی کن", "Ready at 12:30"};

static void screen(cui_ctx *ui, bool automatic) {
    cui_column(ui, .id = "panel", .width = CUI_FILL, .height = CUI_FILL, .pad = 24, .gap = 16) {
        cui_row(ui, .id = "header", .width = CUI_FILL, .height = CUI_PX(40)) {
            cui_text(ui, automatic ? "Today's work" : "کارهای امروز", .size = 24, .nowrap = true);
            cui_spacer(ui);
            if (cui_icon_button(ui, CUI_ICON_CHEVRON_RIGHT, .tooltip = "Header")) clicked[0]++;
            button_ids[0] = cui_last_id(ui);
        }
        cui_column(ui, .id = "list", .width = CUI_FILL, .gap = 8) {
            for (int k = 0; k < 4; k++) {
                cui_push_id_int(ui, k);
                cui_row(ui, .id = "row", .width = CUI_FILL, .height = CUI_PX(64), .gap = 12,
                        .direction = automatic ? CUI_DIRECTION_AUTO : CUI_DIRECTION_INHERIT) {
                    cui_box(ui, .width = CUI_PX(24), .height = CUI_PX(24), .radius = 12,
                            .bg = CUI_C(ui, CONTROL_ON), .dir = CUI_STACK, .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
                        cui_icon(ui, CUI_ICON_CHECK, 14, CUI_C(ui, CONTROL_GLYPH));
                    }
                    cui_column(ui, .id = "copy", .width = CUI_FILL, .gap = 0) {
                        cui_text(ui, titles[k], .size = 16, .nowrap = true, .line_height = 1.5f);
                        cui_text(ui, lines[k], .size = 12, .nowrap = true, .line_height = 1.5f, .color = CUI_COLOR_TEXT_SECONDARY);
                    }
                    if (cui_icon_button(ui, CUI_ICON_CHEVRON_RIGHT, .tooltip = "Open")) clicked[k+1]++;
                    button_ids[k+1] = cui_last_id(ui);
                }
                cui_pop_id(ui);
            }
        }
        const char *fa[] = {"امروز", "هفته", "همه"};
        const char *en[] = {"Today", "Week", "All"};
        cui_segmented(ui, "period", automatic ? en : fa, 3, &segment, .full_width = true);
        cui_input(ui, "search", input, sizeof input, .placeholder = search_placeholder, .icon = CUI_ICON_SEARCH);
        cui_switch(ui, automatic ? "Notifications" : "اعلان‌ها", &enabled);
        cui_checkbox(ui, automatic ? "Keep open" : "باز بمونه", &checked);
        cui_progress(ui, 0.4f, .height = 8);
        cui_row(ui, .id = "footer", .width = CUI_FILL, .gap = 8, .justify = CUI_JUSTIFY_END) {
            cui_button(ui, automatic ? "Cancel" : "بی‌خیال", .variant = CUI_BUTTON_OUTLINE);
            if (cui_button(ui, automatic ? "Continue" : "ادامه", .variant = CUI_BUTTON_PRIMARY, .icon_right = CUI_ICON_ARROW_RIGHT)) clicked[5]++;
            button_ids[5] = cui_last_id(ui);
        }
    }
}
static void frames(cui_ctx *ui, bool automatic, int count) {
    for (int f=0; f<count; f++) { cui_begin_frame(ui, 1.0f/60); screen(ui, automatic); cui_end_frame(ui); }
}
static void alignment_cases(cui_ctx *ui) {
    cui_begin_frame(ui,1.0f/60);
    cui_column(ui,.width=CUI_FILL,.pad_left=12,.pad_right=28,.gap=4) {
        for(int type=0;type<4;type++) {
            cui_box(ui,.dir=(cui_dir)type,.width=CUI_PX(180),.height=CUI_PX(80),.pad_left=8,.pad_right=20,
                    .gap=8,.align=CUI_ALIGN_END,.justify=CUI_JUSTIFY_END,.columns=2,.wrap=type==CUI_ROW) {
                for(int k=0;k<3;k++) cui_box(ui,.width=CUI_PX(48),.height=CUI_PX(20)) {}
            }
        }
        cui_row(ui,.direction=CUI_DIRECTION_AUTO,.width=CUI_PX(180)) {cui_text(ui,"123 … سلام",.nowrap=true);}
        cui_row(ui,.direction=CUI_DIRECTION_AUTO,.width=CUI_PX(180)) {cui_text(ui,"123 … Hello",.nowrap=true);}
        cui_row(ui,.direction=CUI_DIRECTION_AUTO,.width=CUI_PX(180)) {cui_text(ui,"123 …",.nowrap=true);}
    }
    cui_end_frame(ui);
}
static cui_ctx *context(cui_sw_renderer *sw, cui_direction d, const char *font) {
    cui_ctx *ui = cui_create(&(cui_ctx_desc){.renderer=cui_sw_iface(sw),.theme=dark?CUI_THEME_DARK:CUI_THEME_LIGHT,.direction=d});
    cui_set_viewport(ui,W,H,1);
    cui_set_reduced_motion(ui,true);
    if (font) {
        cui_font ft=cui_font_load_file(ui,font); CHECK(ft>=0,"font load failed");
        cui_font_set_role(ui,CUI_FONT_BODY,ft); cui_font_set_role(ui,CUI_FONT_MEDIUM,ft); cui_font_set_role(ui,CUI_FONT_DISPLAY,ft);
    }
    return ui;
}
static void save(cui_ctx *ui, cui_sw_renderer *sw, const char *prefix, const char *suffix) {
    char path[1024]; snprintf(path,sizeof path,"%s-%s.png",prefix,suffix);
    const cui_draw_data *dd=cui_get_draw_data(ui);
    unsigned char *px=malloc(W*H*4); CHECK(px,"pixels allocation failed");
    cui_sw_render(sw,dd,px,W,H,W*4,CUI_SW_RGBA); CHECK(cui_write_png(path,px,W,H,W*4),"PNG failed"); free(px);
    snprintf(path,sizeof path,"%s-%s.json",prefix,suffix); FILE *out=fopen(path,"w"); CHECK(out,"JSON failed");
    fprintf(out,"[\n");
    for(int i=0;i<ui->node_count;i++) {
        cui_node *n=&ui->nodes[i];
        fprintf(out,"{\"node\":%d,\"parent\":%d,\"rtl\":%d,\"x\":%.4f,\"y\":%.4f,\"w\":%.4f,\"h\":%.4f}%s\n",i,n->parent,n->rtl,n->rect.x,n->rect.y,n->rect.w,n->rect.h,i+1<ui->node_count?",":"");
    }
    fprintf(out,"]\n"); fclose(out);
}
static void left_alignment(cui_sw_renderer *sw) {
    cui_ctx *ui = context(sw, CUI_DIRECTION_INHERIT, NULL);
    cui_begin_frame(ui, 1.0f/60);
    cui_row(ui, .width = CUI_FILL, .height = CUI_PX(80)) {
        cui_text(ui, "same", .nowrap = true);
        cui_text(ui, "same", .nowrap = true, .align = CUI_TEXT_LEFT);
    }
    cui_end_frame(ui);
    cui_rect a = ui->nodes[2].rect, b = ui->nodes[3].rect;
    CHECK(fabsf(a.y - b.y) < 0.001f && fabsf(a.h - b.h) < 0.001f && b.h < 80,
          "physical LEFT must retain upstream row centering");
    printf("explicit LEFT: y=%.6f h=%.6f, same as default\n", b.y, b.h);
    cui_destroy(ui);
}
static void scroll_frame(cui_ctx *ui) {
    cui_begin_frame(ui, 1.0f/60);
    cui_row(ui, .id = "overflow", .width = CUI_PX(100), .height = CUI_PX(40), .scroll_x = true) {
        for (int k = 0; k < 3; k++) cui_box(ui, .width = CUI_PX(80), .height = CUI_PX(30)) {}
    }
    cui_end_frame(ui);
}
static void scroll_overflow(cui_sw_renderer *sw) {
    for (int rtl = 0; rtl < 2; rtl++) {
        cui_ctx *ui = context(sw, rtl ? CUI_DIRECTION_RTL : CUI_DIRECTION_LTR, NULL);
        scroll_frame(ui); scroll_frame(ui);
        cui_node *n = &ui->nodes[1];
        cui_rect view = n->rect;
        float start = ui->nodes[2].rect.x;
        CHECK(fabsf(n->content_w - 240) < 0.001f, "scroll content extent lost");
        cui_input_mouse_move(ui, view.x + 50, view.y + 20);
        cui_input_scroll(ui, -4, 0); scroll_frame(ui);
        cui_scroll_state *st = cui_state(ui, ui->nodes[1].id, sizeof *st);
        CHECK(st->tx == 140 && st->x == 140, "wheel cannot reach overflow");
        CHECK(fabsf(ui->nodes[2].rect.x - start - (rtl ? 140 : -140)) < 0.001f,
              "horizontal scroll moved in wrong direction");
        cui_rect last = ui->nodes[4].rect;
        CHECK(last.x >= view.x && last.x + last.w <= view.x + view.w, "last child unreachable");
        cui_input_scroll(ui, 4, 0); scroll_frame(ui);
        CHECK(st->x == 0 && st->tx == 0 && ui->nodes[2].rect.x == start, "cannot scroll back to start");
        cui_destroy(ui);
    }
    printf("LTR/RTL overflow: content 240, viewport 100, wheel reaches 140 and returns to 0\n");
}
static void popover_direction(cui_sw_renderer *sw) {
    for (int mode = 0; mode < 3; mode++) {
        cui_ctx *ui = context(sw, mode == 1 ? CUI_DIRECTION_RTL : CUI_DIRECTION_LTR, NULL);
        cui_begin_frame(ui, 1.0f/60);
        cui_column(ui, .direction = mode == 0 ? CUI_DIRECTION_RTL : mode == 1 ? CUI_DIRECTION_LTR : CUI_DIRECTION_AUTO,
                   .width = CUI_FILL) {
            cui_text(ui, "سلام");
            cui_popover_open(ui, "popup");
            cui_popover(ui, "popup", .width = 180) { cui_text(ui, "Hello"); }
        }
        cui_end_frame(ui);
        CHECK(ui->float_count == 1, "popover missing");
        cui_node *popup = &ui->nodes[ui->float_roots[0]];
        CHECK(popup->rtl == (mode == 0 || (shaping && mode == 2)) && ui->nodes[popup->first].rtl == popup->rtl,
              "popover must inherit opening container direction");
        cui_destroy(ui);
    }
    printf("popover direction: explicit RTL, explicit LTR and resolved AUTO inherited\n");
}
static void placeholder_direction(cui_sw_renderer *sw, const char *font) {
    size_t bytes = W * H * 4;
    unsigned char *pixels = malloc(bytes * 2); CHECK(pixels, "placeholder pixels");
    for (int rtl = 0; rtl < 2; rtl++) {
        for (int root_rtl = 0; root_rtl < 2; root_rtl++) {
            cui_ctx *ui = context(sw, root_rtl ? CUI_DIRECTION_RTL : CUI_DIRECTION_LTR, font);
            char empty[2] = "";
            cui_begin_frame(ui, 1.0f/60);
            cui_column(ui, .direction = rtl ? CUI_DIRECTION_RTL : CUI_DIRECTION_LTR, .width = CUI_FILL) {
                cui_input(ui, "placeholder", empty, sizeof empty, .placeholder = "Timer: سلام 123.", .width = CUI_FILL);
            }
            cui_end_frame(ui);
            cui_sw_render(sw, cui_get_draw_data(ui), pixels + bytes * root_rtl, W, H, W*4, CUI_SW_RGBA);
            cui_destroy(ui);
        }
        CHECK(!memcmp(pixels, pixels + bytes, bytes), "placeholder used context instead of container direction");
    }
    free(pixels);
    printf("LTR/RTL placeholders: 0 differing pixels between opposite context directions\n");
}
int main(int argc,char **argv) {
    dark=argc>3;
    cui_sw_renderer *sw=cui_sw_create();
    left_alignment(sw); scroll_overflow(sw); popover_direction(sw);
    placeholder_direction(sw, argc>1?argv[1]:NULL);
    cui_ctx *ltr=context(sw,CUI_DIRECTION_LTR,argc>1?argv[1]:NULL);
    cui_ctx *rtl=context(sw,CUI_DIRECTION_RTL,argc>1?argv[1]:NULL);
    frames(ltr,false,3); frames(rtl,false,3);
    CHECK(ltr->node_count==rtl->node_count,"tree differs");
    for (int i=0;i<ltr->node_count;i++) {
        cui_rect a=ltr->nodes[i].rect,b=rtl->nodes[i].rect;
        if(fabsf(b.x-(W-a.x-a.w))>0.5f || fabsf(b.y-a.y)>0.5f || fabsf(b.w-a.w)>0.5f || fabsf(b.h-a.h)>0.5f) {
            fprintf(stderr,"mirror failed node %d LTR %.2f %.2f %.2f %.2f RTL %.2f %.2f %.2f %.2f\n",i,a.x,a.y,a.w,a.h,b.x,b.y,b.w,b.h);return 1;
        }
    }
    printf("mirror passed: all %d node rectangles within 0.5 px\n",ltr->node_count);
    int buttons=0;
    for(int k=0;k<6;k++) {
        cui_rect b=cui__prev_rect(rtl,button_ids[k]);
        int before=clicked[k];
        cui_input_mouse_move(rtl,b.x+b.w/2,b.y+b.h/2); frames(rtl,false,1);
        cui_input_mouse_button(rtl,CUI_MOUSE_LEFT,true);frames(rtl,false,1);
        cui_input_mouse_button(rtl,CUI_MOUSE_LEFT,false);frames(rtl,false,1);
        CHECK(clicked[k]==before+1,"click triggered the wrong button");buttons++;
    }
    CHECK(buttons==6,"RTL button click failed"); printf("RTL clicks passed: header, four rows, footer action\n");
    cui_input_mouse_leave(rtl);frames(rtl,false,2);
    if(argc>2) {save(ltr,sw,argv[2],"ltr");save(rtl,sw,argv[2],"rtl");}
    cui_ctx *automatic=context(sw,CUI_DIRECTION_LTR,argc>1?argv[1]:NULL);frames(automatic,true,3);
    int rows=0;
    for(int i=0;i<automatic->node_count;i++) if(automatic->nodes[i].direction==CUI_DIRECTION_AUTO) {
        bool expected=shaping&&(rows==0||rows==2);
        CHECK(automatic->nodes[i].rtl==expected,"AUTO first strong failed");
        for(int j=i+1;j<automatic->node_count && automatic->nodes[j].parent>=i;j++) CHECK(automatic->nodes[j].rtl==expected,"row subtitle direction differs");
        rows++;
    }
    CHECK(rows==4,"AUTO rows missing");printf("AUTO passed: %s\n",shaping?"first strong":"LTR without shaping");
    if(argc>2)save(automatic,sw,argv[2],"auto");
    alignment_cases(ltr);alignment_cases(rtl);
    for(int i=0;i<ltr->node_count;i++) {
        cui_node *a=&ltr->nodes[i],*b=&rtl->nodes[i];
        if(a->parent<0 || ltr->nodes[a->parent].direction!=CUI_DIRECTION_AUTO)
            CHECK(fabsf(b->rect.x-(W-a->rect.x-a->rect.w))<=0.5f,"alignment/padding mirror failed");
        if(a->direction==CUI_DIRECTION_AUTO) {
            if(i==ltr->node_count-2) CHECK(!a->rtl&&b->rtl==shaping,"neutral AUTO direction");
        }
    }
    CHECK(ltr->nodes[ltr->node_count-6].rtl==shaping&&rtl->nodes[rtl->node_count-6].rtl==shaping,"AUTO Persian direction");
    CHECK(!ltr->nodes[ltr->node_count-4].rtl&&!rtl->nodes[rtl->node_count-4].rtl,"AUTO must skip digits before English");
    printf("asymmetric padding, end alignment, wrapped row, grid, stack and neutral AUTO passed\n");
    cui_destroy(ltr);cui_destroy(rtl);cui_destroy(automatic);cui_sw_destroy(sw);return 0;
}
