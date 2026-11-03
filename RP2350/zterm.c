/* zterm.c -- see zterm.h. The wire format is pc_exp.h's EXP_COMMAND_ZTERM.
 *
 * Lines: the transcript is the game's text as printed, CR between lines.
 * A display line is up to the line width's characters of it -- ZT_WIDTH
 * with word wrap on, ZT_LINE_MAX with it off -- broken at the last space
 * when a line runs longer. The line shown is kept as the position of its
 * first character (zt->view); the one after it comes from wrapping
 * forward, the one before it from the start of its logical line wrapped
 * forward again, so nothing but the text itself is stored, and a change of
 * width only has to find the view's line again.
 *
 * The bottom line is the input line: the end of the text (the game's
 * prompt) and what the player has typed so far, with a cursor.
 *
 * F1 shows the status lines, F2 the settings menu (zmachine/DESIGN.md).
 * FILE's prompt is an input line too: its name, edited, is kept for the
 * next one. PROMPT's (2026-11-02) is any program's: its own prompt and
 * starting text. At either, F1 shows the card's directory instead: up and
 * down go through it, L takes the entry's name as the line (as SDLOAD's
 * browser loads), F1 or CL goes back.
 *
 * The mirror (zterm_mirror_*, at the end) puts the same text on the host
 * app's Screen pane (screen.h). */
#include "zterm.h"

#include <stdio.h>
#include <string.h>

#include "pc_exp.h"

#define CR 0x0D

void zterm_init(zterm_t *zt) {
    memset(zt, 0, sizeof *zt);
}

void zterm_default_save_name(zterm_t *zt, const char *path) {
    const char *base = path ? path : "";
    for (const char *p = base; *p; p++)
        if (*p == '/' || *p == '\\' || *p == ':') base = p + 1;
    size_t n = strlen(base);
    const char *dot = strrchr(base, '.');
    if (dot) n = (size_t)(dot - base);
    if (n == 0) {
        base = "STORY";
        n = 5;
    }
    if (n > ZT_SAVENAME_MAX - 4) n = ZT_SAVENAME_MAX - 4;
    memcpy(zt->savename, base, n);
    memcpy(zt->savename + n, ".SAV", 4);
    zt->savename_len = (uint8_t)(n + 4);
}

static uint8_t at(const zt_store_t *s, uint32_t pos) {
    return s->byte(s->ctx, pos);
}

/* The display line starting at pos, w characters wide: its length (*len)
 * and where the next one starts. */
static uint32_t wrap(const zt_store_t *s, uint32_t pos, uint8_t w, uint8_t *len) {
    uint32_t end = s->end(s->ctx);
    uint8_t n = 0;
    while (pos + n < end && n < w && at(s, pos + n) != CR) n++;
    *len = n;
    if (pos + n >= end) return pos + n;           /* the text's last line */
    if (at(s, pos + n) == CR) return pos + n + 1; /* the line ended */
    if (at(s, pos + n) == ' ') return pos + n + 1;
    for (uint8_t k = n; k > 1; k--) /* break after the last space */
        if (at(s, pos + k - 1) == ' ') {
            *len = (uint8_t)(k - 1);
            return pos + k;
        }
    return pos + n; /* one long word: break it */
}

/* Is there a display line starting at next, after the line at pos? */
static bool line_at(const zt_store_t *s, uint32_t pos, uint32_t next) {
    uint32_t end = s->end(s->ctx);
    if (next < end) return true;
    return next == end && next > pos && at(s, next - 1) == CR;
}

/* The start of the logical line holding pos. */
static uint32_t logical_start(const zt_store_t *s, uint32_t pos) {
    uint32_t begin = s->begin(s->ctx);
    while (pos > begin && at(s, pos - 1) != CR) pos--;
    return pos;
}

/* The display line, w wide, that holds pos. */
static uint32_t line_holding(const zt_store_t *s, uint32_t pos, uint8_t w) {
    uint32_t line = logical_start(s, pos), next;
    uint8_t len;
    while ((next = wrap(s, line, w, &len)) <= pos && next > line && line_at(s, line, next)) line = next;
    return line;
}

/* The display line before the one at pos (pos itself at the top). */
static uint32_t line_before(const zt_store_t *s, uint32_t pos, uint8_t w) {
    if (pos <= s->begin(s->ctx)) return s->begin(s->ctx);
    return line_holding(s, pos - 1, w);
}

/* The bottom line's start: the last display line. */
static uint32_t last_line(const zt_store_t *s, uint8_t w) {
    uint32_t line = logical_start(s, s->end(s->ctx)), next;
    uint8_t len;
    while (line_at(s, line, next = wrap(s, line, w, &len))) line = next;
    return line;
}

static bool at_bottom(const zterm_t *zt, const zt_store_t *s) {
    uint8_t len;
    return !line_at(s, zt->view, wrap(s, zt->view, zt->width, &len));
}

/* The line width the setting asks for; a change finds the view's line
 * again at the new width. */
static void use_width(zterm_t *zt, const zt_store_t *s) {
    bool wrap_on = !s->setting || s->setting(s->ctx, ZT_SETTING_WRAP) != 0;
    uint8_t w = wrap_on ? ZT_WIDTH : ZT_LINE_MAX;
    if (zt->width == w) return;
    if (zt->width) zt->view = line_holding(s, zt->view, w);
    zt->width = w;
    zt->hscroll = 0;
}

/* The score and moves -- or the time, for a time game -- as the story's
 * status line has them. */
static void score_text(const zterm_t *zt, char *line, size_t size) {
    if (zt->status_flags & 0x02) {
        int h = zt->status_a % 24, m = zt->status_b;
        snprintf(line, size, "Time: %d:%02d %s", h % 12 == 0 ? 12 : h % 12, m, h < 12 ? "am" : "pm");
    } else {
        snprintf(line, size, "Score: %d  Moves: %d", zt->status_a, zt->status_b);
    }
}

/* The status lines (F1): the location, then the score text. */
static void render_status(const zterm_t *zt, uint8_t *out) {
    char line[ZT_WIDTH + 1];
    if (zt->screen == ZT_SCREEN_LOCATION) {
        uint8_t n = zt->location_len > ZT_WIDTH ? ZT_WIDTH : zt->location_len;
        memcpy(out, zt->location, n);
        return;
    }
    score_text(zt, line, sizeof line);
    memcpy(out, line, strlen(line));
}

/* The line to show into out (ZT_WIDTH, space-padded). */
static void render(const zterm_t *zt, const zt_store_t *s, uint8_t *out) {
    uint8_t len;
    memset(out, ' ', ZT_WIDTH);
    if (zt->screen == ZT_SCREEN_LOCATION || zt->screen == ZT_SCREEN_SCORE) {
        render_status(zt, out);
        return;
    }
    if (zt->browsing) {
        uint8_t n;
        s->dir_entry(s->ctx, zt->browse_at, out, &n);
        return;
    }
    if (zt->screen == ZT_SCREEN_MENU) {
        bool wrap_on = !s->setting || s->setting(s->ctx, ZT_SETTING_WRAP) != 0;
        const char *item = wrap_on ? "Word wrap: on" : "Word wrap: off";
        memcpy(out, item, strlen(item));
        return;
    }
    wrap(s, zt->view, zt->width, &len);
    if (!at_bottom(zt, s) || zt->input_max == 0) {
        uint8_t from = zt->hscroll;
        if (len <= ZT_WIDTH) from = 0;
        else if (from > len - ZT_WIDTH) from = (uint8_t)(len - ZT_WIDTH);
        for (uint8_t i = 0; i < len - from && i < ZT_WIDTH; i++) out[i] = at(s, zt->view + from + i);
        return;
    }
    /* the input line: the prompt, the typing, the cursor; its end if long */
    uint8_t line[ZT_LINE_MAX + ZT_INPUT_MAX + 1];
    uint16_t n = 0;
    for (uint8_t i = 0; i < len; i++) line[n++] = at(s, zt->view + i);
    memcpy(line + n, zt->input, zt->input_len);
    n += zt->input_len;
    line[n++] = '_';
    uint16_t from = n > ZT_WIDTH ? (uint16_t)(n - ZT_WIDTH) : 0;
    memcpy(out, line + from, n - from);
}

static void append(const zt_store_t *s, const uint8_t *data, uint16_t len) {
    s->append(s->ctx, data, len);
}

/* The settings menu (F2): one item so far, word wrap. ENTER, < or >
 * changes it; F2 or CL goes back to the text. */
static void menu_key(zterm_t *zt, const zt_store_t *s, uint8_t k) {
    if (k == ZT_KEY_ENTER || k == ZT_KEY_BS || k == ZT_KEY_RIGHT) {
        bool wrap_on = !s->setting || s->setting(s->ctx, ZT_SETTING_WRAP) != 0;
        if (s->set_setting) s->set_setting(s->ctx, ZT_SETTING_WRAP, wrap_on ? 0 : 1);
        use_width(zt, s);
    } else if (k == ZT_KEY_F2 || k == ZT_KEY_CL) {
        zt->screen = ZT_SCREEN_TEXT;
    }
}

/* The input line is done: echoed into the transcript, and the answer. */
static uint8_t finish_line(zterm_t *zt, const zt_store_t *s, uint8_t *w) {
    uint8_t cr = CR;
    if (zt->input_max > 0) {
        append(s, zt->input, zt->input_len);
        append(s, &cr, 1);
    }
    if (zt->prompt == ZT_PROMPT_FILE && zt->input_len > 0) { /* FILE's name: kept for the next */
        uint8_t n = zt->input_len > ZT_SAVENAME_MAX ? ZT_SAVENAME_MAX : zt->input_len;
        memcpy(zt->savename, zt->input, n);
        zt->savename_len = n;
    }
    if (zt->prompt == ZT_PROMPT_NONE) zt->turn = s->end(s->ctx); /* a prompt's: the turn goes on */
    zt->prompt = ZT_PROMPT_NONE;
    zt->browse_ok = false;
    w[0] = 1;
    w[27] = zt->input_len;
    memcpy(w + 28, zt->input, zt->input_len);
    zt->input_len = 0;
    zt->input_max = 0;
    return EXP_STATUS_SUCCESS;
}

/* A key while the directory is shown. True if it picked an entry: L, as
 * SDLOAD's browser loads (ENTER does nothing there either). */
static bool browse_key(zterm_t *zt, const zt_store_t *s, uint8_t k) {
    if (k == ZT_KEY_UP) {
        if (zt->browse_at > 0) zt->browse_at--;
    } else if (k == ZT_KEY_DOWN) {
        if (zt->browse_at + 1 < zt->browse_count) zt->browse_at++;
    } else if (k == 'L' || k == 'l') {
        uint8_t line[ZT_WIDTH], n;
        s->dir_entry(s->ctx, zt->browse_at, line, &n);
        if (n > zt->input_max) n = zt->input_max;
        memcpy(zt->input, line, n);
        zt->input_len = n;
        zt->browsing = false;
        return true;
    } else if (k == ZT_KEY_F1 || k == ZT_KEY_CL) {
        zt->browsing = false;
    }
    return false;
}

/* KEY: one key of the input line, or ZT_KEY_START to begin it. */
static uint8_t key(zterm_t *zt, const zt_store_t *s, uint8_t *w) {
    uint8_t k = w[1];
    uint8_t len;
    w[0] = 0;
    use_width(zt, s);
    if (zt->browsing && k != ZT_KEY_START) {
        if (browse_key(zt, s, k)) return finish_line(zt, s, w);
    } else if (k == ZT_KEY_F1 && zt->browse_ok && s->dir_count) {
        zt->browse_count = s->dir_count(s->ctx);
        zt->browse_at = 0;
        zt->browsing = zt->browse_count > 0;
    } else if (k == ZT_KEY_START) {
        zt->input_len = 0;
        zt->input_max = w[2] > ZT_INPUT_MAX ? ZT_INPUT_MAX : w[2];
        zt->screen = ZT_SCREEN_TEXT;
        zt->hscroll = 0;
        zt->view = zt->turn; /* this turn's first line */
        if (zt->view < s->begin(s->ctx)) zt->view = s->begin(s->ctx);
        if (zt->view >= s->end(s->ctx)) zt->view = last_line(s, zt->width);
    } else if (zt->screen == ZT_SCREEN_MENU) {
        menu_key(zt, s, k);
    } else if (k == ZT_KEY_F1) { /* the text, the location, the score, the text */
        zt->screen = zt->screen == ZT_SCREEN_LOCATION ? ZT_SCREEN_SCORE
                   : zt->screen == ZT_SCREEN_SCORE    ? ZT_SCREEN_TEXT
                                                      : ZT_SCREEN_LOCATION;
    } else if (k == ZT_KEY_F2) {
        zt->screen = ZT_SCREEN_MENU;
    } else if (zt->screen != ZT_SCREEN_TEXT) {
        zt->screen = ZT_SCREEN_TEXT; /* any other key: back to the text, and that's all it does */
    } else if (k == ZT_KEY_UP) {
        zt->view = line_before(s, zt->view, zt->width);
    } else if (k == ZT_KEY_DOWN) {
        uint32_t next = wrap(s, zt->view, zt->width, &len);
        if (line_at(s, zt->view, next)) zt->view = next;
    } else if ((k == ZT_KEY_BS || k == ZT_KEY_RIGHT) && zt->width > ZT_WIDTH && !at_bottom(zt, s)) {
        /* wrap off, reading: < and > move a long line sideways */
        wrap(s, zt->view, zt->width, &len);
        uint8_t most = len > ZT_WIDTH ? (uint8_t)(len - ZT_WIDTH) : 0;
        if (zt->hscroll > most) zt->hscroll = most;
        if (k == ZT_KEY_RIGHT) zt->hscroll = zt->hscroll + ZT_HSTEP > most ? most : (uint8_t)(zt->hscroll + ZT_HSTEP);
        else zt->hscroll = zt->hscroll > ZT_HSTEP ? (uint8_t)(zt->hscroll - ZT_HSTEP) : 0;
    } else if (k == ZT_KEY_RIGHT) {
        /* nothing to move to */
    } else if (k == ZT_KEY_ENTER) {
        if (zt->input_max == 0 || at_bottom(zt, s)) return finish_line(zt, s, w);
        zt->view = last_line(s, zt->width); /* ENTER while reading: to the prompt */
    } else if (zt->input_max > 0) {
        if (k == ZT_KEY_BS) {
            if (zt->input_len > 0) zt->input_len--;
        } else if (k == ZT_KEY_CL) {
            zt->input_len = 0;
        } else if (k >= 0x20 && k < 0x7F) {
            if (zt->input_len < zt->input_max) zt->input[zt->input_len++] = k;
        }
        zt->view = last_line(s, zt->width); /* typing goes to the input line */
    }
    render(zt, s, w + 1);
    return EXP_STATUS_SUCCESS;
}

uint8_t zterm_command(zterm_t *zt, const zt_store_t *s, uint8_t *w) {
    switch (w[0]) {
    case EXP_ZTERM_OPEN: {
        uint16_t seed = s->random16(s->ctx);
        uint16_t game = zt->game;
        zterm_init(zt);
        s->reset(s->ctx);
        zt->open = true;
        zt->game = (uint16_t)(game + 1);
        zt->story_channel = w[1];
        zt->flags = w[2];
        w[0] = (uint8_t)(seed >> 8);
        w[1] = (uint8_t)seed;
        return EXP_STATUS_SUCCESS;
    }
    case EXP_ZTERM_TEXT:
        if (!zt->open) return EXP_STATUS_ERROR;
        append(s, w + 2, w[1]);
        return EXP_STATUS_SUCCESS;
    case EXP_ZTERM_STATUS:
        if (!zt->open) return EXP_STATUS_ERROR;
        zt->status_a = (int16_t)((w[1] << 8) | w[2]);
        zt->status_b = (int16_t)((w[3] << 8) | w[4]);
        zt->status_flags = w[5];
        zt->location_len = w[6] > sizeof zt->location ? sizeof zt->location : w[6];
        memcpy(zt->location, w + 7, zt->location_len);
        return EXP_STATUS_SUCCESS;
    case EXP_ZTERM_KEY:
        if (!zt->open) return EXP_STATUS_ERROR;
        return key(zt, s, w);
    case EXP_ZTERM_CLOSE:
        zt->open = false;
        return EXP_STATUS_SUCCESS;
    case EXP_ZTERM_FILE: {
        if (!zt->open) return EXP_STATUS_ERROR;
        const char *prompt = w[1] ? "Restore from: " : "Save as: ";
        append(s, (const uint8_t *)prompt, (uint16_t)strlen(prompt));
        use_width(zt, s);
        memcpy(zt->input, zt->savename, zt->savename_len);
        zt->input_len = zt->savename_len;
        zt->input_max = ZT_SAVENAME_MAX;
        zt->prompt = ZT_PROMPT_FILE;
        zt->browse_ok = true;
        zt->browsing = false;
        zt->screen = ZT_SCREEN_TEXT;
        zt->hscroll = 0;
        zt->view = last_line(s, zt->width);
        w[0] = 0;
        render(zt, s, w + 1);
        return EXP_STATUS_SUCCESS;
    }
    case EXP_ZTERM_PROMPT: { /* [flags][max][prompt len][prompt][text len][text] */
        if (!zt->open) return EXP_STATUS_ERROR;
        uint8_t plen = w[3];
        const uint8_t *text = w + 4 + plen;
        uint8_t max = w[2] > ZT_INPUT_MAX ? ZT_INPUT_MAX : w[2];
        uint8_t tlen = text[0] > max ? max : text[0];
        bool browse = (w[1] & EXP_ZTERM_PROMPT_BROWSE) != 0;
        append(s, w + 4, plen);
        use_width(zt, s);
        memmove(zt->input, text + 1, tlen);
        zt->input_len = tlen;
        zt->input_max = max;
        zt->prompt = ZT_PROMPT_LINE;
        zt->browse_ok = browse;
        zt->browsing = false;
        zt->screen = ZT_SCREEN_TEXT;
        zt->hscroll = 0;
        zt->view = last_line(s, zt->width);
        w[0] = 0;
        render(zt, s, w + 1);
        return EXP_STATUS_SUCCESS;
    }
    default:
        return EXP_STATUS_ERROR;
    }
}

/* ---- The mirror (zterm.h) ---- */

/* BLE_PROTOCOL.md's DISPLAY record kinds */
#define REC_OUTPUT 2
#define REC_EDIT 3
#define REC_CLEAR 4
#define REC_GAME 6

void zterm_mirror_restart(zt_mirror_t *m) {
    memset(m, 0, sizeof *m);
}

/* A record onto the payload, if it fits. */
static bool add_record(uint8_t *p, uint16_t *n, uint16_t max, uint8_t kind, const uint8_t *bytes, uint8_t len) {
    if (*n + 2u + len > max) return false;
    p[*n] = kind;
    p[*n + 1] = len;
    if (len) memcpy(p + *n + 2, bytes, len);
    *n = (uint16_t)(*n + 2 + len);
    return true;
}

/* Where starting over starts: the last ZT_MIRROR_CATCHUP bytes, from the
 * first line that starts in them. */
static uint32_t catchup_start(const zt_store_t *s) {
    uint32_t begin = s->begin(s->ctx), end = s->end(s->ctx);
    if (end - begin <= ZT_MIRROR_CATCHUP) return begin;
    uint32_t pos = end - ZT_MIRROR_CATCHUP;
    while (pos < end && at(s, pos - 1) != CR) pos++;
    return pos;
}

/* EDIT's bytes: the cursor's column, then the text's unfinished last line
 * and, at an input line, what's typed so far with the cursor after it --
 * its end, if longer than ZT_MIRROR_EDIT_MAX. */
static uint8_t edit_bytes(const zterm_t *zt, const zt_store_t *s, uint8_t *out) {
    uint8_t text[ZT_MIRROR_EDIT_MAX + ZT_INPUT_MAX];
    uint32_t end = s->end(s->ctx), from = logical_start(s, end);
    if (end - from > ZT_MIRROR_EDIT_MAX) from = end - ZT_MIRROR_EDIT_MAX;
    uint16_t n = 0;
    while (from < end) text[n++] = at(s, from++);
    bool typing = zt->input_max > 0;
    if (typing) {
        memcpy(text + n, zt->input, zt->input_len);
        n = (uint16_t)(n + zt->input_len);
    }
    uint16_t skip = n > ZT_MIRROR_EDIT_MAX - 1 ? (uint16_t)(n - (ZT_MIRROR_EDIT_MAX - 1)) : 0; /* room for the cursor */
    n = (uint16_t)(n - skip);
    out[0] = typing ? (uint8_t)n : 0xFF; /* -1: no cursor */
    memcpy(out + 1, text + skip, n);
    return (uint8_t)(1 + n);
}

/* GAME's bytes: the location (str8), then the score text; none before the
 * story's first status line. */
static uint8_t game_bytes(const zterm_t *zt, uint8_t *out) {
    char score[ZT_MIRROR_GAME_MAX];
    if (!zt->location_len) return 0;
    uint8_t loc = zt->location_len;
    score_text(zt, score, sizeof score - 1 - loc);
    size_t n = strlen(score);
    out[0] = loc;
    memcpy(out + 1, zt->location, loc);
    memcpy(out + 1 + loc, score, n);
    return (uint8_t)(1 + loc + n);
}

uint16_t zterm_mirror_next(const zt_mirror_t *m, zt_mirror_t *q, const zterm_t *zt, const zt_store_t *s,
                           uint8_t *p, uint16_t max) {
    uint16_t n = 0;
    *q = *m;
    if (zt->open && (!q->synced || q->game != zt->game)) { /* a new game, or starting over */
        if (!add_record(p, &n, max, REC_CLEAR, NULL, 0)) return 0;
        zterm_mirror_restart(q);
        q->synced = true;
        q->game = zt->game;
        q->sent = catchup_start(s);
    }
    if (!q->synced) return 0;
    uint32_t end = s->end(s->ctx);
    if (q->sent < s->begin(s->ctx)) q->sent = s->begin(s->ctx); /* fell behind the ring */
    if (q->sent > end) q->sent = catchup_start(s);
    /* The finished lines -- once the game's closed, its last one too */
    uint32_t done = zt->open ? logical_start(s, end) : end;
    while (q->sent < done) {
        uint8_t line[ZT_LINE_MAX], len;
        uint32_t next = wrap(s, q->sent, ZT_LINE_MAX, &len);
        for (uint8_t i = 0; i < len; i++) line[i] = at(s, q->sent + i);
        if (!add_record(p, &n, max, REC_OUTPUT, line, len)) break;
        q->sent = next;
        q->edit_len = 0; /* an OUTPUT ends the EDIT */
    }
    if (zt->open && q->sent >= done) { /* the line being typed, once the lines before it went */
        uint8_t e[1 + ZT_MIRROR_EDIT_MAX];
        uint8_t el = edit_bytes(zt, s, e);
        if ((el != q->edit_len || memcmp(e, q->edit, el)) && add_record(p, &n, max, REC_EDIT, e, el)) {
            memcpy(q->edit, e, el);
            q->edit_len = el;
        }
    }
    uint8_t g[ZT_MIRROR_GAME_MAX];
    uint8_t gl = zt->open ? game_bytes(zt, g) : 0; /* none: the pane hides the status lines */
    if ((!q->status_valid || gl != q->status_len || memcmp(g, q->status, gl)) &&
        add_record(p, &n, max, REC_GAME, g, gl)) {
        q->status_valid = true;
        memcpy(q->status, g, gl);
        q->status_len = gl;
    }
    if (!zt->open && q->sent >= done && q->status_valid && !q->status_len)
        q->synced = false; /* the closed game is all there: nothing more until the next */
    return n;
}

void zterm_mirror_sent(zt_mirror_t *m, const zt_mirror_t *pending) {
    *m = *pending;
}
