/* help.h -- HELP (2026-11-10): a line of help per keyword, BASIC's own and
 * this module's, shown in TERM's 26-character window.
 *
 *   HELP name    exactly that keyword: its line
 *   HELP part    every keyword whose name holds `part` (any case), A-Z
 *   HELP         all of them
 *
 * Left/Right scroll the line (up to 80 characters), Up/Down step through
 * the matches; ENTER, CL, DEF or BREAK leave. The MCU owns the line shown,
 * as for the command history (cmd_history.h): help_begin() opens it,
 * help_poll() runs it between commands until TERM_CLOSED.
 *
 * Portable C, no allocation: the firmware (monitor.c) and pc1500emu's mock
 * both run it. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HELP_WIDTH 26    /* the display */
#define HELP_LINE_MAX 80 /* a help line */
#define HELP_QUERY_MAX 20

typedef struct {
    const char *name; /* as BASIC spells it */
    const char *text; /* its line, starting with its syntax */
} help_entry_t;

/* The table, sorted by name; for the host test. */
const help_entry_t *help_table(size_t *count);

/* Which entries `query` (`len` characters; spaces and quotes ignored)
 * matches: an exact name gives that one alone. Up to `max` indices into
 * help_table() go to `out`; the number matched is returned. */
size_t help_match(const char *query, size_t len, uint16_t *out, size_t max);

/* ---- the viewer ---- */

typedef enum { HELP_UI_OPEN, HELP_UI_DONE, HELP_UI_BREAK } help_result_t;

typedef struct {
    uint16_t match[256];
    uint16_t count, sel;
    uint8_t col;            /* the first character shown */
    help_result_t result;
    uint8_t held;           /* the matrix key at the last sample */
    uint32_t next_repeat_ms;
    bool repeats;
    uint32_t version;       /* changes whenever the display would */
} help_ui_t;

/* False if nothing matches. ENTER (which ran HELP) counts as still down. */
bool help_ui_start(help_ui_t *u, const char *query, size_t len);
/* One sample of the matrix key held (kbd_seq.h's KBD_K_*, 0 none). */
void help_ui_key(help_ui_t *u, uint8_t key, uint32_t now_ms);
void help_ui_break(help_ui_t *u);
/* The 26 characters to show, space-padded. */
void help_ui_render(const help_ui_t *u, char out[HELP_WIDTH]);

/* ---- in TERM's window (pc_exp.h EXP_SSH_TERM_*) ---- */

/* EXP_COMMAND_HELP: the query at w[0] (length) and w[1...]. Opens the
 * viewer and its first line; EXP_STATUS_SUCCESS, or EXP_STATUS_ERROR if
 * nothing matches. */
uint8_t help_command(uint8_t *w);
bool help_terminal(void);
/* Between commands while help_terminal(): the key, the line, and at the
 * end TERM_CLOSED. */
void help_poll(uint8_t *w, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
