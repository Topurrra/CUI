#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif
#include "../src/cui_internal.h"
#include <cui/cui_render.h>
#define CHECK(x, msg) do { if (!(x)) { fprintf(stderr, "%s at %d\n", msg, __LINE__); exit(1); } } while (0)
static cui_ctx *context(cui_sw_renderer *sw, const char *path) {
    cui_ctx *ui = cui_create(&(cui_ctx_desc){.renderer=cui_sw_iface(sw),.theme=CUI_THEME_LIGHT});
    cui_font ft=cui_font_load_file(ui,path); CHECK(ft>=0,"font load failed");
    cui_font_set_role(ui,CUI_FONT_BODY,ft); cui_set_viewport(ui,900,1200,1); return ui;
}
static double now(void) {
#if defined(_WIN32)
    LARGE_INTEGER f,c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (double)c.QuadPart/f.QuadPart;
#else
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec*1e-9;
#endif
}
static const char *samples[] = {"سلام دنیا","امروز ۳ تا Timer دارم","می‌خوام","میخوام","لا","Timer: سلام ۱۲۳ دنیا.","(سلام) Timer 123"};
static const char *para = "امروز می‌خوام یه متن طولانی فارسی بنویسم تا ببینم وقتی خط‌ها شکسته می‌شن، واژه‌ها درست دیده می‌شن یا نه. سه تا Timer دارم و باید همه‌شون رو تا فردا بررسی کنم. سلام دنیا، این یه آزمایش برای نمایش نوشته‌های فارسی در چند خطه. برنامه Edge این متن رو با ۱۲۳ و 456 و واژه‌های English کنار هم نمایش می‌ده. فردا دوباره همه چیز رو بررسی می‌کنم تا مطمئن بشم هیچ کلمه‌ای جا نمونده و ترتیب جمله‌ها درست مونده.";
static const char *english = "Today I want to write a long English paragraph to see how the lines wrap and whether every word stays in the right place. I have three Timer tasks and need to review all of them tomorrow. Hello world, this is a test of text in several lines. The Edge program displays 123 and 456 and English words together. Tomorrow I will review everything again to make sure no word is missing and every sentence stays in the right order.";
static void paragraph_frame(cui_ctx *ui,const char *text,int width,bool print) {
    cui_begin_frame(ui,1.0f/60);
    cui_column(ui,.width=CUI_PX(width+48),.pad=24) {cui_text(ui,text,.size=24,.line_height=1.5f);}
    cui_end_frame(ui);
    if(print)for(int i=0;i<ui->node_count;i++) {
        cui_node *n=&ui->nodes[i];if(!n->lines)continue;
        for(int k=0;k<n->text_lines;k++) {cui_text_layout_line L=n->lines[k];
            printf("LINE %d %d %d %.6f %.*s\n",width,L.start,L.end,L.width,L.end-L.start,text+L.start);
        }
    }
}
static void png(cui_ctx *ui,cui_sw_renderer *sw,const char *path,int w,int h) {
    const cui_draw_data *dd=cui_get_draw_data(ui);unsigned char *px=malloc((size_t)w*h*4);CHECK(px,"pixels");
    cui_sw_render(sw,dd,px,w,h,w*4,CUI_SW_RGBA);CHECK(cui_write_png(path,px,w,h,w*4),"PNG");free(px);
}
int main(int argc,char **argv) {
    CHECK(argc>=3,"FONT OUT_PREFIX [bench|direction|wrap]");
    cui_sw_renderer *sw=cui_sw_create();
    bool bench=argc>3&&!strcmp(argv[3],"bench");
    if(bench) {
        size_t bytes=strlen(para)*2+2;
        char *long_para=malloc(bytes);CHECK(long_para,"long paragraph");
        snprintf(long_para,bytes,"%s %s",para,para);
        const char *texts[]={english,para,long_para};
        const char *names[]={"English","Persian","Persian long (156 words)"};
        double times[3];
        for (int fa=0;fa<3;fa++) {
            cui_ctx *ui=context(sw,argv[1]);
            for(int i=0;i<30;i++)paragraph_frame(ui,texts[fa],360,false);
            double t=now();
            for(int i=0;i<200;i++){cui_input_mouse_move(ui,300+(i%50)*2.0f,400);paragraph_frame(ui,texts[fa],360,false);}
            times[fa]=(now()-t)/200*1000;
            printf("BENCH paragraph %s %.6f ms\n",names[fa],times[fa]);
            cui_destroy(ui);
        }
        free(long_para);
        CHECK(times[1]<=.25&&times[2]<=.25,"Persian frame budget exceeded");
    }else {
        cui_ctx *measure=context(sw,argv[1]);
        const char *word="سلام";
        float single=cui_measure_text(measure,CUI_FONT_BODY,32,word,-1).width;
        char multiline[128];snprintf(multiline,sizeof multiline,"%s\n%s",word,word);
        float two=cui_measure_text(measure,CUI_FONT_BODY,32,multiline,-1).width;
        printf("MEASURE single %.6f multiline %.6f\n",single,two);
        CHECK(fabsf(single-two)<.001f,"multiline width is not the widest paragraph");
        snprintf(multiline,sizeof multiline,"\n%s\n",word);
        CHECK(fabsf(single-cui_measure_text(measure,CUI_FONT_BODY,32,multiline,-1).width)<.001f,"empty paragraphs changed width");
        cui_destroy(measure);
        for(int mode=0;mode<2;mode++) {
            cui_ctx *ui=context(sw,argv[1]);cui_set_viewport(ui,900,640,1);
            cui_begin_frame(ui,1.0f/60);cui_draw_rect(ui,(cui_rect){0,0,900,640},CUI_CORNERS(0),CUI_RGB(0xffffff));
            cui_column(ui,.width=CUI_FILL,.pad=24,.gap=24) {
                for(int k=0;k<7;k++)cui_text(ui,samples[k],.size=32,.line_height=1.5f,.nowrap=true);
            }
            cui_end_frame(ui);char path[1024];snprintf(path,sizeof path,"%s-lines-%s.png",argv[2],mode?"auto":"rtl");png(ui,sw,path,900,640);cui_destroy(ui);
        }
        for(int k=0;k<3;k++) {
            int width=(int[]){240,360,600}[k];cui_ctx *ui=context(sw,argv[1]);cui_set_viewport(ui,width+48,1200,1);
            paragraph_frame(ui,para,width,true);char path[1024];snprintf(path,sizeof path,"%s-wrap-%d.png",argv[2],width);png(ui,sw,path,width+48,1200);
            for(int i=0;i<ui->node_count;i++){cui_node *n=&ui->nodes[i];if(!n->lines)continue;
                for(int j=0;j+1<n->text_lines;j++){int e=n->lines[j+1].start;while(e<n->text_len&&n->text[e]!=' ')e++;
                    CHECK(cui_measure_text(ui,n->font,n->font_size,n->text+n->lines[j].start,e-n->lines[j].start).width>width,"shaped line wrapped prematurely");
                }
            }
            cui_destroy(ui);
        }
        printf("shaped wrap regression passed\n");
    }
    cui_sw_destroy(sw);return 0;
}
