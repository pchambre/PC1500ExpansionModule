/* ssh_term.c -- see ssh_term.h. */
#include "ssh_term.h"

#include <string.h>

enum { ESC_NONE, ESC_START, ESC_CSI, ESC_OSC, ESC_OSC_ESC, ESC_ONE };

static const term_line_t *shown_line(const ssh_term_t *t) {
    if (t->back == 0) return &t->live;
    return &t->history[(t->newest + TERM_HISTORY - (t->back - 1)) % TERM_HISTORY];
}

/* The live window, moved just enough to keep the cursor in it. */
static void follow(ssh_term_t *t) {
    uint8_t c = t->cursor < SSH_COLS ? t->cursor : SSH_COLS - 1;
    if (t->panned) return;
    if (c < t->left) t->left = c;
    else if (c >= t->left + TERM_WIDTH) t->left = (uint8_t)(c - TERM_WIDTH + 1);
}

static void changed(ssh_term_t *t) {
    if (t->back == 0) follow(t);
    t->version++;
}

/* The live line is finished: into the scrollback, and a new one begins. */
static void new_line(ssh_term_t *t) {
    t->newest = (uint16_t)((t->newest + 1) % TERM_HISTORY);
    t->history[t->newest] = t->live;
    if (t->count < TERM_HISTORY) t->count++;
    if (t->back && t->back < t->count) t->back++; /* stay on the line being read */
    t->live.len = 0;
    if (t->back == 0) {
        t->left = 0;
        t->panned = false;
    }
}

static void put_char(ssh_term_t *t, char c) {
    if (t->cursor >= SSH_COLS) { /* the last column was written: wrap, as a terminal does */
        new_line(t);
        t->cursor = 0;
    }
    while (t->live.len < t->cursor) t->live.text[t->live.len++] = ' ';
    t->live.text[t->cursor++] = c;
    if (t->live.len < t->cursor) t->live.len = t->cursor;
}

/* A finished CSI sequence (ESC [ ... final): the one-line ones. */
static void csi(ssh_term_t *t, uint8_t final) {
    uint16_t n = t->esc_arg;
    switch (final) {
        case 'K': /* erase in line: 0 to the end, 1 to the start, 2 all */
            if (n == 0 && t->cursor < t->live.len) t->live.len = t->cursor;
            else if (n == 1) memset(t->live.text, ' ', t->cursor < t->live.len ? t->cursor : t->live.len);
            else if (n == 2) t->live.len = 0;
            break;
        case 'C': /* right */
            n = n ? n : 1;
            t->cursor = (uint8_t)(t->cursor + n >= SSH_COLS ? SSH_COLS - 1 : t->cursor + n);
            break;
        case 'D': /* left */
            n = n ? n : 1;
            t->cursor = (uint8_t)(n > t->cursor ? 0 : t->cursor - n);
            break;
        case 'G': /* to column n (from 1) */
            n = n ? n - 1 : 0;
            t->cursor = (uint8_t)(n >= SSH_COLS ? SSH_COLS - 1 : n);
            break;
        default: break; /* colours, cursor moves to other lines...: not on one line */
    }
}

/* One byte inside an escape sequence; true once it's over. */
static void escape_byte(ssh_term_t *t, uint8_t b) {
    switch (t->esc) {
        case ESC_START:
            if (b == '[') {
                t->esc = ESC_CSI;
                t->esc_arg = 0;
            } else if (b == ']') {
                t->esc = ESC_OSC;
            } else if (b == '(' || b == ')' || b == '#' || b == '%') {
                t->esc = ESC_ONE; /* character set and the like: one more byte */
            } else {
                t->esc = ESC_NONE;
            }
            return;
        case ESC_CSI:
            if (b >= '0' && b <= '9') {
                if (t->esc_arg < 1000) t->esc_arg = (uint16_t)(t->esc_arg * 10 + (b - '0'));
            } else if (b == ';') {
                t->esc_arg = 0; /* only the last argument counts: none of ours take two */
            } else if (b >= 0x40 && b <= 0x7E) {
                csi(t, b);
                t->esc = ESC_NONE;
            } else if (b < 0x20 || b > 0x7E) {
                t->esc = ESC_NONE; /* not a sequence after all */
            }
            return;
        case ESC_OSC: /* a title and the like: up to BEL or ESC \ */
            if (b == 0x07) t->esc = ESC_NONE;
            else if (b == 0x1B) t->esc = ESC_OSC_ESC;
            return;
        case ESC_OSC_ESC: t->esc = b == '\\' ? ESC_NONE : ESC_OSC; return;
        default: t->esc = ESC_NONE; return;
    }
}

void ssh_term_init(ssh_term_t *t) { memset(t, 0, sizeof *t); }

void ssh_term_output(ssh_term_t *t, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];
        if (t->esc != ESC_NONE) {
            escape_byte(t, b);
            continue;
        }
        if (t->utf8_left && (b & 0xC0) == 0x80) {
            t->utf8_left--;
            continue;
        }
        t->utf8_left = 0;
        if (b >= 0x20 && b < 0x7F) {
            put_char(t, (char)b);
        } else if (b >= 0xC0 && b <= 0xF7) { /* a UTF-8 character: shown as '?' */
            put_char(t, '?');
            t->utf8_left = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : 1;
        } else {
            switch (b) {
                case '\r': t->cursor = 0; break;
                case '\n': {
                    uint8_t col = t->cursor < SSH_COLS ? t->cursor : 0;
                    new_line(t);
                    t->cursor = col; /* LF alone keeps the column; the pty sends CR LF */
                    break;
                }
                case '\b':
                    if (t->cursor) t->cursor = (uint8_t)(t->cursor >= SSH_COLS ? SSH_COLS - 1 : t->cursor - 1);
                    break;
                case '\t': {
                    uint8_t to = (uint8_t)((t->cursor / 8 + 1) * 8);
                    if (to > SSH_COLS - 1) to = SSH_COLS - 1;
                    while (t->live.len < to) t->live.text[t->live.len++] = ' ';
                    t->cursor = to;
                    break;
                }
                case 0x1B: t->esc = ESC_START; break;
                default: break; /* BEL and the other controls */
            }
        }
    }
    changed(t);
}

void ssh_term_scroll(ssh_term_t *t, int lines) {
    int back = (int)t->back + lines;
    if (back < 0) back = 0;
    if (back > t->count) back = t->count;
    if (back == t->back) return;
    t->back = (uint16_t)back;
    if (back == 0) {
        t->panned = false;
        t->left = 0;
        follow(t);
    } else {
        uint8_t len = shown_line(t)->len, last = len > TERM_WIDTH ? (uint8_t)(len - TERM_WIDTH) : 0;
        if (t->left > last) t->left = last; /* keep the column, as far as this line goes */
    }
    t->version++;
}

void ssh_term_pan(ssh_term_t *t, int dir) {
    uint8_t len = shown_line(t)->len, last = len > TERM_WIDTH ? (uint8_t)(len - TERM_WIDTH) : 0;
    uint8_t left = t->left;
    if (dir < 0) left = left > TERM_WIDTH ? (uint8_t)(left - TERM_WIDTH) : 0;
    else left = left + TERM_WIDTH > last ? last : (uint8_t)(left + TERM_WIDTH);
    if (left == t->left) return;
    t->left = left;
    if (t->back == 0) t->panned = true;
    t->version++;
}

void ssh_term_live(ssh_term_t *t) {
    if (t->back == 0 && !t->panned) return;
    t->back = 0;
    t->panned = false;
    t->left = 0;
    follow(t);
    t->version++;
}

uint8_t ssh_term_render(const ssh_term_t *t, char out[TERM_WIDTH]) {
    const term_line_t *line = shown_line(t);
    uint8_t cursor = TERM_NO_CURSOR;
    memset(out, ' ', TERM_WIDTH);
    if (t->left < line->len) {
        uint8_t n = (uint8_t)(line->len - t->left);
        memcpy(out, line->text + t->left, n > TERM_WIDTH ? TERM_WIDTH : n);
    }
    if (t->back == 0) {
        uint8_t c = t->cursor < SSH_COLS ? t->cursor : SSH_COLS - 1;
        if (c >= t->left && c < t->left + TERM_WIDTH) cursor = (uint8_t)(c - t->left);
    }
    return cursor;
}
