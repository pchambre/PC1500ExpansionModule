/* plotter.h -- the CE-150 printer/plotter the expansion module stands in for
 * when no real one is attached (2026-09-30). Its state (TEXT/GRAPH mode,
 * pen, origin, CSIZE, ROTATE, line type, how far the paper has moved) and
 * what its BASIC commands do with the pen, as BLE_PROTOCOL.md's PLOT
 * operations. keywords.c parses the statements and sends the operations;
 * this file only does the geometry.
 *
 * Coordinates are absolute QUARTER steps (PLOT_Q per CE-150 step of
 * 0.2 mm): x across the paper, 0 at the left edge of the pen's travel; y
 * along it, + up the paper (towards what was printed earlier), 0 where the
 * MCU started. BASIC's GRAPH coordinates are whole steps from the origin
 * (SORGN): the caller multiplies by PLOT_Q and adds plot_origin().
 *
 * Like the CE-150 (Owner's Manual p.124-127), the pen can be sent anywhere,
 * but the real one stops at the side of the paper, and the paper backs up
 * at most 10.24 cm past the furthest point it has been fed to; outside that,
 * nothing is drawn, and the pen carries on from where it was told to be.
 *
 * Portable C, no Pico SDK: pc1500emu compiles it too. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PLOT_Q 4                        /* quarter steps per CE-150 step */
#define PLOT_X_MAX (215 * PLOT_Q)       /* the pen's travel: 216 steps, 43.2 mm */
#define PLOT_BACK_LIMIT (512 * PLOT_Q)  /* how far the paper backs up: 10.24 cm */
#define PLOT_COORD_MAX 2047             /* BASIC's coordinates: -2048..2047 (LINE) */

/* BLE_PROTOCOL.md's PLOT payload: [pen u8][x i16][y i32] (the pen's state
 * when the frame starts, pen up), then operations. Little-endian. */
#define PLOT_PREFIX 7
#define PLOT_OP_MOVE 0x01     /* x i16, y i32 */
#define PLOT_OP_DRAW 0x02     /* x i16, y i32 */
#define PLOT_OP_MOVE_REL 0x03 /* dx i8, dy i8 */
#define PLOT_OP_DRAW_REL 0x04 /* dx i8, dy i8 */
#define PLOT_OP_PEN 0x05      /* pen u8 */

/* Sends a PLOT payload; false if it didn't get there (no link, the peer
 * doesn't take PLOT). */
typedef bool (*plot_send_fn)(const uint8_t *payload, uint16_t len, void *ctx);

/* The CE-150's state after power-up: TEXT mode, CSIZE 2, pen 0, solid
 * lines, ROTATE 0, the pen at the left edge. */
void plot_reset(void);

/* A statement's drawing is buffered between these, and sent (in pieces if
 * it's long) through `send`. plot_end() sends the rest -- or, if nothing
 * was drawn but the pen or paper moved, just where it now is -- and
 * returns false if any of it failed. */
void plot_begin(plot_send_fn send, void *ctx);
bool plot_end(void);

/* ---- state ---- */
bool plot_graph_mode(void);
uint8_t plot_csize(void);
uint8_t plot_color(void);
uint8_t plot_line_type(void);
void plot_set_csize(uint8_t n);     /* 1-9 */
void plot_set_rotate(uint8_t r);    /* 0-3: GRAPH mode's LPRINT direction */
void plot_set_line_type(uint8_t t); /* 0 solid, 1-8 dashes of (t+1) steps, 9 pen up */
void plot_set_pen(uint8_t pen);     /* 0-3, with no other effect (LINE's colour) */

/* TEXT and GRAPH (CE-150 ROM LACA6/LACD3): both return the pen to the left
 * edge, make that the origin, and set CSIZE 2; GRAPH also sets ROTATE 0. */
void plot_text_mode(void);
void plot_graph_mode_on(void);
/* COLOR: the pen; in TEXT mode the pen also returns to the left edge. */
void plot_color_command(uint8_t pen);
/* SORGN: the pen's position becomes the origin. */
void plot_set_origin(void);

/* ---- the pen, in absolute quarter steps ---- */
void plot_origin(int32_t *x, int32_t *y);
void plot_position(int32_t *x, int32_t *y);
void plot_move(int32_t x, int32_t y); /* pen up */
void plot_line(int32_t x, int32_t y); /* in the current line type */

/* ---- text ---- */
uint8_t plot_columns(void);          /* characters per line at this CSIZE */
uint8_t plot_column(void);           /* the pen's column in TEXT mode */
void plot_set_column(uint8_t col);   /* LCURSOR/TAB; 0 is the carriage return */
void plot_newline(void);             /* carriage return and one line feed */
void plot_graph_newline(void);       /* the same, GRAPH mode's bare LPRINT: the counters stay */
void plot_feed(int32_t lines);       /* LF: + feeds the paper forward, - pulls it back */
void plot_char(uint8_t ch);          /* at the pen (turned by ROTATE in GRAPH mode), which moves on a cell */
void plot_test(void);                /* TEST: a box in each pen */

/* ---- the PLOT payload, for the link and for receivers ---- */

/* Splits a PLOT payload into frames of at most `max` bytes, each with the
 * prefix it needs: start with plot_split_start(), then call
 * plot_split_next() until it returns 0. */
typedef struct {
    uint16_t at;
    uint8_t pen;
    int32_t x, y;
} plot_split_t;
void plot_split_start(plot_split_t *s, const uint8_t *payload, uint16_t len);
uint16_t plot_split_next(plot_split_t *s, const uint8_t *payload, uint16_t len, uint8_t *out, uint16_t max);

/* A receiver's side: applies one PLOT payload to `pen` (the prefix sets
 * it), calling `line` for each line drawn. False for a malformed payload
 * (what it held up to there has been applied). */
typedef struct {
    uint8_t pen;
    int32_t x, y;
} plot_pen_t;
typedef void (*plot_line_fn)(void *ctx, uint8_t pen, int32_t x0, int32_t y0, int32_t x1, int32_t y1);
bool plot_decode(const uint8_t *payload, uint16_t len, plot_pen_t *pen, plot_line_fn line, void *ctx);

#ifdef __cplusplus
}
#endif
