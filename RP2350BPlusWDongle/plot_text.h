/* plot_text.h -- CE-150 lettering as pen strokes: one character in the
 * Hershey Roman Simplex font (hershey_simplex.h), scaled to a CSIZE cell and
 * turned to a ROTATE direction (2026-09-30).
 *
 * Units are QUARTER plotter steps (0.05 mm; the CE-150's step is 0.2 mm), in
 * plotter coordinates: x to the right across the paper, y up the paper.
 *
 * CSIZE n (1-9): a character is 0.8n x 1.2n mm = 16n x 24n quarter steps,
 * drawn at the start of a 24n-wide cell (so 36 columns at CSIZE 1 across
 * the 216-step width); a text line is 48n high (2.4 mm at CSIZE 1).
 *
 * Portable C, no Pico SDK: pc1500emu compiles it too. */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PLOT_QSTEPS_PER_STEP 4

/* Where the strokes go. move() lifts the pen and goes to (x, y); draw()
 * draws a straight line from the current point to (x, y). */
typedef struct {
    void (*move)(void *ctx, int32_t x, int32_t y);
    void (*draw)(void *ctx, int32_t x, int32_t y);
    void *ctx;
} plot_sink_t;

/* Draws `ch` in the cell whose lower-left corner (in the text's own frame,
 * i.e. before rotation) is at (x, y). Characters outside 0x20-0x7F draw
 * nothing. The pen is left up at the glyph's last point, or not moved at
 * all for a blank. `csize` is clamped to 1-9, `rotate` taken mod 4:
 *   0 normal, 1 the text runs down the paper (turned 90 degrees clockwise),
 *   2 upside down, running right to left, 3 running up the paper. */
void plot_text_char(const plot_sink_t *sink, int32_t x, int32_t y,
                    uint8_t ch, int csize, int rotate);

/* The step from one cell to the next along a text line (dx, dy), and from
 * one line to the next (a line feed: `down` is the move from a line's
 * origin to the next one's), for this CSIZE and ROTATE, in quarter steps. */
void plot_text_advance(int csize, int rotate, int32_t *dx, int32_t *dy);
void plot_text_line_feed(int csize, int rotate, int32_t *dx, int32_t *dy);

#ifdef __cplusplus
}
#endif
