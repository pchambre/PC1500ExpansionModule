/* cmd_history.c -- see cmd_history.h. */
#include "cmd_history.h"

#include <ctype.h>
#include <string.h>

#include "kbd_seq.h"

#define REPEAT_DELAY_MS 500
#define REPEAT_MS 80
#define IND_DEF 0x80   /* 764EH's bits (rom.asm STATUS1_ABS) */
#define IND_SHIFT 0x02

/* ---- the history ---- */

void hist_init(hist_t *h) { memset(h, 0, sizeof *h); }

const hist_entry_t *hist_get(const hist_t *h, uint16_t back) {
    if (back == 0 || back > h->count) return NULL;
    return &h->e[(h->newest + HIST_ENTRIES - (back - 1)) % HIST_ENTRIES];
}

bool hist_add(hist_t *h, const char *line, size_t len) {
    const hist_entry_t *last = hist_get(h, 1);
    hist_entry_t *e;
    if (len > HIST_LINE_MAX) len = HIST_LINE_MAX;
    while (len && line[len - 1] == ' ') len--; /* BASIC keeps none of them either */
    if (len == 0 || (last && last->len == len && memcmp(last->text, line, len) == 0)) return false;
    h->newest = (uint16_t)((h->newest + 1) % HIST_ENTRIES);
    e = &h->e[h->newest];
    memcpy(e->text, line, len);
    e->len = (uint8_t)len;
    if (h->count < HIST_ENTRIES) h->count++;
    return true;
}

/* ---- the UI ---- */

/* A key's character in a search: letters in capitals (matching ignores
 * case), digits, space, and the keyboard's symbols plain or with SHIFT as
 * printed (kbd_seq.c's table, the other way round). 0 for none. */
static char key_char(uint8_t key, bool shift) {
    static const struct {
        uint8_t key;
        char plain, shifted;
    } kKeys[] = {
        {KBD_K_0, '0', 0},   {KBD_K_1, '1', 0},     {KBD_K_2, '2', 0},       {KBD_K_3, '3', 0},
        {KBD_K_4, '4', 0},   {KBD_K_5, '5', 0},     {KBD_K_6, '6', 0},       {KBD_K_7, '7', 0},
        {KBD_K_8, '8', 0},   {KBD_K_9, '9', 0},     {KBD_K_SPACE, ' ', '^'}, {KBD_K_PERIOD, '.', 0},
        {KBD_K_SLASH, '/', '?'}, {KBD_K_ASTERISK, '*', ':'}, {KBD_K_PLUS, '+', ';'}, {KBD_K_MINUS, '-', ','},
        {KBD_K_EQUALS, '=', '@'}, {KBD_K_LPAREN, '(', '<'}, {KBD_K_RPAREN, ')', '>'}, {KBD_K_F1, 0, '!'},
        {KBD_K_F2, 0, '"'},  {KBD_K_F3, 0, '#'},    {KBD_K_F4, 0, '$'},      {KBD_K_F5, 0, '%'},
        {KBD_K_F6, 0, '&'},
    };
    static const uint8_t kLetters[26] = {KBD_K_A, KBD_K_B, KBD_K_C, KBD_K_D, KBD_K_E, KBD_K_F, KBD_K_G,
                                         KBD_K_H, KBD_K_I, KBD_K_J, KBD_K_K, KBD_K_L, KBD_K_M, KBD_K_N,
                                         KBD_K_O, KBD_K_P, KBD_K_Q, KBD_K_R, KBD_K_S, KBD_K_T, KBD_K_U,
                                         KBD_K_V, KBD_K_W, KBD_K_X, KBD_K_Y, KBD_K_Z};
    for (int i = 0; i < 26; i++)
        if (kLetters[i] == key) return (char)('A' + i);
    for (size_t i = 0; i < sizeof kKeys / sizeof kKeys[0]; i++)
        if (kKeys[i].key == key) return shift ? kKeys[i].shifted : kKeys[i].plain;
    return 0;
}

static bool holds(const hist_entry_t *e, const char *q, uint8_t qlen) {
    if (qlen == 0) return true;
    for (int i = 0; i + qlen <= e->len; i++) {
        int j = 0;
        while (j < qlen && toupper((unsigned char)e->text[i + j]) == toupper((unsigned char)q[j])) j++;
        if (j == qlen) return true;
    }
    return false;
}

/* The next match from `back` on, older (+1) or newer (-1); 0 if none. */
static uint16_t find(const hist_ui_t *u, int back, int dir) {
    for (; back >= 1 && back <= u->h->count; back += dir)
        if (holds(hist_get(u->h, (uint16_t)back), u->query, u->qlen)) return (uint16_t)back;
    return 0;
}

static void changed(hist_ui_t *u) { u->version++; }

bool hist_ui_start(hist_ui_t *u, const hist_t *h, hist_start_t how) {
    memset(u, 0, sizeof *u);
    u->h = h;
    if (h->count == 0) return false;
    u->search = how == HIST_START_SEARCH;
    u->back = 1;
    u->result = HIST_UI_OPEN;
    /* the arrow that opened it is still down: not a press of its own */
    u->held = how == HIST_START_OLDER ? KBD_K_UP : how == HIST_START_NEWER ? KBD_K_DOWN : KBD_K_LEFT;
    return true;
}

static void finish(hist_ui_t *u, hist_result_t r) {
    if ((r == HIST_UI_EDIT || r == HIST_UI_RUN) && u->back == 0) r = HIST_UI_CANCEL; /* nothing to take */
    u->result = r;
    changed(u);
}

/* A key going down. True if it's one that repeats when held. */
static bool press(hist_ui_t *u, uint8_t key) {
    bool shift = u->shift, def = u->def;
    char c;
    switch (key) {
        case KBD_K_SHIFT:
            u->shift = !u->shift;
            changed(u);
            return false;
        case KBD_K_DEF:
            if (!u->search) { /* browsing: DEF leaves, as it came in */
                finish(u, HIST_UI_CANCEL);
                return false;
            }
            u->def = !u->def;
            changed(u);
            return false;
        case KBD_K_SML: return false;
        default: break;
    }
    u->shift = u->def = false;
    switch (key) {
        case KBD_K_CL: finish(u, HIST_UI_CANCEL); return false;
        case KBD_K_ENTER: finish(u, HIST_UI_RUN); return false;
        case KBD_K_RIGHT: finish(u, HIST_UI_EDIT); return false;
        case KBD_K_LEFT:
            if (u->search && def) { /* the next older match */
                uint16_t b = find(u, u->back + 1, 1);
                if (b) u->back = b;
                changed(u);
                return true;
            }
            if (u->search && shift) { /* a character less */
                if (u->qlen) u->qlen--;
                u->back = find(u, 1, 1);
                changed(u);
                return true;
            }
            finish(u, HIST_UI_EDIT);
            return false;
        case KBD_K_UP:
        case KBD_K_DOWN: {
            int dir = key == KBD_K_UP ? 1 : -1;
            uint16_t b = u->search ? find(u, u->back + dir, dir) : (uint16_t)(u->back + dir);
            if (b >= 1 && b <= u->h->count) u->back = b;
            changed(u);
            return true;
        }
        default: break;
    }
    if (u->search && (c = key_char(key, shift)) != 0 && u->qlen < HIST_QUERY_MAX) {
        u->query[u->qlen++] = c;
        u->back = find(u, 1, 1);
        changed(u);
    }
    return false;
}

void hist_ui_key(hist_ui_t *u, uint8_t key, uint32_t now_ms) {
    if (u->result != HIST_UI_OPEN) return;
    if (key != u->held) {
        u->held = key;
        u->repeats = key && press(u, key);
        u->next_repeat_ms = now_ms + REPEAT_DELAY_MS;
        return;
    }
    if (key && u->repeats && (int32_t)(now_ms - u->next_repeat_ms) >= 0) {
        u->next_repeat_ms = now_ms + REPEAT_MS;
        /* again, with the modifiers it had: DEF+Left's next match */
        if (key == KBD_K_LEFT && u->search) u->def = true;
        press(u, key);
    }
}

void hist_ui_break(hist_ui_t *u) {
    if (u->result == HIST_UI_OPEN) finish(u, HIST_UI_BREAK);
}

void hist_ui_render(const hist_ui_t *u, char out[HIST_WIDTH]) {
    const hist_entry_t *e = hist_get(u->h, u->back);
    uint8_t n = 0;
    memset(out, ' ', HIST_WIDTH);
    if (u->search) { /* "PRI> PRINT A*2" */
        for (uint8_t i = 0; i < u->qlen && n < HIST_WIDTH; i++) out[n++] = u->query[i];
        if (n < HIST_WIDTH) out[n++] = '>';
        if (n < HIST_WIDTH) out[n++] = ' ';
        if (!e) {
            static const char kNone[] = "(NONE)";
            for (uint8_t i = 0; kNone[i] && n < HIST_WIDTH; i++) out[n++] = kNone[i];
            return;
        }
    }
    for (uint8_t i = 0; e && i < e->len && n < HIST_WIDTH; i++) out[n++] = e->text[i];
}

const hist_entry_t *hist_ui_selected(const hist_ui_t *u) {
    return u->result == HIST_UI_EDIT || u->result == HIST_UI_RUN ? hist_get(u->h, u->back) : NULL;
}

uint8_t hist_ui_indicators(const hist_ui_t *u) {
    return (uint8_t)((u->search ? (u->def ? IND_DEF : 0) : IND_DEF) | (u->shift ? IND_SHIFT : 0));
}
