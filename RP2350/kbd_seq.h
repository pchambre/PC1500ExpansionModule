/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* kbd_seq.h -- the external keyboard's virtual key (2026-10-04).
 *
 * An external (BLE) keyboard reaches BASIC as a key of the PC-1500's own
 * 8x8 matrix: the MCU publishes one matrix index in the data window
 * (EXP_KBD_KEY), and the ROM's keyboard driver (rom.asm KBD_ENTRY/
 * KBD_LOOP, on the base ROM's 79D4H/785BH keyboard hook) reads it as if
 * that key were held down. The base ROM's own wait loop then does the rest -- SHIFT, DEF and
 * SML state, debounce, auto-repeat, auto power-off -- exactly as for a
 * physical key. This is the same model pc1500emu's host keyboard uses
 * (src/host/main.cpp), and the timings and the character map below are
 * that code's: a text character is a queued tap (a Shift tap first for a
 * shifted symbol), a control key is held for as long as it's held on the
 * keyboard.
 *
 * A matrix index is what KEYSCAN_NOWAIT (E42CH) leaves in XL: 80H + 8 *
 * column + (7 - row), the key's code being at FE00H + index (FE40H + index
 * shifted). 0 = no key. ON (BREAK) isn't in the matrix: it's a counter
 * (EXP_KBD_BREAK) the driver compares with its own acknowledgement.
 *
 * Portable C with no Pico SDK dependencies: pc1500emu's ExpansionMock
 * compiles this same file. One owner, no locking: the firmware calls it
 * from core0 only (BTstack's callbacks and the bus loop both run there). */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Matrix indices, from pc1500emu's keyboard.cpp kMatrix (confirmed on a
 * real PC-1500) -- each checked against ROM1's FE80H table. */
enum {
    KBD_K_UP = 0x80, KBD_K_N = 0x81, KBD_K_Y = 0x82, KBD_K_SHIFT = 0x83,
    KBD_K_H = 0x84, KBD_K_8 = 0x85, KBD_K_5 = 0x86, KBD_K_2 = 0x87,
    KBD_K_ROCKER = 0x88, KBD_K_X = 0x89, KBD_K_W = 0x8A, KBD_K_F1 = 0x8B,
    KBD_K_S = 0x8C, KBD_K_OFF = 0x8D, KBD_K_MINUS = 0x8E, KBD_K_PERIOD = 0x8F,
    KBD_K_0 = 0x90, KBD_K_M = 0x91, KBD_K_U = 0x92, KBD_K_F5 = 0x93,
    KBD_K_J = 0x94, KBD_K_7 = 0x95, KBD_K_4 = 0x96, KBD_K_1 = 0x97,
    KBD_K_ENTER = 0x98, KBD_K_LPAREN = 0x99, KBD_K_I = 0x9A, KBD_K_F6 = 0x9B,
    KBD_K_K = 0x9C, KBD_K_O = 0x9D, KBD_K_L = 0x9E, KBD_K_RPAREN = 0x9F,
    KBD_K_RCL = 0xA0, KBD_K_C = 0xA1, KBD_K_E = 0xA2, KBD_K_F2 = 0xA3,
    KBD_K_D = 0xA4, KBD_K_SLASH = 0xA5, KBD_K_ASTERISK = 0xA6, KBD_K_PLUS = 0xA7,
    KBD_K_SPACE = 0xA8, KBD_K_V = 0xA9, KBD_K_R = 0xAA, KBD_K_F3 = 0xAB,
    KBD_K_F = 0xAC, KBD_K_P = 0xAD, KBD_K_LEFT = 0xAE, KBD_K_EQUALS = 0xAF,
    KBD_K_SML = 0xB0, KBD_K_Z = 0xB1, KBD_K_Q = 0xB2, KBD_K_DEF = 0xB3,
    KBD_K_A = 0xB4, KBD_K_CL = 0xB5, KBD_K_MODE = 0xB6, KBD_K_RIGHT = 0xB7,
    KBD_K_DOWN = 0xB8, KBD_K_B = 0xB9, KBD_K_T = 0xBA, KBD_K_F4 = 0xBB,
    KBD_K_G = 0xBC, KBD_K_9 = 0xBD, KBD_K_6 = 0xBE, KBD_K_3 = 0xBF,
};

/* pc1500emu's kTapFrames/kIdleFrames (4 frames each at 60Hz): a tap is
 * held long enough for the ROM's ~25ms scan to see it, then released long
 * enough for its debounce to let the next key through. */
#define KBD_TAP_MS 67
#define KBD_IDLE_MS 67
/* The shortest a held key stays down (pc1500emu Bus's kMinimumHoldCycles,
 * 40000 cycles at 1.3MHz): one scan period always sees it. */
#define KBD_MIN_HOLD_MS 31

#define KBD_QUEUE_LEN 256 /* actions: 64 shifted symbols, or 128 plain characters */

typedef struct {
    uint8_t key; /* matrix index */
    bool pressed;
    uint16_t wait_ms; /* after applying this, before the next action */
} kbd_action_t;

typedef struct kbd_seq {
    kbd_action_t queue[KBD_QUEUE_LEN];
    uint16_t head, tail; /* head == tail: empty */
    bool started;        /* the head action has been applied */
    uint32_t due_ms;     /* ...and the next one is due then */
    uint8_t tapped;      /* the key the queue is holding down, or 0 */
    uint8_t held;        /* a control key held on the keyboard, or 0 */
    bool releasing;      /* `held` is up, but still within its minimum hold */
    uint32_t held_since_ms;
    uint8_t break_count; /* EXP_KBD_BREAK */
    uint8_t report[8];   /* kbd_seq_report(): the keyboard's last boot report */
} kbd_seq_t;

void kbd_seq_init(kbd_seq_t *s);

/* Queues one character's taps (pc1500emu's charToTapActions): letters
 * (either case -- SML picks the case, not the character), digits, space,
 * . / + - = ( ) * directly, ! " # $ % & @ ^ ? : < > ; , as Shift then the
 * key, and CR as ENTER. False (nothing queued) for anything else, or when
 * the queue is full. */
bool kbd_seq_char(kbd_seq_t *s, char c);

/* Queues a tap of one matrix key, after a Shift tap if `shifted`. */
bool kbd_seq_tap(kbd_seq_t *s, uint8_t key, bool shifted);

/* A control key going down or up on the keyboard (pc1500emu's
 * isImmediateHostKey: the arrows, CL, OFF, ENTER, SHIFT, MODE, DEF, RCL,
 * F1-F6) -- passed straight through rather than queued. One at a time: a
 * second key down replaces the first. */
void kbd_seq_hold(kbd_seq_t *s, uint8_t key, bool down, uint32_t now_ms);

/* ON: one BREAK. */
void kbd_seq_break(kbd_seq_t *s);

/* Drops everything queued and lets go of any key. */
void kbd_seq_clear(kbd_seq_t *s);

/* Advances the queue to `now_ms` and returns the matrix index to publish
 * (0 = none): a queued tap wins over a held key. */
uint8_t kbd_seq_key(kbd_seq_t *s, uint32_t now_ms);

/* True while taps are still queued or a key is down. */
bool kbd_seq_busy(const kbd_seq_t *s);

/* A keyboard's HID boot-protocol input report (8 bytes: modifiers, 0, six
 * key usages -- classic and BLE keyboards alike): each key that went down
 * or up since the last one, mapped as pc1500emu's host keyboard maps it
 * (src/host/main.cpp kKeyMap and the F10/F12/Insert/Delete cases; US
 * layout):
 * - printable keys type their character (kbd_seq_char), Shift picking it;
 *   [ and ] are ( and ); a character the PC-1500 has no key for is ignored;
 * - held for as long as they're held (kbd_seq_hold): the arrows, Backspace
 *   (left), Tab (SHIFT), Enter, F1-F6, F7 CL, F8 MODE, F9 DEF, F10 SML
 *   (Shift+F10 the up/down rocker), F11 RCL, Shift+F12 OFF;
 * - F12 is ON (BREAK); Insert and Delete are Shift + right / left;
 * - with Ctrl down nothing goes through (the emulator keeps those for its
 *   menus) -- nor RESET, the emulator's Ctrl+F12.
 * The keypad's digits, + and * are their keys, its - / . their characters,
 * and its Enter is Enter (the emulator ignores that one). */
void kbd_seq_report(kbd_seq_t *s, const uint8_t report[8], uint32_t now_ms);

/* ---- The driver's wait loop (EXP_COMMAND_KBD_INSTALL) ----
 *
 * ROM1's keyboard wait loop, E24AH-E365H: KEYSCAN_WAIT (E243H) past its
 * own hook test, through AUTO_POWER_OFF. The module doesn't carry it --
 * the boot hook copies it from the machine's own ROM -- and its CRC-32 is
 * how the MCU knows it got ROM1's (the ROM BASWORD's PEEK &E2B9 = 56 test
 * also looks for). */
#define KBD_LOOP_LEN 284
#define KBD_LOOP_CRC32 0x8B43EF78u

/* Where the ROM tells the MCU what to patch in: ROM_BASE + 0x11, four
 * big-endian addresses -- KBD_LOOP (where the loop goes), KBD_ANY and
 * KBD_SCAN (its two keyboard reads, rerouted there) and KBD_DISPATCH (where
 * it hands a key to ROM1's SML_DISPATCH, E366H). See rom.asm's KBD_HOOK. */
#define KBD_DESCRIPTOR_OFFSET 0x11

/* E2B9H: BASWORD's ROM test. 38H (the NOP after ROM1's VEJ CCH,5BH at
 * E2B7H) where the hook works. An older PC-1500 ROM (2026-10-05, a real
 * machine's dump) has D5H there: its E2B7H is VEJ F4H,79H,D5H, which loads
 * the vector at 79D5H into U, and the STX P that follows jumps to whatever
 * X held -- the hook can't be used, so neither BASWORD nor this driver runs
 * on it. */
#define KBD_HOOK_TEST_ADDR 0xE2B9u
#define KBD_HOOK_TEST_OK 0x38u

typedef enum {
    KBD_LOOP_OK,
    KBD_LOOP_NO_HOOK,     /* E2B9H isn't 38H: a ROM whose hook doesn't work */
    KBD_LOOP_UNKNOWN_ROM, /* the hook looks right but the loop isn't ROM1's */
    KBD_LOOP_BAD_LAYOUT,  /* the module ROM's descriptor doesn't fit */
} kbd_loop_result_t;

/* Checks `loop` (KBD_LOOP_LEN bytes, copied from E24AH) and, if it is
 * ROM1's, writes it into `rom` -- the module's ROM image, `rom[0]` =
 * ROM_BASE (8800H), `rom_len` bytes -- at KBD_LOOP, patched: its scans call
 * KBD_ANY/KBD_SCAN, its two jumps to SML_DISPATCH branch to KBD_DISPATCH,
 * and its power-off resumes its own loop. Anything but KBD_LOOP_OK writes
 * nothing. `crc`, if not NULL, gets the loop's CRC-32. */
kbd_loop_result_t kbd_loop_install(const uint8_t *loop, uint8_t *rom, uint32_t rom_len, uint32_t *crc);

#ifdef __cplusplus
}
#endif
