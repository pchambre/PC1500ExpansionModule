/* zterm.h -- ZIP/1500's console (zmachine/DESIGN.md), 2026-10-29.
 *
 * The Z-machine runs on the LH5801 (zmachine/zip.asm); this is the screen
 * and keyboard it talks to with EXP_COMMAND_ZTERM: it keeps the game's
 * text (the transcript), shows it a 26-character line at a time, scrolls
 * it, and edits the input line. Every key the player presses comes here;
 * each KEY answer is the line the LCD shows next.
 *
 * Portable C, no allocation: the firmware (monitor.c, a flash ring) and
 * pc1500emu's mock (a vector) both run it. The transcript lives in the
 * store the platform passes in, and everything else in zterm_t, which a
 * platform keeps across a POWMAN sleep (it holds no pointers). */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZT_WIDTH 26     /* the LCD */
#define ZT_LINE_MAX 80  /* a line with word wrap off (the display buffer's length) */
#define ZT_INPUT_MAX 77 /* the longest input line */
#define ZT_HSTEP 13     /* how far < and > move a long line sideways */

/* KEY codes from KEYSCAN_WAIT (measured in pc1500emu, 2026-11-01) */
#define ZT_KEY_START 0x00 /* no key: an input line begins */
#define ZT_KEY_BS 0x08    /* <, as a backspace while typing */
#define ZT_KEY_DOWN 0x0A
#define ZT_KEY_UP 0x0B
#define ZT_KEY_RIGHT 0x0C /* > */
#define ZT_KEY_ENTER 0x0D
#define ZT_KEY_F1 0x11 /* the status lines */
#define ZT_KEY_F2 0x12 /* the settings menu */
#define ZT_KEY_CL 0x18

/* Settings, kept by the platform (the firmware: MCONF, mcu_config.h) */
#define ZT_SETTING_WRAP 0 /* 1 = word wrap to the LCD (default), 0 = lines up to ZT_LINE_MAX */

/* The transcript: bytes appended in order, read by position. A ring that
 * forgets its oldest bytes when full: positions keep counting, begin()
 * says which is the oldest still kept. And the console's settings. */
typedef struct {
    void *ctx;
    void (*reset)(void *ctx);
    void (*append)(void *ctx, const uint8_t *data, uint16_t len);
    uint32_t (*begin)(void *ctx);
    uint32_t (*end)(void *ctx); /* one past the newest */
    uint8_t (*byte)(void *ctx, uint32_t pos);
    uint16_t (*random16)(void *ctx); /* OPEN's seed for the game's RANDOM */
    uint16_t (*setting)(void *ctx, uint8_t id);
    void (*set_setting)(void *ctx, uint8_t id, uint16_t value);
} zt_store_t;

/* What the LCD shows: the text, a status line, or the settings menu. */
enum { ZT_SCREEN_TEXT, ZT_SCREEN_LOCATION, ZT_SCREEN_SCORE, ZT_SCREEN_MENU };

typedef struct {
    bool open;
    uint8_t story_channel; /* the SDOPEN channel the story is on */
    uint8_t flags;         /* OPEN's: bit 0 = a time game */
    uint32_t view;         /* the start of the line shown */
    uint32_t turn;         /* where this turn's text began */
    uint8_t input[ZT_INPUT_MAX];
    uint8_t input_len;
    uint8_t input_max;
    int16_t status_a; /* the status line: score or hours */
    int16_t status_b; /* ... moves or minutes */
    uint8_t status_flags; /* the story's Flags 1: bit 1 = a time game */
    uint8_t location_len;
    uint8_t location[40];
    uint8_t screen;  /* ZT_SCREEN_* */
    uint8_t width;   /* the line width the view was found with (0: not yet) */
    uint8_t hscroll; /* with wrap off: how far along the line the LCD starts */
} zterm_t;

/* Clears the console (no game open). */
void zterm_init(zterm_t *zt);

/* One EXP_COMMAND_ZTERM: the request at window[0..], the answer written
 * back there. Returns the EXP_STATUS_* for the instruction cell. */
uint8_t zterm_command(zterm_t *zt, const zt_store_t *store, uint8_t *window);

#ifdef __cplusplus
}
#endif
