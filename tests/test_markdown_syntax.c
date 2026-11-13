/* Tests for the markdown language of the syntax highlighter (src/cui_code.c): the token kinds of
 * headings, emphasis, links, lists, fences and front matter, line by line with the multi-line state. */
#include "../src/cui_internal.h"

#include <stdio.h>
#include <string.h>

static int g_failed, g_checks;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

enum { TEXT, KEYWORD, TYPE, STRING, NUMBER, COMMENT, FUNC, PREPROC, PUNCT, CONST };

/* the kind of the token that covers the first occurrence of `what` in s, -1 if none does */
static int kind_of(const char *s, int *state, const char *what) {
    cui__tok t[256];
    int n = cui__tokenize_md(s, (int)strlen(s), state, t, 256);
    const char *at = strstr(s, what);
    int pos = at ? (int)(at - s) : -1;
    for (int i = 0; i < n; i++)
        if (pos >= t[i].start && pos < t[i].start + t[i].len) return t[i].kind;
    return -1;
}
static int kind1(const char *s, const char *what) { int st = 0; return kind_of(s, &st, what); }

int main(void) {
    int st;
    cui__tok t[64];

    /* the names */
    CHECK(cui__lang_for("md").markdown && cui__lang_for("markdown").markdown && !cui__lang_for("c").markdown);

    /* headings */
    CHECK(kind1("# Title", "Title") == KEYWORD && kind1("###### six", "six") == KEYWORD);
    CHECK(kind1("####### seven", "seven") == TEXT && kind1("#tag", "tag") == TEXT);

    /* emphasis, and snake_case that is not */
    CHECK(kind1("a *b* c", "b") == TYPE && kind1("a *b* c", "*b") == PUNCT && kind1("a **b** c", "b") == TYPE);
    CHECK(kind1("a _b_ c", "b") == TYPE && kind1("a ~~b~~ c", "b") == COMMENT);
    CHECK(kind1("snake_case_name here", "case") == TEXT && kind1("snake_case_name here", "_case") == TEXT);
    CHECK(kind1("2 * 3 * 4", "3") == TEXT && kind1("a lone * star", "star") == TEXT);
    CHECK(kind1("`x_y`", "x_y") == STRING && kind1("a `code` b", "code") == STRING);

    /* links, wiki links, images */
    CHECK(kind1("[text](http://x.io)", "text") == FUNC && kind1("[text](http://x.io)", "http") == COMMENT);
    CHECK(kind1("[text](http://x.io)", "[") == PUNCT && kind1("![alt](a.png)", "alt") == FUNC);
    CHECK(kind1("see [[note]]", "note") == FUNC && kind1("see [[note]]", "[[") == PUNCT);
    CHECK(kind1("[[note|label]]", "note") == COMMENT && kind1("[[note|label]]", "label") == FUNC);
    CHECK(kind1("![[x.png]]", "x.png") == FUNC && kind1("![[x.png]]", "!") == PUNCT);
    CHECK(kind1("a [b] c", "b") == TEXT);

    /* lists, tasks, quotes, callouts, rules, tables */
    CHECK(kind1("- item", "-") == PUNCT && kind1("1. item", "1.") == PUNCT && kind1("- item", "item") == TEXT);
    CHECK(kind1("- [x] done", "[x]") == CONST && kind1("- [ ] todo", "[ ]") == CONST);
    CHECK(kind1("> quote", ">") == PUNCT && kind1("> [!note] Title", "[!note]") == PREPROC);
    CHECK(kind1("---", "-") == PUNCT && kind1("* * *", "*") == PUNCT);
    CHECK(kind1("| a | b |", "|") == PUNCT && kind1("|---|:-:|", "-") == PUNCT && kind1("a | b", "|") == TEXT);

    /* fences carry across lines */
    st = 0;
    CHECK(kind_of("```c", &st, "c") == COMMENT && st != 0);
    CHECK(kind_of("int a = *p; # x", &st, "int") == STRING && st != 0);
    CHECK(kind_of("~~~", &st, "~") == STRING && st != 0);      /* a different fence does not close it */
    CHECK(kind_of("```", &st, "`") == COMMENT && st == 0);
    CHECK(kind_of("# Heading", &st, "Heading") == KEYWORD);
    st = 0;
    kind_of("~~~~", &st, "~");
    kind_of("~~~", &st, "~");
    CHECK(st != 0);                                            /* a shorter fence does not close it */
    kind_of("~~~~~", &st, "~");
    CHECK(st == 0);

    /* the bool path (the editor): a block is open, whatever kind */
    {
        cui__lang L = cui__lang_for("md");
        bool b = false;
        cui__tokenize(&L, false, "```", 3, &b, t, 64);
        CHECK(b);
        CHECK(cui__tokenize(&L, false, "plain *x*", 9, &b, t, 64) == 1 && t[0].kind == STRING && b);
        cui__tokenize(&L, false, "```", 3, &b, t, 64);
        CHECK(!b);
        cui__tokenize(&L, false, "a <!-- b", 8, &b, t, 64);
        CHECK(b);
        cui__tokenize(&L, false, "still", 5, &b, t, 64);
        CHECK(b);      /* the bool cannot tell a comment from a fence: the body reads as code */
        cui__tokenize(&L, false, "end --> after", 13, &b, t, 64);
        CHECK(!b);
    }

    /* front matter, only from the first line */
    st = CUI__MD_START;
    CHECK(kind_of("---", &st, "-") == COMMENT && st != 0);
    CHECK(kind_of("title: My note", &st, "title") == FUNC && kind_of("title: My note", &st, "My") == TEXT);
    CHECK(kind_of("tags: [a, b]", &st, "[") == TEXT && st != 0);
    CHECK(kind_of("---", &st, "-") == COMMENT && st == 0);
    CHECK(kind_of("# Body", &st, "Body") == KEYWORD);
    st = CUI__MD_START;
    CHECK(kind_of("text", &st, "text") == TEXT && st == 0);
    CHECK(kind_of("---", &st, "-") == PUNCT && st == 0);       /* a rule, not front matter */

    /* HTML comments */
    st = 0;
    CHECK(kind_of("a <!-- b --> c", &st, "b") == COMMENT && kind_of("a <!-- b --> c", &st, "c") == TEXT && st == 0);
    CHECK(kind_of("<!-- start", &st, "start") == COMMENT && st != 0);
    CHECK(kind_of("# not a heading", &st, "heading") == COMMENT);
    CHECK(kind_of("end --> *em*", &st, "em") == TYPE && st == 0);

    /* odd input stays safe: tiny caps, empty and unterminated lines */
    st = 0;
    CHECK(cui__tokenize_md("", 0, &st, t, 64) == 0);
    CHECK(cui__tokenize_md("[[", 2, &st, t, 64) >= 0 && cui__tokenize_md("`a", 2, &st, t, 64) >= 0);
    CHECK(cui__tokenize_md("**a** [b](c) `d` ![[e]]", 23, &st, t, 3) == 3);
    CHECK(cui__tokenize_md("[x](", 4, &st, t, 64) >= 0 && cui__tokenize_md("![", 2, &st, t, 64) >= 0);

    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
