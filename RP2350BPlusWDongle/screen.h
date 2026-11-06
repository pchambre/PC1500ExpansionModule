/* screen.h -- the PC-1500's screen on the host app's Screen pane
 * (BLE_PROTOCOL.md's DISPLAY frames, MCONF BLDISP; 2026-11-03, out of the
 * 2026-10-17 text watch trial and ZIP/1500's mirror). One mirror for
 * everything the PC-1500 shows, fed by:
 *   - BASIC's text buffers, seen on the bus (screen_watch_write(), from
 *     screen_watch.h's PIO on the RP2350, or pc1500emu's bus): lines typed,
 *     entered and listed, PRINT's output, BREAK's message, the indicators
 *   - the module ROM's own LCD lines, which never pass through BASIC's
 *     buffers: SHOW (screen_show()) and BROWSE listings (screen_listing())
 *   - the SSH terminal (ssh_session.c) and ZIP/1500's console
 *     (zterm_session.c), as records (screen_line()/screen_edit()/...).
 *
 * Finished lines go into a history ring; the line being edited, the
 * indicators and ZIP's status lines are only ever their latest state. A
 * link that comes up (or BLDISP turned on) gets CLEAR, then the last
 * SCREEN_CATCHUP bytes of the history, then the latest states. With
 * BLDISP=0 nothing is kept. In RAM only: a POWMAN wake starts it empty.
 *
 * Portable C, no Pico SDK: the firmware and pc1500emu's ExpansionMock
 * both build it, each with its own screen_backend_t. Single-threaded: the
 * firmware calls it on core1 only; the mock serializes with lock/unlock. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* DISPLAY record kinds (BLE_PROTOCOL.md): `kind len bytes`. */
#define SCREEN_LINE 1   /* a line entered or listed */
#define SCREEN_OUTPUT 2 /* output: PRINT's, a listing's, ZIP's, SSH's */
#define SCREEN_EDIT 3   /* the line being typed or edited: [cursor i8][text] */
#define SCREEN_CLEAR 4  /* BLCLS, CLS typed, a new game */
#define SCREEN_STATUS 5 /* the LCD's indicators: [764EH][764FH] */
#define SCREEN_GAME 6   /* ZIP's status lines: location (str8), score text; empty = none */

#define SCREEN_HISTORY 8192u /* the ring: room for a long SDLS while a frame waits */
#define SCREEN_CATCHUP 3072u /* what a new link gets of it */
#define SCREEN_TEXT_MAX 200u /* a record's text, at most */

typedef struct {
    void *ctx;
    uint32_t (*session)(void *ctx);         /* 0: no link; else different for every link */
    int (*display)(void *ctx, const uint8_t *payload, uint16_t len); /* 1 sent, 0 no link/display, -1 failed */
    uint16_t (*display_max)(void *ctx);     /* the largest payload */
    bool (*enabled)(void *ctx);             /* MCONF BLDISP */
    uint32_t (*now_us)(void *ctx);
    int (*rom_byte)(void *ctx, uint16_t addr); /* the module ROM image (8800H-9FFFH), -1 none */
    void (*drain)(void *ctx);               /* the bus samples so far to screen_watch_write(), or NULL */
    void (*lock)(void *ctx);                /* both NULL in the firmware */
    void (*unlock)(void *ctx);
} screen_backend_t;

/* Everything dropped; the backend to use from now on. */
void screen_init(const screen_backend_t *backend);

/* ---- sources ---- */
void screen_line(uint8_t kind, const char *text, size_t len); /* SCREEN_LINE or SCREEN_OUTPUT */
void screen_edit(const char *text, size_t len, int cursor);   /* cursor: its column, -1 none */
void screen_clear(void);
void screen_status(const uint8_t status[2]);
void screen_game(const uint8_t *bytes, uint8_t len);
/* A DISPLAY payload's records, each as the calls above (zterm.h's mirror). */
void screen_records(const uint8_t *payload, uint16_t len);
/* Changes whenever the history was dropped (screen_init(), BLDISP turned
 * on): a source that keeps its own place in it starts over. */
uint32_t screen_generation(void);

/* The module ROM draws these straight onto the LCD (ROM1's DISP_N_CHARS0).
 * SHOW: like the LCD, the next SHOW replaces it -- WFCON's PASSWORD:
 * prompt redraws on every key -- so it stands as the EDIT line until
 * anything else comes, which makes it a line. A listing: the
 * EXP_COMMAND_LIST_SD_DIR window, every row of it. */
void screen_show(const uint8_t *chars, size_t len);
void screen_listing(const uint8_t *window);

/* ---- BASIC's buffers on the bus ---- */
/* One ME0 write to 7000H-7FFFH. */
void screen_watch_write(uint16_t addr, uint8_t data);
/* While ZIP/1500 has the screen: BASIC's buffers aren't what's shown. */
void screen_watch_mute(bool mute);

/* Reports what settled, then sends one frame. True if one went (there may
 * be more). */
bool screen_poll(void);

/* BLDISP on and a link up: the bus has to be watched, so no sleep. */
bool screen_watching(void);

#ifdef __cplusplus
}
#endif
