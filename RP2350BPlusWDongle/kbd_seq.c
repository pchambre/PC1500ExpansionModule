/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* kbd_seq.c -- see kbd_seq.h. */
#include "kbd_seq.h"

#include <string.h>

void kbd_seq_init(kbd_seq_t *s) { memset(s, 0, sizeof *s); }

uint8_t kbd_seq_layout_for_country(uint16_t country) {
    switch (country) {
        case KBD_COUNTRY_FRENCH: return KBD_LAYOUT_FR;
        case KBD_COUNTRY_GERMAN: return KBD_LAYOUT_DE;
        case KBD_COUNTRY_SPANISH: return KBD_LAYOUT_ES;
        case KBD_COUNTRY_BELGIAN: return KBD_LAYOUT_BE;
        default: return KBD_LAYOUT_US;
    }
}

bool kbd_seq_country_supported(uint16_t country) {
    return country == KBD_COUNTRY_NONE || country == KBD_COUNTRY_US ||
           kbd_seq_layout_for_country(country) != KBD_LAYOUT_US;
}

static uint16_t queue_free(const kbd_seq_t *s) {
    return (uint16_t)(KBD_QUEUE_LEN - 1 - ((s->tail - s->head + KBD_QUEUE_LEN) % KBD_QUEUE_LEN));
}

static void push(kbd_seq_t *s, uint8_t key, bool pressed, uint16_t wait_ms) {
    s->queue[s->tail] = (kbd_action_t){key, pressed, wait_ms};
    s->tail = (uint16_t)((s->tail + 1) % KBD_QUEUE_LEN);
}

bool kbd_seq_tap(kbd_seq_t *s, uint8_t key, bool shifted) {
    if (queue_free(s) < (shifted ? 4 : 2)) return false;
    if (shifted) {
        push(s, KBD_K_SHIFT, true, KBD_TAP_MS);
        push(s, KBD_K_SHIFT, false, KBD_IDLE_MS);
    }
    push(s, key, true, KBD_TAP_MS);
    push(s, key, false, KBD_IDLE_MS);
    return true;
}

/* Letters by position: A..Z. */
static const uint8_t kLetters[26] = {
    KBD_K_A, KBD_K_B, KBD_K_C, KBD_K_D, KBD_K_E, KBD_K_F, KBD_K_G, KBD_K_H, KBD_K_I,
    KBD_K_J, KBD_K_K, KBD_K_L, KBD_K_M, KBD_K_N, KBD_K_O, KBD_K_P, KBD_K_Q, KBD_K_R,
    KBD_K_S, KBD_K_T, KBD_K_U, KBD_K_V, KBD_K_W, KBD_K_X, KBD_K_Y, KBD_K_Z};
static const uint8_t kDigits[10] = {KBD_K_0, KBD_K_1, KBD_K_2, KBD_K_3, KBD_K_4,
                                    KBD_K_5, KBD_K_6, KBD_K_7, KBD_K_8, KBD_K_9};

/* Unshifted punctuation keys, then the symbols that are Shift + a key --
 * the ROM's FEC0H table gives the same pairs. */
static const struct {
    char c;
    uint8_t key;
} kDirect[] = {{' ', KBD_K_SPACE}, {'.', KBD_K_PERIOD},   {'/', KBD_K_SLASH},  {'+', KBD_K_PLUS},
               {'-', KBD_K_MINUS}, {'=', KBD_K_EQUALS},   {'(', KBD_K_LPAREN}, {')', KBD_K_RPAREN},
               {'*', KBD_K_ASTERISK}, {'\r', KBD_K_ENTER}},
  kShifted[] = {{'!', KBD_K_F1},     {'"', KBD_K_F2},       {'#', KBD_K_F3},     {'$', KBD_K_F4},
                {'%', KBD_K_F5},     {'&', KBD_K_F6},       {'@', KBD_K_EQUALS}, {'^', KBD_K_SPACE},
                {'?', KBD_K_SLASH},  {':', KBD_K_ASTERISK}, {'<', KBD_K_LPAREN}, {'>', KBD_K_RPAREN},
                {';', KBD_K_PLUS},   {',', KBD_K_MINUS}};

bool kbd_seq_char(kbd_seq_t *s, char c) {
    if (c >= 'a' && c <= 'z') return kbd_seq_tap(s, kLetters[c - 'a'], false);
    if (c >= 'A' && c <= 'Z') return kbd_seq_tap(s, kLetters[c - 'A'], false);
    if (c >= '0' && c <= '9') return kbd_seq_tap(s, kDigits[c - '0'], false);
    for (unsigned i = 0; i < sizeof kDirect / sizeof kDirect[0]; i++)
        if (kDirect[i].c == c) return kbd_seq_tap(s, kDirect[i].key, false);
    for (unsigned i = 0; i < sizeof kShifted / sizeof kShifted[0]; i++)
        if (kShifted[i].c == c) return kbd_seq_tap(s, kShifted[i].key, true);
    return false;
}

void kbd_seq_hold(kbd_seq_t *s, uint8_t key, bool down, uint32_t now_ms) {
    if (down) {
        s->held = key;
        s->releasing = false;
        s->held_since_ms = now_ms;
    } else if (key == s->held) {
        s->releasing = true;
    }
}

void kbd_seq_break(kbd_seq_t *s) { s->break_count++; }

void kbd_seq_clear(kbd_seq_t *s) {
    uint8_t breaks = s->break_count, layout = s->layout;
    kbd_seq_init(s);
    s->break_count = breaks; /* the driver's acknowledgement counts these */
    s->layout = layout;
}

uint8_t kbd_seq_key(kbd_seq_t *s, uint32_t now_ms) {
    /* Apply every action that's due, oldest first. */
    while (s->head != s->tail) {
        if (s->started) {
            if ((int32_t)(now_ms - s->due_ms) < 0) break;
            s->head = (uint16_t)((s->head + 1) % KBD_QUEUE_LEN);
            s->started = false;
            continue;
        }
        const kbd_action_t *a = &s->queue[s->head];
        s->tapped = a->pressed ? a->key : 0;
        s->started = true;
        s->due_ms = now_ms + a->wait_ms;
    }
    if (s->releasing && now_ms - s->held_since_ms >= KBD_MIN_HOLD_MS) {
        s->held = 0;
        s->releasing = false;
    }
    return s->tapped ? s->tapped : s->held;
}

bool kbd_seq_busy(const kbd_seq_t *s) { return s->head != s->tail || s->tapped || s->held; }

/* ---- HID boot reports ---- */

/* HID keyboard usages (page 07H) this maps. */
enum {
    U_A = 0x04, U_Z = 0x1D, U_1 = 0x1E, U_0 = 0x27, U_ENTER = 0x28, U_BACKSPACE = 0x2A,
    U_TAB = 0x2B, U_SPACE = 0x2C, U_LBRACKET = 0x2F, U_RBRACKET = 0x30, U_F1 = 0x3A,
    U_F6 = 0x3F, U_F7 = 0x40, U_F8 = 0x41, U_F9 = 0x42, U_F10 = 0x43, U_F11 = 0x44,
    U_F12 = 0x45, U_INSERT = 0x49, U_DELETE = 0x4C, U_RIGHT = 0x4F, U_LEFT = 0x50,
    U_DOWN = 0x51, U_UP = 0x52, U_KP_SLASH = 0x54, U_KP_STAR = 0x55, U_KP_MINUS = 0x56,
    U_KP_PLUS = 0x57, U_KP_ENTER = 0x58, U_KP_1 = 0x59, U_KP_0 = 0x62, U_KP_DOT = 0x63,
};
#define MOD_CTRL 0x11  /* left, right */
#define MOD_SHIFT 0x22
#define MOD_ALTGR 0x40 /* right Alt */

/* US layout, usages 2DH-38H: unshifted, shifted. */
static const char kPunct[][2] = {
    {'-', '_'}, {'=', '+'}, {'[', '{'}, {']', '}'}, {'\\', '|'}, {0, 0}, {';', ':'},
    {'\'', '"'}, {'`', '~'}, {',', '<'}, {'.', '>'}, {'/', '?'},
};
static const char kShiftedDigits[] = ")!@#$%^&*("; /* Shift + 0..9 */

/* The layouts but US (KBD_LAYOUT_FR..BE), the character keys by their US
 * position: what's printed on them, from the standard (Windows) layouts.
 * A letter string per layout for usages 04H-1DH (US A..Z) -- '1' marks
 * AZERTY's ",?" key where US has M -- then plain / Shift / AltGr for the
 * number row (1EH-27H), the punctuation keys (2DH-38H) and the ISO key by
 * the left Shift (64H). 0: a key of the layout with nothing the PC-1500
 * has (accented letters, sharp s, n tilde, pound, degree, section, euro,
 * most dead keys -- a dead circumflex types '^', which the PC-1500 has);
 * D: not the layout's (Enter, Esc, Backspace, Tab, Space: as US). Digits
 * on Shift for the AZERTYs, as printed. Only French was checked on a real
 * keyboard (2026-10-07, the KHB030). */
#define D 0x7F
static const char *const kLayoutLetters[KBD_LAYOUT_COUNT] = {
    [KBD_LAYOUT_FR] = "QBCDEFGHIJKL1NOPARSTUVZXYW",
    [KBD_LAYOUT_DE] = "ABCDEFGHIJKLMNOPQRSTUVWXZY",
    [KBD_LAYOUT_ES] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ",
    [KBD_LAYOUT_BE] = "QBCDEFGHIJKL1NOPARSTUVZXYW",
};
static const char kLayoutKeys[KBD_LAYOUT_COUNT][28][3] = {
    [KBD_LAYOUT_FR] = {
        {'&', '1', 0}, {0, '2', '~'}, {'"', '3', '#'}, {'\'', '4', '{'}, {'(', '5', '['},
        {'-', '6', '|'}, {0, '7', '`'}, {'_', '8', '\\'}, {0, '9', '^'}, {0, '0', '@'},
        {D, D, D}, {D, D, D}, {D, D, D}, {D, D, D}, {D, D, D},             /* 28H-2CH */
        {')', 0, ']'}, {'=', '+', '}'}, {'^', 0, 0}, {'$', 0, 0}, {'*', 0, 0}, {'*', 0, 0},
        {'M', 'M', 0}, {0, '%', 0}, {0, 0, 0}, {';', '.', 0}, {':', '/', 0}, {'!', 0, 0},
        {'<', '>', 0},                                                      /* 64H */
    },
    [KBD_LAYOUT_DE] = {
        {'1', '!', 0}, {'2', '"', 0}, {'3', 0, 0}, {'4', '$', 0}, {'5', '%', 0},
        {'6', '&', 0}, {'7', '/', '{'}, {'8', '(', '['}, {'9', ')', ']'}, {'0', '=', '}'},
        {D, D, D}, {D, D, D}, {D, D, D}, {D, D, D}, {D, D, D},
        {0, '?', '\\'}, {0, 0, 0}, {0, 0, 0}, {'+', '*', '~'}, {'#', '\'', 0}, {'#', '\'', 0},
        {0, 0, 0}, {0, 0, 0}, {'^', 0, 0}, {',', ';', 0}, {'.', ':', 0}, {'-', '_', 0},
        {'<', '>', '|'},
    },
    [KBD_LAYOUT_ES] = {
        {'1', '!', '|'}, {'2', '"', '@'}, {'3', 0, '#'}, {'4', '$', '~'}, {'5', '%', 0},
        {'6', '&', 0}, {'7', '/', 0}, {'8', '(', 0}, {'9', ')', 0}, {'0', '=', 0},
        {D, D, D}, {D, D, D}, {D, D, D}, {D, D, D}, {D, D, D},
        {'\'', '?', 0}, {0, 0, 0}, {0, '^', '['}, {'+', '*', ']'}, {0, 0, '}'}, {0, 0, '}'},
        {0, 0, 0}, {0, 0, '{'}, {0, 0, '\\'}, {',', ';', 0}, {'.', ':', 0}, {'-', '_', 0},
        {'<', '>', 0},
    },
    [KBD_LAYOUT_BE] = {
        {'&', '1', '|'}, {0, '2', '@'}, {'"', '3', '#'}, {'\'', '4', 0}, {'(', '5', 0},
        {0, '6', '^'}, {0, '7', 0}, {'!', '8', 0}, {0, '9', '{'}, {0, '0', '}'},
        {D, D, D}, {D, D, D}, {D, D, D}, {D, D, D}, {D, D, D},
        {')', 0, 0}, {'-', '_', 0}, {'^', 0, '['}, {'$', '*', ']'}, {0, 0, '`'}, {0, 0, '`'},
        {'M', 'M', 0}, {0, '%', 0}, {0, 0, 0}, {';', '.', 0}, {':', '/', 0}, {'=', '+', '~'},
        {'<', '>', '\\'},
    },
};

/* The character for usage `u` in layout `layout` (not US), 0 for none, or
 * -1 for a key the layout doesn't move (the US mapping then). Letters come
 * back in capitals: SML picks the case. */
static int layout_char(uint8_t layout, uint8_t u, bool shift, bool altgr) {
    if (layout == KBD_LAYOUT_US || layout >= KBD_LAYOUT_COUNT) return -1;
    if (u >= U_A && u <= U_Z) {
        char c = kLayoutLetters[layout][u - U_A];
        if (c == '1') return altgr ? 0 : shift ? '?' : ','; /* AZERTY's key where US has M */
        if (altgr) return layout == KBD_LAYOUT_DE && c == 'Q' ? '@' : 0; /* the euro sign, and so on */
        return c;
    }
    int row;
    if (u >= U_1 && u <= 0x38) row = u - U_1;
    else if (u == 0x64) row = 27;
    else return -1;
    char c = kLayoutKeys[layout][row][altgr ? 2 : shift ? 1 : 0];
    return c == D ? -1 : c;
}
#undef D

/* The control key a usage holds down, or 0. */
static uint8_t held_key(uint8_t u, bool shift) {
    if (u >= U_F1 && u <= U_F6) {
        static const uint8_t f[] = {KBD_K_F1, KBD_K_F2, KBD_K_F3, KBD_K_F4, KBD_K_F5, KBD_K_F6};
        return f[u - U_F1];
    }
    switch (u) {
        case U_ENTER:
        case U_KP_ENTER: return KBD_K_ENTER;
        case U_BACKSPACE:
        case U_LEFT: return KBD_K_LEFT;
        case U_RIGHT: return KBD_K_RIGHT;
        case U_UP: return KBD_K_UP;
        case U_DOWN: return KBD_K_DOWN;
        case U_TAB: return KBD_K_SHIFT;
        case U_F7: return KBD_K_CL;
        case U_F8: return KBD_K_MODE;
        case U_F9: return KBD_K_DEF;
        case U_F10: return shift ? KBD_K_ROCKER : KBD_K_SML;
        case U_F11: return KBD_K_RCL;
        case U_F12: return shift ? KBD_K_OFF : 0;
        default: return 0;
    }
}

static void key_down(kbd_seq_t *s, uint8_t u, uint8_t mods, uint32_t now_ms) {
    bool shift = (mods & MOD_SHIFT) != 0;
    if (mods & MOD_CTRL) return;
    uint8_t held = held_key(u, shift);
    if (held) {
        /* behind taps still queued, it waits its turn as a tap */
        if (s->head != s->tail || s->tapped) kbd_seq_tap(s, held, false);
        else kbd_seq_hold(s, held, true, now_ms);
        return;
    }
    int c = layout_char(s->layout, u, shift, (mods & MOD_ALTGR) != 0);
    if (c > 0) kbd_seq_char(s, (char)c);
    if (c >= 0) return;
    if (u == U_F12) {
        kbd_seq_break(s);
    } else if (u == U_INSERT || u == U_DELETE) {
        kbd_seq_tap(s, u == U_INSERT ? KBD_K_RIGHT : KBD_K_LEFT, true);
    } else if (u >= U_A && u <= U_Z) {
        kbd_seq_char(s, (char)('A' + (u - U_A)));
    } else if (u >= U_1 && u <= U_0) {
        int d = (u - U_1 + 1) % 10; /* 1..9, then 0 */
        kbd_seq_char(s, shift ? kShiftedDigits[d] : (char)('0' + d));
    } else if (u == U_SPACE) {
        kbd_seq_char(s, ' ');
    } else if (u == U_LBRACKET || u == U_RBRACKET) {
        kbd_seq_char(s, u == U_LBRACKET ? '(' : ')');
    } else if (u >= 0x2D && u <= 0x38) {
        char c = kPunct[u - 0x2D][shift];
        if (c) kbd_seq_char(s, c);
    } else if (u >= U_KP_1 && u <= U_KP_0) {
        kbd_seq_char(s, (char)('0' + (u - U_KP_1 + 1) % 10));
    } else if (u == U_KP_SLASH || u == U_KP_STAR || u == U_KP_MINUS || u == U_KP_PLUS || u == U_KP_DOT) {
        kbd_seq_char(s, u == U_KP_SLASH ? '/' : u == U_KP_STAR ? '*' : u == U_KP_MINUS ? '-' : u == U_KP_PLUS ? '+' : '.');
    }
}

static bool has_key(const uint8_t *keys, uint8_t u) {
    for (int i = 0; i < 6; i++)
        if (keys[i] == u) return true;
    return false;
}

void kbd_seq_report(kbd_seq_t *s, const uint8_t report[8], uint32_t now_ms) {
    const uint8_t *keys = report + 2, *was = s->report + 2;
    if (keys[0] == 0x01) return; /* phantom state: too many keys down */
    for (int i = 0; i < 6; i++) /* up first: a held key let go (whatever Shift does now) */
        if (was[i] && !has_key(keys, was[i]) && s->held &&
            (s->held == held_key(was[i], false) || s->held == held_key(was[i], true)))
            kbd_seq_hold(s, s->held, false, now_ms);
    for (int i = 0; i < 6; i++)
        if (keys[i] > 0x03 && !has_key(was, keys[i])) key_down(s, keys[i], report[0], now_ms);
    memcpy(s->report, report, sizeof s->report);
}

/* ---- the driver's wait loop ---- */

static uint32_t crc32(const uint8_t *p, uint32_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

/* Offsets into the loop (ROM1 address - E24AH) of what gets patched. */
enum {
    PATCH_ANY = 0x24,      /* E26DH: SJP E418H (any key down?) -> KBD_ANY */
    PATCH_SCAN_1 = 0x29,   /* E272H: SJP E42CH (which key?) -> KBD_SCAN */
    PATCH_SCAN_2 = 0x63,   /* E2ACH: SJP E42CH -> KBD_SCAN */
    PATCH_BCH_1 = 0x52,    /* E29BH: BCH E366H -> BCH KBD_DISPATCH */
    PATCH_BCH_2 = 0x6C,    /* E2B5H: BCH E366H -> BCH KBD_DISPATCH */
    PATCH_RESUME = 0x11A,  /* E363H: JMP E269H (after power-off) -> the copy's own */
    LOOP_E269 = 0x1F,
};

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

kbd_loop_result_t kbd_loop_install(const uint8_t *loop, uint8_t *rom, uint32_t rom_len, uint32_t *crc) {
    uint32_t c = crc32(loop, KBD_LOOP_LEN);
    if (crc) *crc = c;
    if (loop[KBD_HOOK_TEST_ADDR - 0xE24Au] != KBD_HOOK_TEST_OK) return KBD_LOOP_NO_HOOK;
    if (c != KBD_LOOP_CRC32) return KBD_LOOP_UNKNOWN_ROM;
    const uint8_t *d = rom + KBD_DESCRIPTOR_OFFSET;
    uint16_t at = (uint16_t)((d[0] << 8) | d[1]), any = (uint16_t)((d[2] << 8) | d[3]);
    uint16_t scan = (uint16_t)((d[4] << 8) | d[5]), dispatch = (uint16_t)((d[6] << 8) | d[7]);
    uint32_t base = 0x8800;
    if (at < base || at - base + KBD_LOOP_LEN > rom_len) return KBD_LOOP_BAD_LAYOUT;
    /* BCH reaches 255 bytes on from the byte after it. */
    uint32_t from_1 = at + PATCH_BCH_1 + 1u, from_2 = at + PATCH_BCH_2 + 1u;
    if (dispatch < from_1 || dispatch - from_1 > 0xFF || dispatch < from_2 || dispatch - from_2 > 0xFF)
        return KBD_LOOP_BAD_LAYOUT;

    uint8_t *p = rom + (at - base);
    memcpy(p, loop, KBD_LOOP_LEN);
    put16(p + PATCH_ANY, any);
    put16(p + PATCH_SCAN_1, scan);
    put16(p + PATCH_SCAN_2, scan);
    p[PATCH_BCH_1] = (uint8_t)(dispatch - from_1);
    p[PATCH_BCH_2] = (uint8_t)(dispatch - from_2);
    put16(p + PATCH_RESUME, (uint16_t)(at + LOOP_E269));
    return KBD_LOOP_OK;
}
