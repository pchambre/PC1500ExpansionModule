/* ssh_keys.c -- see ssh_keys.h. */
#include "ssh_keys.h"

#include <string.h>

#include "kbd_seq.h"

/* Matrix index -> letter; 0 for keys that aren't letters. */
static char letter(uint8_t key) {
    static const struct {
        uint8_t key;
        char c;
    } kLetters[] = {
        {KBD_K_A, 'a'}, {KBD_K_B, 'b'}, {KBD_K_C, 'c'}, {KBD_K_D, 'd'}, {KBD_K_E, 'e'}, {KBD_K_F, 'f'},
        {KBD_K_G, 'g'}, {KBD_K_H, 'h'}, {KBD_K_I, 'i'}, {KBD_K_J, 'j'}, {KBD_K_K, 'k'}, {KBD_K_L, 'l'},
        {KBD_K_M, 'm'}, {KBD_K_N, 'n'}, {KBD_K_O, 'o'}, {KBD_K_P, 'p'}, {KBD_K_Q, 'q'}, {KBD_K_R, 'r'},
        {KBD_K_S, 's'}, {KBD_K_T, 't'}, {KBD_K_U, 'u'}, {KBD_K_V, 'v'}, {KBD_K_W, 'w'}, {KBD_K_X, 'x'},
        {KBD_K_Y, 'y'}, {KBD_K_Z, 'z'},
    };
    for (size_t i = 0; i < sizeof kLetters / sizeof kLetters[0]; i++)
        if (kLetters[i].key == key) return kLetters[i].c;
    return 0;
}

/* Everything else that types something: plain, with SHIFT, with DEF, with
 * DEF and SHIFT (0 = nothing). */
static const struct {
    uint8_t key;
    char plain, shift, def, def_shift;
} kOthers[] = {
    {KBD_K_0, '0', 0, 0, 0},        {KBD_K_1, '1', 0, 0, 0},        {KBD_K_2, '2', 0, 0, 0},
    {KBD_K_3, '3', 0, 0, 0},        {KBD_K_4, '4', 0, 0, 0},        {KBD_K_5, '5', 0, 0, 0},
    {KBD_K_6, '6', 0, 0, 0},        {KBD_K_7, '7', 0, 0, 0},        {KBD_K_8, '8', 0, 0, 0},
    {KBD_K_9, '9', 0, 0, 0},        {KBD_K_SPACE, ' ', '^', 0, 0},  {KBD_K_PERIOD, '.', 0, 0, 0},
    {KBD_K_SLASH, '/', '?', '\\', 0}, {KBD_K_ASTERISK, '*', ':', '`', 0}, {KBD_K_PLUS, '+', ';', 0, 0},
    {KBD_K_MINUS, '-', ',', 0, 0},  {KBD_K_EQUALS, '=', '@', 0, 0}, {KBD_K_LPAREN, '(', '<', '[', '{'},
    {KBD_K_RPAREN, ')', '>', ']', '}'}, {KBD_K_ENTER, '\r', 0, 0, 0}, {KBD_K_F1, '\t', '!', 0, 0},
    {KBD_K_F2, '\'', '"', 0, 0},    {KBD_K_F3, '_', '#', 0, 0},     {KBD_K_F4, '|', '$', 0, 0},
    {KBD_K_F5, '~', '%', 0, 0},     {KBD_K_F6, 0x1B, '&', 0, 0},    {KBD_K_CL, 0x15, 0, 0, 0},
    {KBD_K_RCL, 0x12, 0, 0, 0},
};

void ssh_keys_init(ssh_keys_t *k) { memset(k, 0, sizeof *k); }

/* A key going down: what it does. Returns false for one that does nothing
 * (a modifier, or a key with no meaning in this combination). */
static bool press(ssh_keys_t *k, uint8_t key) {
    char c;
    bool shift, def;
    bool arrow = key == KBD_K_UP || key == KBD_K_DOWN || key == KBD_K_LEFT || key == KBD_K_RIGHT;
    k->out_len = 0;
    k->view = SSH_VIEW_NONE;
    if (k->scrolling) {
        if (arrow) {
            k->view = key == KBD_K_UP ? SSH_VIEW_UP : key == KBD_K_DOWN ? SSH_VIEW_DOWN : key == KBD_K_LEFT ? SSH_VIEW_LEFT
                                                                                                            : SSH_VIEW_RIGHT;
            return true;
        }
        if (key == KBD_K_SML || key == KBD_K_SHIFT) {
            /* modifiers as usual, still scrolling */
        } else {
            k->scrolling = false;
            k->shift = k->def = false;
            if (key == KBD_K_DEF || key == KBD_K_CL) {
                k->view = SSH_VIEW_LIVE;
                return true;
            }
            k->view = SSH_VIEW_LIVE; /* any other key: back to the live line, and on to the shell */
        }
    }
    shift = k->shift, def = k->def;
    switch (key) {
        case KBD_K_SHIFT: k->shift = !k->shift; return false;
        case KBD_K_DEF: k->def = !k->def; return false;
        case KBD_K_SML: k->caps = !k->caps; return false;
        default: break;
    }
    k->shift = k->def = false; /* used up by this key, whatever it does */
    if ((c = letter(key)) != 0) {
        if (def) {
            k->out[k->out_len++] = (uint8_t)(c - 'a' + 1);
        } else {
            bool upper = k->caps != shift;
            k->out[k->out_len++] = (uint8_t)(upper ? c - 'a' + 'A' : c);
        }
        return true;
    }
    if (arrow) {
        if (def) {
            k->scrolling = true;
            k->view = key == KBD_K_UP ? SSH_VIEW_UP : key == KBD_K_DOWN ? SSH_VIEW_DOWN : key == KBD_K_LEFT ? SSH_VIEW_LEFT
                                                                                                            : SSH_VIEW_RIGHT;
        } else if (shift && key == KBD_K_LEFT) {
            k->out[k->out_len++] = 0x7F;
        } else {
            k->out[0] = 0x1B;
            k->out[1] = '[';
            k->out[2] = key == KBD_K_UP ? 'A' : key == KBD_K_DOWN ? 'B' : key == KBD_K_RIGHT ? 'C' : 'D';
            k->out_len = 3;
        }
        return true;
    }
    if (key == KBD_K_CL && def) {
        k->view = SSH_VIEW_QUIT;
        return true;
    }
    for (size_t i = 0; i < sizeof kOthers / sizeof kOthers[0]; i++) {
        if (kOthers[i].key != key) continue;
        c = def ? (shift ? kOthers[i].def_shift : kOthers[i].def) : shift ? kOthers[i].shift : kOthers[i].plain;
        if (!c) return false;
        k->out[k->out_len++] = (uint8_t)c;
        return true;
    }
    return false; /* MODE, OFF, the rocker: nothing */
}

size_t ssh_keys_sample(ssh_keys_t *k, uint8_t key, uint32_t now_ms, uint8_t out[SSH_KEYS_OUT_MAX], ssh_view_t *view) {
    *view = SSH_VIEW_NONE;
    if (key != k->held) {
        k->held = key;
        k->repeats = false;
        if (key == 0 || !press(k, key)) return 0;
        k->repeats = k->view != SSH_VIEW_QUIT && k->view != SSH_VIEW_LIVE;
        k->next_repeat_ms = now_ms + SSH_KEYS_REPEAT_DELAY_MS;
    } else if (key == 0 || !k->repeats || (int32_t)(now_ms - k->next_repeat_ms) < 0) {
        return 0;
    } else {
        k->next_repeat_ms = now_ms + SSH_KEYS_REPEAT_MS;
    }
    *view = k->view;
    memcpy(out, k->out, k->out_len);
    return k->out_len;
}
