/* history_host_test.c -- cmd_history.c's checks, on a PC (2026-10-07).
 * gcc -std=c99 -I. tools/history_host_test.c cmd_history.c -o build/history_host_test.exe */
#include <stdio.h>
#include <string.h>

#include "cmd_history.h"
#include "kbd_seq.h"

static int g_failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); g_failures++; } } while (0)

static uint32_t g_now = 1000;
static void tap(hist_ui_t *u, uint8_t key) {
    hist_ui_key(u, key, g_now += 50);
    hist_ui_key(u, 0, g_now += 50);
}
static int shows(const hist_ui_t *u, const char *want) {
    char line[HIST_WIDTH + 1], pad[HIST_WIDTH + 1];
    hist_ui_render(u, line);
    line[HIST_WIDTH] = 0;
    snprintf(pad, sizeof pad, "%-26s", want);
    if (strcmp(line, pad)) printf("  shows [%s], wanted [%s]\n", line, pad);
    return strcmp(line, pad) == 0;
}
static void add(hist_t *h, const char *s) { hist_add(h, s, strlen(s)); }
/* Opened, and the arrow that opened it let go. */
static bool start(hist_ui_t *u, const hist_t *h, hist_start_t how) {
    bool ok = hist_ui_start(u, h, how);
    hist_ui_key(u, 0, g_now += 50);
    return ok;
}

int main(void) {
    static hist_t h;
    hist_ui_t u;
    hist_init(&h);
    CHECK(!start(&u, &h, HIST_START_OLDER)); /* none yet */
    add(&h, "PRINT 1");
    add(&h, "A=5");
    add(&h, "A=5"); /* a repeat: not again */
    add(&h, "   ");
    add(&h, "PRINT A*2  ");
    CHECK(h.count == 3);
    CHECK(hist_get(&h, 1)->len == 9); /* trailing spaces dropped */

    /* browse: newest first, Up older, stops at the ends; Left edits */
    CHECK(start(&u, &h, HIST_START_OLDER));
    CHECK(shows(&u, "PRINT A*2"));
    tap(&u, KBD_K_UP);
    CHECK(shows(&u, "A=5"));
    tap(&u, KBD_K_UP);
    tap(&u, KBD_K_UP);
    CHECK(shows(&u, "PRINT 1"));
    tap(&u, KBD_K_DOWN);
    CHECK(shows(&u, "A=5"));
    CHECK(hist_ui_indicators(&u) == 0x80);
    tap(&u, KBD_K_LEFT);
    CHECK(hist_ui_result(&u) == HIST_UI_EDIT && hist_ui_selected(&u)->text[0] == 'A');
    /* ENTER runs; DEF, CL and BREAK leave */
    start(&u, &h, HIST_START_NEWER);
    tap(&u, KBD_K_ENTER);
    CHECK(hist_ui_result(&u) == HIST_UI_RUN && hist_ui_selected(&u)->len == 9);
    start(&u, &h, HIST_START_OLDER);
    tap(&u, KBD_K_DEF);
    CHECK(hist_ui_result(&u) == HIST_UI_CANCEL && !hist_ui_selected(&u));
    start(&u, &h, HIST_START_OLDER);
    tap(&u, KBD_K_CL);
    CHECK(hist_ui_result(&u) == HIST_UI_CANCEL);
    start(&u, &h, HIST_START_OLDER);
    hist_ui_break(&u);
    CHECK(hist_ui_result(&u) == HIST_UI_BREAK && !hist_ui_selected(&u));

    /* holding Up repeats */
    start(&u, &h, HIST_START_OLDER);
    hist_ui_key(&u, KBD_K_UP, g_now += 10);
    hist_ui_key(&u, KBD_K_UP, g_now += 600);
    CHECK(shows(&u, "PRINT 1"));
    hist_ui_key(&u, 0, g_now += 10);

    /* search: any case, the newest match; DEF+Left the next older; SHIFT+Left a character less */
    start(&u, &h, HIST_START_SEARCH);
    CHECK(shows(&u, "> PRINT A*2"));
    tap(&u, KBD_K_P);
    tap(&u, KBD_K_R);
    CHECK(shows(&u, "PR> PRINT A*2"));
    tap(&u, KBD_K_DEF);
    CHECK(hist_ui_indicators(&u) == 0x80);
    tap(&u, KBD_K_LEFT);
    CHECK(shows(&u, "PR> PRINT 1"));
    tap(&u, KBD_K_DOWN);
    CHECK(shows(&u, "PR> PRINT A*2"));
    tap(&u, KBD_K_X);
    CHECK(shows(&u, "PRX> (NONE)"));
    tap(&u, KBD_K_SHIFT);
    tap(&u, KBD_K_LEFT);
    CHECK(shows(&u, "PR> PRINT A*2"));
    tap(&u, KBD_K_SHIFT);
    tap(&u, KBD_K_ASTERISK); /* SHIFT+* is ':' -- none has it */
    CHECK(shows(&u, "PR:> (NONE)"));
    tap(&u, KBD_K_ENTER); /* nothing to run: as if cancelled */
    CHECK(hist_ui_result(&u) == HIST_UI_CANCEL);
    start(&u, &h, HIST_START_SEARCH);
    tap(&u, KBD_K_EQUALS);
    tap(&u, KBD_K_RIGHT);
    CHECK(hist_ui_result(&u) == HIST_UI_EDIT && hist_ui_selected(&u)->text[1] == '=');

    /* full: the oldest goes */
    for (int i = 0; i < HIST_ENTRIES + 5; i++) {
        char s[16];
        snprintf(s, sizeof s, "PRINT %d", i);
        add(&h, s);
    }
    CHECK(h.count == HIST_ENTRIES);
    CHECK(strncmp(hist_get(&h, HIST_ENTRIES)->text, "PRINT 5", 7) == 0);
    printf("history checks: %s\n", g_failures ? "FAILED" : "pass");
    return g_failures != 0;
}
