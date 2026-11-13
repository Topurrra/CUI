/* Tests for mermaid diagrams in the markdown view (src/cui_mermaid.c): which text is drawn as a
 * diagram and which falls back to code, the size caps, the panel's height, and random input,
 * both through cui__mermaid_block and through cui_markdown. Runs headless. */
#include "../src/cui_internal.h"
#include <cui/cui_render.h>

#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static int g_failed, g_checks;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static cui_sw_renderer *g_sw;
static cui_ctx *new_ctx(void) {
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(g_sw) });
    cui_set_viewport(ui, 800, 600, 1);
    cui_set_reduced_motion(ui, true);
    return ui;
}

typedef struct { const char *src; int len; bool ok; float natural; } mm_run;

/* a few frames of one diagram in a 640 point column; ok is what the last frame returned */
static void mm_frames(cui_ctx *ui, mm_run *m, int frames) {
    for (int f = 0; f < frames; f++) {
        cui_begin_frame(ui, 1.0f / 60.0f);
        cui_column(ui, .pad = 20, .width = CUI_PX(640)) {
            m->ok = cui__mermaid_block(ui, "d", m->src, m->len > 0 ? m->len : (int)strlen(m->src), 13, &m->natural);
        }
        cui_end_frame(ui);
    }
}
static bool mm_ok(cui_ctx *ui, const char *src) {
    mm_run m = { src, 0, false, 0 };
    mm_frames(ui, &m, 2);
    return m.ok;
}
static float mm_height(cui_ctx *ui) { return cui_get_rect(ui, cui_make_id(ui, "d")).h; }

static void test_accepts(void) {
    printf("mermaid: flowchart, pie and gantt syntax that draws\n");
    cui_ctx *ui = new_ctx();
    static const char *const good[] = {
        "graph TD\nA-->B",
        "graph LR\n  A --> B --> C\n",
        "flowchart RL\nA-->B",
        "graph BT;A-->B;B-->C",
        "graph\nA-->B",
        "flowchart TB\n  a[rect] --> b(round) --> c([stadium]) --> d[[sub]] --> e[(db)]\n  e --> f{diamond} --> g{{hex}} --> h((circle)) --> i>flag]\n  i --> j(((double))) --> k[/para/] --> l[\\alt\\]",
        "graph TD\n  A[\"quoted [text] (with) {brackets}\"] --> B[\"line<br>two<br/>three\"]",
        "graph LR\n  A --- B\n  A -.-> C\n  A ==> D\n  A -.- E\n  A === F\n  A ~~~ G\n  A <--> H\n  A --o I\n  A --x J\n  A o--o K\n  A x--x L",
        "graph TD\n  A -->|yes| B\n  A -- no --> C\n  A -. maybe .-> D\n  A == sure ==> E\n  A -- open ---F",
        "graph TD\n  A & B --> C & D\n  C --> A",
        "graph TD\n  %% a comment\n  A --> B %% a comment to the end of the line\n",
        "graph TD\n  subgraph one [First]\n    A --> B\n  end\n  subgraph two\n    C\n    subgraph inner[\"Inner\"]\n      D\n    end\n  end\n  B --> C --> D",
        "graph TD\n  A-->B\n  classDef hot fill:#f96,stroke:#333\n  class A hot\n  style B fill:#bbf\n  linkStyle 0 stroke:#f00\n  click A callback \"tip\"\n  A:::hot --> C",
        "---\ntitle: Front matter\n---\n%%{init: {'theme':'dark'}}%%\ngraph LR\nA-->B",
        "graph TD\n  A --> A\n  A --> B",
        "graph TD\n  my-node --> other.node --> x_1\n  \xe4\xb8\xad --> \xe6\x96\x87",
        "graph LR\n  A-->B\n  B-->C\n  C-->A",
        "pie\n  \"Dogs\" : 386\n  \"Cats\" : 85.5\n",
        "pie title Pets\n  \"Dogs\" : 386\n",
        "pie showData\n  title Pets adopted\n  \"Dogs\" : 386\n  \"Rats\" : 15",
        "gantt\n  title Plan\n  dateFormat YYYY-MM-DD\n  section A\n  One :done, a1, 2024-01-01, 3d\n  Two :active, a2, after a1, 1w\n  Three :crit, 2024-01-20, 2024-01-25\n  Four :12h\n  M :milestone, m1, after a2 a1, 0d\n  Five :f5, 2d\n  Six :s6, 2024-01-02, until m1",
    };
    for (int k = 0; k < (int)(sizeof good / sizeof good[0]); k++) {
        bool ok = mm_ok(ui, good[k]);
        if (!ok) printf("  rejected: %s\n", good[k]);
        CHECK(ok);
    }
    cui_destroy(ui);
}

static void test_falls_back(void) {
    printf("mermaid: other diagrams and malformed text fall back to code\n");
    cui_ctx *ui = new_ctx();
    static const char *const bad[] = {
        "", "   \n\n", "%% only a comment", "sequenceDiagram\n  A->>B: hi", "classDiagram\n  A <|-- B", "stateDiagram-v2\n  [*] --> A",
        "erDiagram\n  A ||--o{ B : has", "graphTD\nA-->B", "graph XY\nA-->B", "graph TD\nA-->", "graph TD\n-->B", "graph TD\nA[unclosed",
        "graph TD\nA(unclosed]", "graph TD\nA[\"no close quote]", "graph TD\nA -- text without end B", "graph TD\nA --> B junk junk",
        "graph TD\nA -->|unclosed B", "graph TD\nA -- B", "graph TD\nend", "graph TD\nsubgraph x\nA", "graph TD\n", "flowchart-elk TD\nA-->B",
        "pie", "pie title Only a title", "pie\n\"a\" : x", "pie\n\"a\" : -3", "pie\n\"a\" 3", "pie\nnot a slice",
        "gantt\ndateFormat DD-MM-YYYY\nA :2024-01-01, 1d", "gantt\ndateFormat YYYY-MM-DD\nA :after nobody, 1d", "gantt\nA :3d",
        "gantt\nA :2024-13-01, 1d", "gantt\nA :2024-01-05, 2024-01-01", "gantt\nA :2024-01-01, 3y", "gantt\ntitle nothing else",
        "gantt\nA :2001-01-01, 1d\nB :2030-01-01, 1d",
    };
    for (int k = 0; k < (int)(sizeof bad / sizeof bad[0]); k++) {
        bool ok = mm_ok(ui, bad[k]);
        if (ok) printf("  accepted: %s\n", bad[k]);
        CHECK(!ok);
    }
    cui_destroy(ui);
}

static void test_caps(void) {
    printf("mermaid: node and edge caps, height cap, natural width\n");
    cui_ctx *ui = new_ctx();
    static char buf[60000];
    /* 300 nodes in a chain draw; 301 fall back */
    for (int n = 300; n <= 301; n++) {
        int o = snprintf(buf, sizeof buf, "graph LR\n");
        for (int k = 0; k < n; k++) o += snprintf(buf + o, sizeof buf - (size_t)o, "n%d --> n%d\n", k, k + 1 < n ? k + 1 : 0);
        CHECK(mm_ok(ui, buf) == (n == 300));
    }
    /* 16 x 16 fans: two make 512 edges, three 768 */
    for (int rows = 2; rows <= 3; rows++) {
        int o = snprintf(buf, sizeof buf, "graph TD\n");
        for (int r = 0; r < rows; r++) {
            for (int k = 0; k < 16; k++) o += snprintf(buf + o, sizeof buf - (size_t)o, "%sa%d_%d", k ? " & " : "", r, k);
            o += snprintf(buf + o, sizeof buf - (size_t)o, " --> ");
            for (int k = 0; k < 16; k++) o += snprintf(buf + o, sizeof buf - (size_t)o, "%sb%d_%d", k ? " & " : "", r, k);
            o += snprintf(buf + o, sizeof buf - (size_t)o, "\n");
        }
        CHECK(mm_ok(ui, buf) == (rows == 2));
    }
    /* long edges past a long chain need more waypoints than the layout takes: code, quickly */
    {
        int o = snprintf(buf, sizeof buf, "graph TD\n");
        for (int k = 0; k < 150; k++) o += snprintf(buf + o, sizeof buf - (size_t)o, "c%d --> c%d\nc0 --> c%d\n", k, k + 1, k + 1);
        CHECK(!mm_ok(ui, buf));
    }
    /* 17 joined with & is past the cap */
    {
        int o = snprintf(buf, sizeof buf, "graph TD\n");
        for (int k = 0; k < 17; k++) o += snprintf(buf + o, sizeof buf - (size_t)o, "%sa%d", k ? " & " : "", k);
        snprintf(buf + o, sizeof buf - (size_t)o, " --> b\n");
        CHECK(!mm_ok(ui, buf));
    }
    /* a small chart is as tall as it is; a long top down chain stops at 520 */
    mm_run m = { "graph LR\nA-->B", 0, false, 0 };
    mm_frames(ui, &m, 3);
    CHECK(m.ok && mm_height(ui) > 40 && mm_height(ui) < 200);
    CHECK(m.natural > 100 && m.natural < 400);
    {
        int o = snprintf(buf, sizeof buf, "graph TD\n");
        for (int k = 0; k < 40; k++) o += snprintf(buf + o, sizeof buf - (size_t)o, "s%d --> s%d\n", k, k + 1);
        m.src = buf;
        mm_frames(ui, &m, 3);
        CHECK(m.ok && mm_height(ui) > 519 && mm_height(ui) < 521);
        /* drag and wheel pan it without trouble */
        cui_rect r = cui_get_rect(ui, cui_make_id(ui, "d"));
        cui_input_mouse_move(ui, r.x + r.w * 0.5f, r.y + 100);
        mm_frames(ui, &m, 1);
        cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
        mm_frames(ui, &m, 1);
        cui_input_mouse_move(ui, r.x + r.w * 0.5f, r.y + 20);
        mm_frames(ui, &m, 1);
        cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
        cui_input_scroll(ui, 0, -3);
        mm_frames(ui, &m, 2);
        CHECK(m.ok && mm_height(ui) > 519);
    }
    /* an edge back to an earlier node is laid out between the same layers, not past the end:
     * this chart fits 880 points at full size, as wide as it was without the back edge */
    {
        static const char *const sync =
            "flowchart LR\n    A[Edit note] --> B{Press Sync}\n    B -->|commit| C[(Local repo)]\n"
            "    C -->|pull --rebase| D[GitHub]\n    D -->|push| C\n    B -.->|no git| E([Show a hint])\n";
        static const char *const forward =
            "flowchart LR\n    A[Edit note] --> B{Press Sync}\n    B -->|commit| C[(Local repo)]\n"
            "    C -->|pull --rebase| D[GitHub]\n    B -.->|no git| E([Show a hint])\n";
        mm_run a = { forward, 0, false, 0 }, b = { sync, 0, false, 0 };
        mm_frames(ui, &a, 3);
        mm_frames(ui, &b, 3);
        CHECK(a.ok && b.ok);
        CHECK(b.natural < 880 && b.natural <= a.natural + 0.5f);
        /* top down, with a self loop and a cycle back to the start */
        CHECK(mm_ok(ui, "graph TD\n  S --> W{Ready?}\n  W -->|no| T --> W\n  W -->|yes| R((Run))\n  R --> R\n  R -->|again| R\n  R --> X --> S"));
        CHECK(mm_ok(ui, "graph BT\n  A --> B --> C --> A\n  A <--> B\n  B --> B"));
    }
    /* longer than the source cap */
    {
        static char big[70000];
        memset(big, ' ', sizeof big - 1);
        memcpy(big, "graph TD\nA-->B\n", 15);
        big[sizeof big - 1] = 0;
        CHECK(!mm_ok(ui, big));
    }
    cui_destroy(ui);
}

/* Back edges are routed beside the forward edges: no routed edge crosses another or runs through a
 * box that is not its end, top down and left to right, for flowcharts with cycles. */
static void test_crossings(void) {
    printf("mermaid: cycles are routed without crossings\n");
    cui_ctx *ui = new_ctx();
    static const char *const body[] = {
        "  A[Start] --> B{Valid?}\n  B -- yes --> C[Process]\n  B -- no --> D[Fix input]\n  D --> B\n  C --> E[Save]\n  E --> F{More?}\n  F -- yes --> A\n  F -- no --> G[Done]\n",
        "  A-->B; B-->C; C-->A; B-->D\n",
        "  A-->B-->C-->D; D-->A\n",
        "  A-->B-->C-->D; D-->B\n",
        "  A-->B; A-->C; B-->D; C-->D; D-->A\n",
        "  A-->B-->C; C-->B; C-->A\n",
        "  S --> W{Ready?}\n  W -->|no| T --> W\n  W -->|yes| R((Run))\n  R --> X --> S\n",
        "  A-->B; B-->A\n",
    };
    static const char *const heads[] = { "graph TD\n", "graph LR\n", "graph BT\n", "graph RL\n" };
    for (int h = 0; h < 4; h++)
        for (int k = 0; k < (int)(sizeof body / sizeof body[0]); k++) {
            static char src[1024];
            int cr, hit, n = snprintf(src, sizeof src, "%s%s", heads[h], body[k]);
            bool ok = cui__mermaid_probe(ui, src, n, 13, &cr, &hit);
            if (!ok || cr || hit) printf("  %d crossings, %d box hits: %s\n", cr, hit, src);
            CHECK(ok && cr == 0 && hit == 0);
        }
    cui_destroy(ui);
}

/* A diagram wider than its column shrinks to the width (no sideways scrolling) and offers an
 * expanded view when the text would be small; Esc closes it. */
static void test_fit(void) {
    printf("mermaid: wide diagrams fit the width, small ones expand\n");
    cui_ctx *ui = new_ctx();
    float zoom, natural;
    bool expand, big;
    /* 600 points of room (the column is 640 with a pad of 20): about 1.0 fits, and 0.3 is a big shrink */
    mm_run a = { "graph LR\n  A --> B", 0, false, 0 };
    mm_frames(ui, &a, 4);
    CHECK(cui__mermaid_view_info(ui, "d", &zoom, &expand, &big, &natural));
    CHECK(zoom == 1.0f && !expand && !big);
    static char wide[600];
    int o = snprintf(wide, sizeof wide, "graph LR\n");
    for (int k = 0; k < 7; k++) o += snprintf(wide + o, sizeof wide - (size_t)o, "  Step%d[Stage number %d] --> Step%d\n", k, k, k + 1);
    mm_run w = { wide, 0, false, 0 };
    mm_frames(ui, &w, 4);
    CHECK(w.ok && cui__mermaid_view_info(ui, "d", &zoom, &expand, &big, &natural));
    float panel = cui_get_rect(ui, cui_make_id(ui, "d")).w, want = (panel - 32.0f) / (natural - 32.0f);   /* natural has the padding in it */
   CHECK(natural > 1000);
    CHECK(fabsf(zoom - want) < 0.01f && zoom < 0.55f && zoom >= 0.25f);   /* it used to stop at 0.55 and scroll */
    CHECK(expand && !big);
    /* the button is in the top right corner; a click opens the expanded view, Esc closes it */
    cui_rect r = cui_get_rect(ui, cui_make_id(ui, "d"));
    cui_input_mouse_move(ui, r.x + r.w - 6 - 14, r.y + 6 + 14);
    mm_frames(ui, &w, 3);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
    mm_frames(ui, &w, 2);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
    mm_frames(ui, &w, 3);
    CHECK(cui__mermaid_view_info(ui, "d", &zoom, &expand, &big, &natural) && big);
    cui_input_key(ui, CUI_KEY_ESCAPE, true, 0);
    mm_frames(ui, &w, 1);
    cui_input_key(ui, CUI_KEY_ESCAPE, false, 0);
    mm_frames(ui, &w, 3);
    CHECK(cui__mermaid_view_info(ui, "d", &zoom, &expand, &big, &natural) && !big);
    /* an even wider one stops at the floor */
    static char huge[2000];
    o = snprintf(huge, sizeof huge, "graph LR\n");
    for (int k = 0; k < 40; k++) o += snprintf(huge + o, sizeof huge - (size_t)o, "  S%d[Stage %d] --> S%d\n", k, k, k + 1);
    w.src = huge;
    mm_frames(ui, &w, 4);
    CHECK(w.ok && cui__mermaid_view_info(ui, "d", &zoom, &expand, &big, &natural) && zoom == 0.25f && expand);
    cui_destroy(ui);
}

static unsigned g_rng = 0x9E3779B9u;
static unsigned rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }

typedef struct { const char *md; } md_run;
static void build_md(cui_ctx *ui, void *u) {
    md_run *r = u;
    cui_column(ui, .pad = 10, .width = CUI_PX(500)) { cui_markdown(ui, "md", r->md); }
}

static void test_random(void) {
    printf("mermaid: random input, directly and through markdown\n");
    cui_ctx *ui = new_ctx();
    static const char *const heads[] = { "graph TD\n", "graph LR\n", "flowchart BT\n", "pie\n", "gantt\n", "" };
    static const char *const bits[] = { "A", "B", "c1", "-->", "---", "-.->", "==>", "--", "-.", "==", "~~~", "<", ">", "o", "x",
                                        "|", "|t|", "[", "]", "(", ")", "{", "}", "((", "))", "[(", ")]", "\"", "&", ";", " ", "\n",
                                        "subgraph S\n", "end\n", "%%", ":::", "<br>", "\"x\" : 4\n", "title T\n", "dateFormat YYYY-MM-DD\n",
                                        "t :a, 2024-01-01, 3d\n", "u :after a, 2d\n", "section s\n", ":", ",", "\xc3\xa9", "\xff", "2024-02-30" };
    static char src[2048], md[2200];
    for (int iter = 0; iter < 3000; iter++) {
        int o = snprintf(src, sizeof src, "%s", heads[rnd() % 6]);
        int parts = (int)(rnd() % 80);
        for (int k = 0; k < parts; k++) {
            const char *b = bits[rnd() % (sizeof bits / sizeof bits[0])];
            int bl = (int)strlen(b);
            if (o + bl >= (int)sizeof src - 1) break;
            memcpy(src + o, b, (size_t)bl);
            o += bl;
        }
        src[o] = 0;
        mm_run m = { src, (int)(rnd() % 2 ? 0 : 1 + rnd() % (unsigned)(o + 1)), false, 0 };
        if (m.len > o) m.len = 0;
        if (m.len == 0 && o == 0) continue;
        mm_frames(ui, &m, 1);
        if (iter % 4 == 0) {
            snprintf(md, sizeof md, "Text\n\n```mermaid\n%s\n```\n\nmore", src);
            md_run r = { md };
            for (int f = 0; f < 2; f++) { cui_begin_frame(ui, 1.0f / 60.0f); build_md(ui, &r); cui_end_frame(ui); }
        }
    }
    CHECK(true);
    cui_destroy(ui);
}

int main(void) {
    g_sw = cui_sw_create();
    test_accepts();
    test_falls_back();
    test_caps();
    test_crossings();
    test_fit();
    test_random();
    cui_sw_destroy(g_sw);
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
