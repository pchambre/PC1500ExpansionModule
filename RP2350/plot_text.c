/* plot_text.c -- CE-150 lettering as pen strokes. See plot_text.h. */
#include "plot_text.h"

#include "hershey_simplex.h"

/* Hershey units to the cell: a capital spans y = -12 (top) .. +9 (baseline),
 * 21 units, onto the 24n-quarter-step character height; 20 units of width
 * (the widest capitals, M and W, are about that) onto its 16n width. Each
 * glyph is centred on its own (left + right) / 2 within the 16n. */
#define HERSHEY_BASELINE 9

/* Rounded n / d, d > 0, for either sign of n. */
static int32_t div_round(int32_t n, int32_t d)
{
    return n >= 0 ? (n + d / 2) / d : -((-n + d / 2) / d);
}

static int clamp_csize(int csize)
{
    return csize < 1 ? 1 : csize > 9 ? 9 : csize;
}

/* The unit vectors of the text frame's u (advance) and v (up) axes in
 * plotter coordinates, per ROTATE. */
static const int8_t kAxes[4][4] = {
    /* ux, uy, vx, vy */
    { 1, 0, 0, 1 },   /* 0: left to right */
    { 0, -1, 1, 0 },  /* 1: down the paper, tops to the right */
    { -1, 0, 0, -1 }, /* 2: upside down */
    { 0, 1, -1, 0 },  /* 3: up the paper, tops to the left */
};

void plot_text_char(const plot_sink_t *sink, int32_t x, int32_t y,
                    uint8_t ch, int csize, int rotate)
{
    if (ch < HERSHEY_FIRST || ch >= HERSHEY_FIRST + HERSHEY_COUNT)
        return;
    const int32_t n = clamp_csize(csize);
    const int8_t *ax = kAxes[rotate & 3];
    const hershey_glyph_t *g = &kHersheyGlyphs[ch - HERSHEY_FIRST];
    const int8_t *p = &kHersheyPoints[2 * g->offset];
    const int32_t mid2 = g->left + g->right; /* twice the glyph's centre */
    int pen_up = 1;

    for (unsigned i = 0; i < g->count; i++, p += 2) {
        if (p[0] == HERSHEY_PEN_UP) {
            pen_up = 1;
            continue;
        }
        /* u = 8n + (hx - mid) * 16n / 20; v = (baseline - hy) * 24n / 21 */
        const int32_t u = 8 * n + div_round((2 * p[0] - mid2) * 2 * n, 5);
        const int32_t v = div_round((HERSHEY_BASELINE - p[1]) * 8 * n, 7);
        const int32_t px = x + u * ax[0] + v * ax[2];
        const int32_t py = y + u * ax[1] + v * ax[3];
        if (pen_up)
            sink->move(sink->ctx, px, py);
        else
            sink->draw(sink->ctx, px, py);
        pen_up = 0;
    }
}

void plot_text_advance(int csize, int rotate, int32_t *dx, int32_t *dy)
{
    const int32_t pitch = 24 * clamp_csize(csize);
    *dx = pitch * kAxes[rotate & 3][0];
    *dy = pitch * kAxes[rotate & 3][1];
}

void plot_text_line_feed(int csize, int rotate, int32_t *dx, int32_t *dy)
{
    const int32_t down = 48 * clamp_csize(csize);
    *dx = -down * kAxes[rotate & 3][2];
    *dy = -down * kAxes[rotate & 3][3];
}
