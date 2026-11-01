/* zterm.c -- see zterm.h. The wire format is pc_exp.h's EXP_COMMAND_ZTERM.
 *
 * Lines: the transcript is the game's text as printed, CR between lines.
 * A display line is up to ZT_WIDTH characters of it, broken at the last
 * space when a line runs longer (word wrap). The line shown is kept as
 * the position of its first character (zt->view); the one after it comes
 * from wrapping forward, the one before it from the start of its logical
 * line wrapped forward again, so nothing but the text itself is stored.
 *
 * The bottom line is the input line: the end of the text (the game's
 * prompt) and what the player has typed so far, with a cursor. */
#include "zterm.h"

#include <string.h>

#include "pc_exp.h"

#define CR 0x0D

void zterm_init(zterm_t *zt) {
    memset(zt, 0, sizeof *zt);
}

static uint8_t at(const zt_store_t *s, uint32_t pos) {
    return s->byte(s->ctx, pos);
}

/* The display line starting at pos: its length (*len) and where the next
 * one starts. */
static uint32_t wrap(const zt_store_t *s, uint32_t pos, uint8_t *len) {
    uint32_t end = s->end(s->ctx);
    uint8_t n = 0;
    while (pos + n < end && n < ZT_WIDTH && at(s, pos + n) != CR) n++;
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

/* The display line before the one at pos (pos itself at the top). */
static uint32_t line_before(const zt_store_t *s, uint32_t pos) {
    uint32_t begin = s->begin(s->ctx);
    uint32_t p, line, next;
    uint8_t len;
    if (pos <= begin) return begin;
    p = pos - 1; /* the start of the logical line holding pos - 1 */
    while (p > begin && at(s, p - 1) != CR) p--;
    line = p;
    while ((next = wrap(s, line, &len)) < pos && next > line) line = next;
    return line;
}

/* The bottom line's start: the last display line. */
static uint32_t last_line(const zt_store_t *s) {
    uint32_t line = s->begin(s->ctx), next;
    uint32_t end = s->end(s->ctx);
    uint8_t len;
    /* from the start of the last logical line */
    uint32_t p = end;
    while (p > line && at(s, p - 1) != CR) p--;
    line = p;
    while (line_at(s, line, next = wrap(s, line, &len))) line = next;
    return line;
}

static bool at_bottom(const zterm_t *zt, const zt_store_t *s) {
    uint8_t len;
    return !line_at(s, zt->view, wrap(s, zt->view, &len));
}

/* The line to show into out (ZT_WIDTH, space-padded). */
static void render(const zterm_t *zt, const zt_store_t *s, uint8_t *out) {
    uint8_t len;
    memset(out, ' ', ZT_WIDTH);
    wrap(s, zt->view, &len);
    if (!at_bottom(zt, s) || zt->input_max == 0) {
        for (uint8_t i = 0; i < len; i++) out[i] = at(s, zt->view + i);
        return;
    }
    /* the input line: the prompt, the typing, the cursor; its end if long */
    uint8_t line[ZT_WIDTH + ZT_INPUT_MAX + 1];
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

/* KEY: one key of the input line, or ZT_KEY_START to begin it. */
static uint8_t key(zterm_t *zt, const zt_store_t *s, uint8_t *w) {
    uint8_t k = w[1];
    uint8_t len;
    w[0] = 0;
    if (k == ZT_KEY_START) {
        zt->input_len = 0;
        zt->input_max = w[2] > ZT_INPUT_MAX ? ZT_INPUT_MAX : w[2];
        zt->view = zt->turn; /* this turn's first line */
        if (zt->view < s->begin(s->ctx)) zt->view = s->begin(s->ctx);
        if (zt->view >= s->end(s->ctx)) zt->view = last_line(s);
    } else if (k == ZT_KEY_UP) {
        zt->view = line_before(s, zt->view);
    } else if (k == ZT_KEY_DOWN) {
        uint32_t next = wrap(s, zt->view, &len);
        if (line_at(s, zt->view, next)) zt->view = next;
    } else if (k == ZT_KEY_ENTER) {
        if (zt->input_max == 0 || at_bottom(zt, s)) {
            /* the line is done: echo it into the transcript */
            uint8_t cr = CR;
            if (zt->input_max > 0) {
                append(s, zt->input, zt->input_len);
                append(s, &cr, 1);
            }
            zt->turn = s->end(s->ctx);
            w[0] = 1;
            w[27] = zt->input_len;
            memcpy(w + 28, zt->input, zt->input_len);
            zt->input_len = 0;
            zt->input_max = 0;
            return EXP_STATUS_SUCCESS;
        }
        zt->view = last_line(s); /* ENTER while reading: to the prompt */
    } else if (zt->input_max > 0) {
        if (k == ZT_KEY_BS) {
            if (zt->input_len > 0) zt->input_len--;
        } else if (k == ZT_KEY_CL) {
            zt->input_len = 0;
        } else if (k >= 0x20 && k < 0x7F) {
            if (zt->input_len < zt->input_max) zt->input[zt->input_len++] = k;
        }
        zt->view = last_line(s); /* typing goes to the input line */
    }
    render(zt, s, w + 1);
    return EXP_STATUS_SUCCESS;
}

uint8_t zterm_command(zterm_t *zt, const zt_store_t *s, uint8_t *w) {
    switch (w[0]) {
    case EXP_ZTERM_OPEN: {
        uint16_t seed = s->random16(s->ctx);
        zterm_init(zt);
        s->reset(s->ctx);
        zt->open = true;
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
    default:
        return EXP_STATUS_ERROR;
    }
}
