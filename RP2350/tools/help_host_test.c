/* help_host_test.c -- help.c's checks, on a PC (2026-11-10). From RP2350/:
 * gcc -std=c99 -I. tools/help_host_test.c help.c basic_detok.c -o build/help_host_test.exe
 * Run from RP2350/ too: it reads keywords.c for the module's token names. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "basic_detok.h"
#include "help.h"
#include "kbd_seq.h"
#include "pc_exp.h"

static int g_failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); g_failures++; } } while (0)

static uint32_t g_now = 1000;
static void tap(help_ui_t *u, uint8_t key) {
    help_ui_key(u, key, g_now += 50);
    help_ui_key(u, 0, g_now += 50);
}
static int shows(const help_ui_t *u, const char *want) {
    char line[HELP_WIDTH + 1], pad[HELP_WIDTH + 1];
    help_ui_render(u, line);
    line[HELP_WIDTH] = 0;
    snprintf(pad, sizeof pad, "%-26s", want);
    if (strcmp(line, pad)) printf("  shows [%s], wanted [%s]\n", line, pad);
    return strcmp(line, pad) == 0;
}
static int has(const char *name) {
    uint16_t m[1];
    size_t t;
    const help_entry_t *h = help_table(&t);
    return help_match(name, strlen(name), m, 1) == 1 && strcmp(h[m[0]].name, name) == 0;
}
static size_t count(const char *q) {
    uint16_t m[256];
    return help_match(q, strlen(q), m, 256);
}

int main(void) {
    size_t n;
    const help_entry_t *h = help_table(&n);

    /* the table: sorted, one line per name, each fits */
    for (size_t i = 0; i < n; i++) {
        size_t len = strlen(h[i].text);
        if (len == 0 || len > HELP_LINE_MAX) printf("  %s: %u characters\n", h[i].name, (unsigned)len);
        CHECK(len > 0 && len <= HELP_LINE_MAX);
        /* only what the PC-1500 shows as ASCII ([ and ] go out as the ROM's
         * TERM glyphs); 5CH, 5EH-60H, 7CH and 7EH-7FH unchecked */
        if (strpbrk(h[i].text, "\\^_`|~")) printf("  %s: %s\n", h[i].name, h[i].text);
        CHECK(strpbrk(h[i].text, "\\^_`|~") == NULL);
        if (i && strcmp(h[i - 1].name, h[i].name) >= 0) printf("  %s before %s\n", h[i - 1].name, h[i].name);
        CHECK(i == 0 || strcmp(h[i - 1].name, h[i].name) < 0);
    }
    CHECK(n <= 256);

    /* every BASIC keyword (TRM sec.5-2's, as basic_detok.c has them) */
    for (uint32_t code = 0xE000; code <= 0xF1FF; code++) {
        const char *name = basic_detok_builtin((uint16_t)code);
        if (name && !has(name)) {
            printf("  no help for %s\n", name);
            CHECK(0);
        }
    }
    /* ...and every one of this module's: keywords.c's kModuleTokens */
    {
        FILE *f = fopen("keywords.c", "rb");
        static char src[400000];
        size_t len, found = 0;
        char *p;
        CHECK(f != NULL);
        if (f) {
            len = fread(src, 1, sizeof src - 1, f);
            src[len] = 0;
            fclose(f);
            p = strstr(src, "kModuleTokens[] = {");
            CHECK(p != NULL);
            for (char *end = p ? strstr(p, "};") : NULL; p && (p = strstr(p, "{0xE1")) && p < end; p++) {
                char name[16];
                char *q = strchr(p, '"');
                size_t k = (size_t)(strchr(q + 1, '"') - q - 1);
                memcpy(name, q + 1, k);
                name[k] = 0;
                found++;
                if (!has(name)) {
                    printf("  no help for %s\n", name);
                    CHECK(0);
                }
            }
            CHECK(found > 70);
        }
    }
    CHECK(has("HELP"));

    /* matching: an exact name alone, else every name holding it */
    CHECK(count("SDLOAD") == 1);
    CHECK(count("LOAD") == 5); /* BLLOAD CLOAD FNLOAD SDLOAD STLOAD */
    CHECK(count(" load ") == 5);
    CHECK(count("\"LOAD\"") == 5);
    CHECK(count("PRINT") == 1);
    CHECK(count("RINT") == 5); /* BLPRINT GPRINT LPRINT PRINT SDPRINT */
    CHECK(count("QQQ") == 0);
    CHECK(count("") == n);

    /* the viewer */
    {
        help_ui_t u;
        CHECK(!help_ui_start(&u, "QQQ", 3));
        CHECK(help_ui_start(&u, "SDLOAD", 6));
        CHECK(u.count == 1);
        help_ui_key(&u, KBD_K_ENTER, g_now += 50); /* the ENTER that ran it: not a press */
        CHECK(u.result == HELP_UI_OPEN);
        help_ui_key(&u, 0, g_now += 50);
        CHECK(shows(&u, "SDLOAD [M] [name$[,addr]]:"));
        tap(&u, KBD_K_LEFT);
        CHECK(u.col == 0);
        tap(&u, KBD_K_RIGHT);
        CHECK(shows(&u, "DLOAD [M] [name$[,addr]]: "));
        for (int i = 0; i < 100; i++) tap(&u, KBD_K_RIGHT);
        CHECK((size_t)u.col + HELP_WIDTH == strlen(h[u.match[0]].text)); /* stops at the end */
        CHECK(shows(&u, " a binary; no name: browse"));
        tap(&u, KBD_K_DOWN); /* nothing below */
        CHECK(u.sel == 0);
        /* held: repeats after the delay */
        help_ui_key(&u, KBD_K_LEFT, g_now += 50);
        size_t c = u.col;
        help_ui_key(&u, KBD_K_LEFT, g_now += 400); /* not yet: 500 ms */
        CHECK(u.col == c);
        help_ui_key(&u, KBD_K_LEFT, g_now += 200);
        CHECK(u.col == c - 1);
        help_ui_key(&u, KBD_K_LEFT, g_now += 100); /* then every 80 ms */
        CHECK(u.col == c - 2);
        help_ui_key(&u, 0, g_now += 50);
        tap(&u, KBD_K_CL);
        CHECK(u.result == HELP_UI_DONE);

        CHECK(help_ui_start(&u, "LOAD", 4));
        help_ui_key(&u, 0, g_now += 50);
        CHECK(shows(&u, "BLLOAD [M] [name$[,addr]]:"));
        tap(&u, KBD_K_RIGHT);
        tap(&u, KBD_K_DOWN); /* the next match, from its start */
        CHECK(u.col == 0);
        CHECK(shows(&u, "CLOAD [\"name\"] / CLOAD? [\""));
        tap(&u, KBD_K_DOWN);
        tap(&u, KBD_K_DOWN);
        tap(&u, KBD_K_DOWN);
        CHECK(shows(&u, "STLOAD: brings back the RA"));
        tap(&u, KBD_K_DOWN);
        CHECK(u.sel == 4);
        tap(&u, KBD_K_UP);
        CHECK(shows(&u, "SDLOAD [M] [name$[,addr]]:"));
        help_ui_break(&u);
        CHECK(u.result == HELP_UI_BREAK);
    }

    /* in TERM's window */
    {
        static uint8_t w[0x200];
        uint8_t count0;
        memset(w, 0, sizeof w);
        w[0] = 3;
        memcpy(w + 1, "QQQ", 3);
        CHECK(help_command(w) == EXP_STATUS_ERROR);
        CHECK(!help_terminal());
        w[0] = 6;
        memcpy(w + 1, "SDLOAD", 6);
        w[EXP_SSH_TERM_CLOSED] = 9;
        w[EXP_SSH_TERM_BREAK_COUNT] = 7;
        count0 = w[EXP_SSH_TERM_LINE_COUNT];
        CHECK(help_command(w) == EXP_STATUS_SUCCESS);
        CHECK(help_terminal());
        CHECK(w[EXP_SSH_TERM_CLOSED] == 0);
        CHECK(w[EXP_SSH_TERM_LINE_COUNT] == (uint8_t)(count0 + 1));
        /* [ and ] as the ROM's glyphs, B0H and B1H */
        CHECK(memcmp(w + EXP_SSH_TERM_LINE, "SDLOAD \xB0M\xB1 \xB0name$\xB0,addr\xB1\xB1:", 26) == 0);
        w[EXP_SSH_TERM_KEY] = 0;
        help_poll(w, g_now += 50);
        w[EXP_SSH_TERM_KEY] = KBD_K_RIGHT;
        help_poll(w, g_now += 50);
        CHECK(w[EXP_SSH_TERM_LINE_COUNT] == (uint8_t)(count0 + 2));
        CHECK(memcmp(w + EXP_SSH_TERM_LINE, "DLOAD", 5) == 0);
        w[EXP_SSH_TERM_KEY] = 0;
        help_poll(w, g_now += 50);
        w[EXP_SSH_TERM_BREAK_COUNT]++; /* BREAK leaves */
        help_poll(w, g_now += 50);
        CHECK(!help_terminal());
        CHECK(w[EXP_SSH_TERM_CLOSED] != 0);
    }

    printf(g_failures ? "%d FAILED\n" : "help: all passed (%d)\n", g_failures ? g_failures : (int)n);
    return g_failures != 0;
}
