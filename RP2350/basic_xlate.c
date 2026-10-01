/* basic_xlate.c -- see basic_xlate.h. */
#include "basic_xlate.h"

#include <string.h>

enum { S_NUMBER_HI, S_NUMBER_LO, S_LENGTH, S_BODY, S_END };

static struct {
    uint8_t mode;
    uint8_t state;
    uint8_t left;   /* bytes of the line's body still to come */
    bool quoted;
    bool held;      /* a token's first byte, waiting for its second */
    uint8_t hold;
} X;

void basic_xlate_begin(uint8_t mode) {
    memset(&X, 0, sizeof X);
    X.mode = mode;
}

void basic_xlate_end(void) { X.mode = BASIC_XLATE_OFF; }

uint8_t basic_xlate_mode(void) { return X.mode; }

static void map(uint8_t *hi, uint8_t *lo) {
    if (X.mode == BASIC_XLATE_SAVE && *hi == 0xE1 && *lo >= 0xC0 && *lo <= 0xC6) {
        *hi = 0xE6;
        *lo = (uint8_t)(*lo - 0x40);
    } else if (X.mode == BASIC_XLATE_LOAD && *hi == 0xE6 && *lo >= 0x80 && *lo <= 0x86) {
        *hi = 0xE1;
        *lo = (uint8_t)(*lo + 0x40);
    }
}

/* Translates d[0..n), following the program; returns how many bytes are
 * settled: n, or (with `hold`) n - 1 when the last is a token's first byte,
 * which is then left for the next chunk. */
static uint16_t process(uint8_t *d, uint16_t n, bool hold) {
    uint16_t i = 0;
    while (i < n) {
        const uint8_t b = d[i];
        switch (X.state) {
            case S_NUMBER_HI: X.state = b == 0xFF ? S_END : S_NUMBER_LO; break;
            case S_NUMBER_LO: X.state = S_LENGTH; break;
            case S_LENGTH:
                X.left = b;
                X.quoted = false;
                X.state = b ? S_BODY : S_NUMBER_HI;
                break;
            case S_BODY:
                if (!X.quoted && b >= 0xE0 && X.left >= 2) {
                    if (i + 1 == n) {
                        if (hold) return i;
                    } else {
                        map(&d[i], &d[i + 1]);
                        i++;
                        X.left--;
                    }
                } else if (b == '"') {
                    X.quoted = !X.quoted;
                }
                if (--X.left == 0) X.state = S_NUMBER_HI;
                break;
            default: break; /* past the end: nothing more to change */
        }
        i++;
    }
    return n;
}

/* The byte held back from the last chunk goes first in this one. */
static uint16_t unhold(uint8_t *d, uint16_t n) {
    if (!X.held) return n;
    memmove(d + 1, d, n);
    d[0] = X.hold;
    X.held = false;
    return (uint16_t)(n + 1);
}

static uint16_t settle(uint8_t *d, uint16_t n, bool hold) {
    uint16_t done = process(d, n, hold);
    if (done < n) {
        X.hold = d[done];
        X.held = true;
    }
    return done;
}

uint16_t basic_xlate_write(uint8_t *data, uint16_t len) {
    if (X.mode != BASIC_XLATE_SAVE) return len;
    len = unhold(data, len);
    return settle(data, len, true);
}

uint16_t basic_xlate_flush(uint8_t *data) {
    if (X.mode != BASIC_XLATE_SAVE || !X.held) return 0;
    data[0] = X.hold;
    X.held = false;
    return 1;
}

uint16_t basic_xlate_read(uint8_t *data, uint16_t len) {
    if (X.mode != BASIC_XLATE_LOAD) return len;
    len = unhold(data, len);
    /* never hold back a chunk's only byte: a read of 0 ends the load */
    return settle(data, len, len > 1);
}
