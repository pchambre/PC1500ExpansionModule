/* ssh_keys.h -- the PC-1500's keyboard as an SSH terminal's (2026-10-07,
 * ssh_term.h).
 *
 * The ROM's terminal loop reports the matrix key held down at each timer
 * wake (kbd_seq.h's KBD_K_*, 0 for none); this turns those samples into
 * the bytes a terminal sends, with SHIFT, DEF and SML handled here rather
 * than by the ROM, and auto-repeat. The keys:
 *
 *   letters          lower case; SML toggles upper case; SHIFT+letter the other case
 *   DEF+letter       Ctrl+letter (DEF+C = Ctrl-C, DEF+D = Ctrl-D...)
 *   the keyboard's own symbols, plain and with SHIFT, as printed:
 *                    SHIFT+F1..F6 = ! " # $ % &   SHIFT+= @   SHIFT+SPACE ^
 *                    SHIFT+/ ?   SHIFT+* :   SHIFT+( <   SHIFT+) >   SHIFT++ ;   SHIFT+- ,
 *   F1..F6           TAB  '  _  |  ~  ESC  (characters the keyboard lacks)
 *   DEF+( DEF+)      [ ]      DEF+SHIFT+( DEF+SHIFT+)  { }
 *   DEF+/ DEF+*      \ `
 *   arrows           the cursor keys (ESC [ A..D)
 *   SHIFT+Left       backspace (DEL), as the PC-1500's DEL
 *   CL               Ctrl-U (erase the line)
 *   RCL              Ctrl-R (search the history)
 *   ENTER            CR
 *   DEF+arrow        scrollback: that arrow's move, and then the plain arrows go on
 *                    moving -- Up/Down through the scrollback, Left/Right across the
 *                    line -- until DEF or CL ends it (back to the live line); any
 *                    other key ends it too, and goes to the shell as usual
 *   DEF+CL           end the session from this side
 * BREAK (ON) is the ROM's own: Ctrl-C.
 *
 * SHIFT and DEF are one-shot, as on the PC-1500: pressed (and let go)
 * before the key they apply to; pressing one again cancels it. Portable C. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SSH_KEYS_OUT_MAX 4
#define SSH_KEYS_REPEAT_DELAY_MS 500
#define SSH_KEYS_REPEAT_MS 80

typedef enum {
    SSH_VIEW_NONE,
    SSH_VIEW_UP,
    SSH_VIEW_DOWN,
    SSH_VIEW_LEFT,
    SSH_VIEW_RIGHT,
    SSH_VIEW_QUIT, /* DEF+CL */
    SSH_VIEW_LIVE, /* scrollback ended: back to the live line */
} ssh_view_t;

typedef struct {
    uint8_t held; /* the key down at the last sample */
    uint32_t next_repeat_ms;
    bool repeats; /* the held key repeats once next_repeat_ms comes */
    uint8_t out[SSH_KEYS_OUT_MAX], out_len; /* what the held key sent: a repeat sends it again */
    ssh_view_t view;                        /* ...or did to the display */
    bool shift, def, caps;
    bool scrolling; /* scrollback: the arrows move the display */
} ssh_keys_t;

void ssh_keys_init(ssh_keys_t *k);

/* One sample: `key` held at `now_ms`. Bytes to send go to `out` (returns
 * how many); a display action goes to *view. */
size_t ssh_keys_sample(ssh_keys_t *k, uint8_t key, uint32_t now_ms, uint8_t out[SSH_KEYS_OUT_MAX], ssh_view_t *view);

/* For the display's corner: the one-shot modifiers waiting, and SML. */
static inline bool ssh_keys_shift(const ssh_keys_t *k) { return k->shift; }
static inline bool ssh_keys_def(const ssh_keys_t *k) { return k->def; }
static inline bool ssh_keys_caps(const ssh_keys_t *k) { return k->caps; }
static inline bool ssh_keys_scrolling(const ssh_keys_t *k) { return k->scrolling; }
