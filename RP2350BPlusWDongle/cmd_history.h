/* cmd_history.h -- BASIC's RUN-mode command history, bash-like
 * (2026-10-07, MCONF HISTORY).
 *
 * The keyboard driver (rom.asm KBD_DISPATCH) hands the MCU every command
 * line ENTERed at BASIC's prompt; DEF+Up/Down (browse) or DEF+Left (search)
 * then shows them a line at a time, as the SSH terminal does: the MCU owns
 * the line shown (TERM's window bytes), the ROM reports the matrix key held.
 *
 *   browse   Up older, Down newer
 *   search   letters, digits and symbols narrow it to the newest command
 *            holding them (any case); DEF+Left the next older match; SHIFT+
 *            Left deletes a character; Up/Down step through the matches
 *   either   ENTER runs the command shown; Left/Right put it on the line to
 *            edit; DEF, CL or BREAK leave it, the line as it was
 *
 * This file: the history (a ring, newest first) and that UI. Portable C, no
 * allocation: the firmware (history_session.c) and pc1500emu's mock both
 * run it. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HIST_LINE_MAX 80 /* BASIC's input line, 7BB0H-7BFFH */
#define HIST_ENTRIES 128
#define HIST_WIDTH 26    /* the display */
#define HIST_QUERY_MAX 20

typedef struct {
    char text[HIST_LINE_MAX];
    uint8_t len;
} hist_entry_t;

typedef struct {
    hist_entry_t e[HIST_ENTRIES];
    uint16_t newest; /* index of the newest entry */
    uint16_t count;
} hist_t;

void hist_init(hist_t *h);
/* A command: added as the newest, unless it's empty or the same as the
 * newest already. True if added. */
bool hist_add(hist_t *h, const char *line, size_t len);
/* The `back`-th newest (1 = the newest), or NULL. */
const hist_entry_t *hist_get(const hist_t *h, uint16_t back);

/* ---- browsing and searching ---- */

typedef enum {
    HIST_UI_OPEN,   /* still going */
    HIST_UI_CANCEL, /* DEF, CL or BREAK: the line as it was */
    HIST_UI_EDIT,   /* Left/Right: the command on the line, to edit */
    HIST_UI_RUN,    /* ENTER: the command run */
    HIST_UI_BREAK,  /* BREAK: as CANCEL, and then BREAK as usual */
} hist_result_t;

typedef enum { HIST_START_OLDER, HIST_START_NEWER, HIST_START_SEARCH } hist_start_t;

typedef struct {
    const hist_t *h;
    bool search;
    uint16_t back; /* the entry shown (1 = newest), 0 = none (search: no match) */
    char query[HIST_QUERY_MAX];
    uint8_t qlen;
    hist_result_t result;
    bool shift, def;        /* one-shot, as ROM1's */
    uint8_t held;           /* the matrix key at the last sample */
    uint32_t next_repeat_ms;
    bool repeats;
    uint32_t version; /* changes whenever the display would */
} hist_ui_t;

/* `how`: DEF+Up (older), DEF+Down (newer: the newest) or DEF+Left (search).
 * False if there's no history to show (the key then does what it always did). */
bool hist_ui_start(hist_ui_t *u, const hist_t *h, hist_start_t how);
/* One sample of the matrix key held (kbd_seq.h's KBD_K_*, 0 none) at `now_ms`. */
void hist_ui_key(hist_ui_t *u, uint8_t key, uint32_t now_ms);
void hist_ui_break(hist_ui_t *u);
/* The 26 characters to show, space-padded. */
void hist_ui_render(const hist_ui_t *u, char out[HIST_WIDTH]);
/* The command the result is about (EDIT/RUN), or NULL. */
const hist_entry_t *hist_ui_selected(const hist_ui_t *u);
static inline hist_result_t hist_ui_result(const hist_ui_t *u) { return u->result; }
/* The LCD's indicators for it, as 764EH's bits: DEF (lit throughout: DEF
 * leaves), SHIFT waiting, SMALL. */
uint8_t hist_ui_indicators(const hist_ui_t *u);
