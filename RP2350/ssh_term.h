/* ssh_term.h -- the SSH shell's screen, on a 26-character display
 * (2026-10-07, ssh_client.h).
 *
 * The shell sees an 80-column terminal (SSH_COLS). Its output becomes lines
 * of up to 80 characters: the one being written (the live line) and a
 * scrollback ring of finished ones. The display shows 26 characters of one
 * line:
 * - live, the window follows the cursor;
 * - DEF+Up/Down (ssh_term_scroll) steps back through the scrollback, one
 *   line per step, and DEF+Left/Right (ssh_term_pan) moves the window
 *   across the line in 26-character steps -- the last step stops at the
 *   line's end, so it's always full. Typing returns to the live line.
 *
 * Output handling is a "dumb" terminal's: CR, LF, BS, TAB, and the few
 * escape sequences a line editor uses on one line (CSI K, C, D, G); every
 * other escape sequence is dropped. A UTF-8 character shows as '?'.
 * Portable C, no allocation. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ssh_client.h"

#define TERM_WIDTH 26    /* the PC-1500's display */
#define TERM_HISTORY 200 /* finished lines kept */
#define TERM_NO_CURSOR 0xFF

typedef struct {
    char text[SSH_COLS];
    uint8_t len;
} term_line_t;

typedef struct {
    term_line_t live;
    uint8_t cursor; /* 0..SSH_COLS: SSH_COLS = past the last column (wraps on the next character) */
    term_line_t history[TERM_HISTORY];
    uint16_t newest; /* history index of the newest finished line */
    uint16_t count;  /* finished lines kept */
    uint16_t back;   /* 0 = live; n = the n-th newest finished line */
    uint8_t left;    /* the window's first column */
    bool panned;     /* the live window was moved by hand: don't follow the cursor */
    uint8_t esc;     /* escape-sequence parser state */
    uint16_t esc_arg;
    uint8_t utf8_left; /* continuation bytes still to skip */
    uint32_t version;  /* changes whenever the display would */
} ssh_term_t;

void ssh_term_init(ssh_term_t *t);
void ssh_term_output(ssh_term_t *t, const uint8_t *data, size_t len);
/* Up (`lines` > 0, older) or down; down past the newest is the live line. */
void ssh_term_scroll(ssh_term_t *t, int lines);
/* Left (-1) or right (+1), a display width at a time. */
void ssh_term_pan(ssh_term_t *t, int dir);
/* Back to the live line, following the cursor (a key was typed). */
void ssh_term_live(ssh_term_t *t);
/* The 26 characters shown, space-padded; returns the cursor's place in
 * them, or TERM_NO_CURSOR (scrolled back, or panned away from it). */
uint8_t ssh_term_render(const ssh_term_t *t, char out[TERM_WIDTH]);
