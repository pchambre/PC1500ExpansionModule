/* plotter.c -- the CE-150 stand-in's geometry. See plotter.h.
 *
 * Three positions are kept: where BASIC has told the pen to be (x, y --
 * the CE-150's counters, which keep counting outside the paper); where the
 * real pen would be (px, py: that, stopped at the paper's side and at the
 * paper's backing limit); and where the receiver thinks it is (ex, ey, the
 * end of the last operation in the buffer). Only lines are sent; a pen-up
 * move becomes a MOVE just before the next line that needs it, or at the
 * end of the statement, so the receiver sees where the pen came to rest. */
#include "plotter.h"

#include <math.h>
#include <string.h>

#include "plot_text.h"

#define BUF_MAX 1000 /* one EXP_COMMAND_BLE_PLOT: 2 length bytes + this fit the 1K area */

static struct {
    bool graph;
    uint8_t csize, rotate, pen, type;
    int32_t x, y;      /* the counters */
    int32_t ox, oy;    /* the origin */
    int32_t px, py;    /* the real pen */
    int32_t front;     /* the lowest py so far: the paper backs up PLOT_BACK_LIMIT past it */
    int32_t told_x, told_y; /* where the last statement left the receiver's pen */
    float dash;        /* how far into the dash pattern this statement's line is */
} P = {.csize = 2};    /* plot_reset()'s state, for an MCU just powered up */

static struct {
    uint8_t data[BUF_MAX];
    uint16_t len; /* 0: not started */
    uint8_t epen;
    int32_t ex, ey;
    plot_send_fn send;
    void *ctx;
    bool failed;
} B;

void plot_reset(void) {
    memset(&P, 0, sizeof P);
    P.csize = 2;
}

/* ---- the buffer ---- */

static void put16(uint8_t *p, int32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, int32_t v) {
    put16(p, v);
    put16(p + 2, v >> 16);
}

static int32_t get16(const uint8_t *p) { return (int16_t)(p[0] | (p[1] << 8)); }
static int32_t get32(const uint8_t *p) { return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24)); }

static void start(uint8_t pen, int32_t x, int32_t y) {
    B.data[0] = pen;
    put16(B.data + 1, x);
    put32(B.data + 3, y);
    B.len = PLOT_PREFIX;
    B.epen = pen;
    B.ex = x;
    B.ey = y;
}

static void send(void) {
    if (B.len && !B.failed && !B.send(B.data, B.len, B.ctx)) B.failed = true;
    B.len = 0;
}

static void op(const uint8_t *bytes, uint16_t n) {
    if (B.len + n > BUF_MAX) {
        send();
        start(B.epen, B.ex, B.ey);
    }
    memcpy(B.data + B.len, bytes, n);
    B.len = (uint16_t)(B.len + n);
}

/* A MOVE or DRAW to (x, y), relative when it's close. The buffer has been
 * started. */
static void op_to(uint8_t absolute, uint8_t relative, int32_t x, int32_t y) {
    uint8_t bytes[7];
    const int32_t dx = x - B.ex, dy = y - B.ey;
    if (dx >= -128 && dx <= 127 && dy >= -128 && dy <= 127) {
        bytes[0] = relative;
        bytes[1] = (uint8_t)dx;
        bytes[2] = (uint8_t)dy;
        op(bytes, 3);
    } else {
        bytes[0] = absolute;
        put16(bytes + 1, x);
        put32(bytes + 3, y);
        op(bytes, 7);
    }
    B.ex = x;
    B.ey = y;
}

/* A line the receiver draws, from (x0, y0) to (x1, y1). */
static void emit_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    if (B.len == 0) start(P.pen, P.px, P.py);
    if (B.epen != P.pen) {
        const uint8_t bytes[2] = {PLOT_OP_PEN, P.pen};
        op(bytes, 2);
        B.epen = P.pen;
    }
    if (x0 != B.ex || y0 != B.ey) op_to(PLOT_OP_MOVE, PLOT_OP_MOVE_REL, x0, y0);
    op_to(PLOT_OP_DRAW, PLOT_OP_DRAW_REL, x1, y1);
}

void plot_begin(plot_send_fn send_fn, void *ctx) {
    B.send = send_fn;
    B.ctx = ctx;
    B.len = 0;
    B.failed = false;
    P.dash = 0;
}

bool plot_end(void) {
    if (B.len) {
        if (B.ex != P.px || B.ey != P.py) op_to(PLOT_OP_MOVE, PLOT_OP_MOVE_REL, P.px, P.py);
    } else if (P.px != P.told_x || P.py != P.told_y) {
        start(P.pen, P.px, P.py); /* no lines: just where the pen is now */
    }
    send();
    P.told_x = P.px;
    P.told_y = P.py;
    return !B.failed;
}

/* ---- the pen ---- */

static int32_t ymax(void) { return P.front + PLOT_BACK_LIMIT; }

/* The real pen follows the counters as far as the paper lets it. */
static void pen_to(int32_t x, int32_t y) {
    P.x = x;
    P.y = y;
    P.px = x < 0 ? 0 : x > PLOT_X_MAX ? PLOT_X_MAX : x;
    P.py = y > ymax() ? ymax() : y;
    if (P.py < P.front) P.front = P.py;
}

static int32_t round_f(float v) { return (int32_t)(v < 0 ? v - 0.5f : v + 0.5f); }

/* Draws the part of (x0, y0)-(x1, y1) that's on the paper (Liang-Barsky). */
static void draw_clipped(float x0, float y0, float x1, float y1) {
    const float dx = x1 - x0, dy = y1 - y0;
    const float p[3] = {-dx, dx, dy};
    const float q[3] = {x0, (float)PLOT_X_MAX - x0, (float)ymax() - y0};
    float t0 = 0, t1 = 1;
    for (int i = 0; i < 3; i++) {
        if (p[i] == 0) {
            if (q[i] < 0) return; /* parallel to that edge, and outside it */
            continue;
        }
        float t = q[i] / p[i];
        if (p[i] < 0) {
            if (t > t1) return;
            if (t > t0) t0 = t;
        } else {
            if (t < t0) return;
            if (t < t1) t1 = t;
        }
    }
    emit_line(round_f(x0 + t0 * dx), round_f(y0 + t0 * dy), round_f(x0 + t1 * dx), round_f(y0 + t1 * dy));
}

/* A line from the pen in `type`: solid, dashed, or not drawn at all. The
 * dash pattern (on and off (type + 1) steps each) carries on from one line
 * of a statement to the next. */
static void line_typed(int32_t x, int32_t y, uint8_t type) {
    const float x0 = (float)P.x, y0 = (float)P.y;
    const float dx = (float)(x - P.x), dy = (float)(y - P.y);
    const float len = sqrtf(dx * dx + dy * dy);
    if (type == 0) {
        draw_clipped(x0, y0, (float)x, (float)y);
    } else if (type < 9 && len > 0) {
        const float dash = (float)((type + 1) * PLOT_Q);
        float s = 0;
        while (s < len) {
            float phase = fmodf(P.dash, 2 * dash);
            float step = (phase < dash ? dash : 2 * dash) - phase;
            if (s + step > len) step = len - s;
            if (phase < dash)
                draw_clipped(x0 + dx * s / len, y0 + dy * s / len, x0 + dx * (s + step) / len,
                             y0 + dy * (s + step) / len);
            s += step;
            P.dash += step;
        }
    }
    pen_to(x, y);
}

void plot_move(int32_t x, int32_t y) { pen_to(x, y); }
void plot_line(int32_t x, int32_t y) { line_typed(x, y, P.type); }

void plot_origin(int32_t *x, int32_t *y) {
    *x = P.ox;
    *y = P.oy;
}

void plot_position(int32_t *x, int32_t *y) {
    *x = P.x;
    *y = P.y;
}

/* ---- state ---- */

bool plot_graph_mode(void) { return P.graph; }
uint8_t plot_csize(void) { return P.csize; }
uint8_t plot_color(void) { return P.pen; }
uint8_t plot_line_type(void) { return P.type; }
void plot_set_csize(uint8_t n) { P.csize = n < 1 ? 1 : n > 9 ? 9 : n; }
void plot_set_rotate(uint8_t r) { P.rotate = (uint8_t)(r & 3); }
void plot_set_line_type(uint8_t t) { P.type = t > 9 ? 9 : t; }
void plot_set_pen(uint8_t pen) { P.pen = (uint8_t)(pen & 3); }

static void carriage_return(void) { pen_to(0, P.y); }

void plot_text_mode(void) {
    P.graph = false;
    P.csize = 2;
    carriage_return();
    P.ox = P.x;
    P.oy = P.y;
}

void plot_graph_mode_on(void) {
    carriage_return();
    P.csize = 2;
    P.rotate = 0;
    P.ox = P.x;
    P.oy = P.y;
    P.graph = true;
}

void plot_color_command(uint8_t pen) {
    plot_set_pen(pen);
    if (!P.graph) carriage_return();
}

void plot_set_origin(void) {
    P.ox = P.x;
    P.oy = P.y;
}

/* ---- text ---- */

static int32_t pitch(void) { return 24 * P.csize; } /* 6n steps: plot_text.h */

uint8_t plot_columns(void) { return (uint8_t)((PLOT_X_MAX + PLOT_Q) / pitch()); }

uint8_t plot_column(void) { return P.x <= 0 ? 0 : (uint8_t)(P.x / pitch()); }

void plot_set_column(uint8_t col) { pen_to(col * pitch(), P.y); }

void plot_feed(int32_t lines) {
    int32_t y = P.y - lines * 48 * P.csize; /* 12n steps a line: 2.4 mm at CSIZE 1 */
    pen_to(P.x, y > ymax() ? ymax() : y);   /* the paper stops at its backing limit */
}

void plot_newline(void) {
    carriage_return();
    plot_feed(1);
}

/* The CE-150's bare LPRINT in GRAPH mode (Owner's Manual p.123): the pen
 * returns and the paper feeds, but the GRAPH counters don't change -- the
 * printer still believes the pen is where it was, so everything after is
 * drawn that much further over. As the origin moving with the pen. */
void plot_graph_newline(void) {
    const int32_t cx = P.x - P.ox, cy = P.y - P.oy;
    plot_newline();
    P.ox = P.x - cx;
    P.oy = P.y - cy;
}

static void glyph_move(void *ctx, int32_t x, int32_t y) {
    (void)ctx;
    pen_to(x, y);
}

static void glyph_draw(void *ctx, int32_t x, int32_t y) {
    (void)ctx;
    line_typed(x, y, 0);
}

void plot_char(uint8_t ch) {
    const plot_sink_t sink = {glyph_move, glyph_draw, NULL};
    const int rotate = P.graph ? P.rotate : 0;
    const int32_t x = P.x, y = P.y;
    int32_t dx, dy;
    plot_text_char(&sink, x, y, ch, P.csize, rotate);
    plot_text_advance(P.csize, rotate, &dx, &dy);
    pen_to(x + dx, y + dy);
}

/* CE-150 ROM LAB91: TEXT mode, CSIZE 2, four line feeds, then a 25-step
 * square in each pen, 40 steps apart, from the left edge; pen 0, and four
 * more line feeds. */
void plot_test(void) {
    const int32_t side = 25 * PLOT_Q;
    uint8_t type = P.type;
    plot_text_mode();
    plot_feed(4);
    P.type = 0;
    for (uint8_t i = 0; i < 4; i++) {
        int32_t x = i * 40 * PLOT_Q, y = P.y;
        P.pen = i;
        pen_to(x, y);
        line_typed(x + side, y, 0);
        line_typed(x + side, y + side, 0);
        line_typed(x, y + side, 0);
        line_typed(x, y, 0);
    }
    P.type = type;
    P.pen = 0;
    carriage_return();
    plot_feed(4);
}

/* ---- the payload ---- */

/* An operation's size, or 0 for an unknown one. */
static uint16_t op_size(uint8_t code) {
    switch (code) {
        case PLOT_OP_MOVE:
        case PLOT_OP_DRAW: return 7;
        case PLOT_OP_MOVE_REL:
        case PLOT_OP_DRAW_REL: return 3;
        case PLOT_OP_PEN: return 2;
        default: return 0;
    }
}

/* Applies the operation at p to the pen; true if it draws. */
static bool apply(const uint8_t *p, uint8_t *pen, int32_t *x, int32_t *y) {
    switch (p[0]) {
        case PLOT_OP_MOVE:
        case PLOT_OP_DRAW:
            *x = get16(p + 1);
            *y = get32(p + 3);
            return p[0] == PLOT_OP_DRAW;
        case PLOT_OP_MOVE_REL:
        case PLOT_OP_DRAW_REL:
            *x += (int8_t)p[1];
            *y += (int8_t)p[2];
            return p[0] == PLOT_OP_DRAW_REL;
        default:
            *pen = p[1];
            return false;
    }
}

void plot_split_start(plot_split_t *s, const uint8_t *payload, uint16_t len) {
    s->at = len < PLOT_PREFIX ? len : PLOT_PREFIX;
    s->pen = 0;
    s->x = s->y = 0;
    if (len >= PLOT_PREFIX) {
        s->pen = payload[0];
        s->x = get16(payload + 1);
        s->y = get32(payload + 3);
    } else {
        s->at = 0xFFFF; /* nothing to send */
    }
}

uint16_t plot_split_next(plot_split_t *s, const uint8_t *payload, uint16_t len, uint8_t *out, uint16_t max) {
    uint16_t n = PLOT_PREFIX;
    if (s->at == 0xFFFF || max < PLOT_PREFIX + 7) return 0;
    out[0] = s->pen;
    put16(out + 1, s->x);
    put32(out + 3, s->y);
    while (s->at < len) {
        uint16_t size = op_size(payload[s->at]);
        if (size == 0 || s->at + size > len) {
            s->at = len; /* malformed: the rest is dropped */
            break;
        }
        if (n + size > max) break;
        memcpy(out + n, payload + s->at, size);
        apply(payload + s->at, &s->pen, &s->x, &s->y);
        n = (uint16_t)(n + size);
        s->at = (uint16_t)(s->at + size);
    }
    if (s->at >= len) s->at = 0xFFFF; /* that was the last frame */
    return n;
}

bool plot_decode(const uint8_t *payload, uint16_t len, plot_pen_t *pen, plot_line_fn line, void *ctx) {
    uint16_t at = PLOT_PREFIX;
    if (len < PLOT_PREFIX) return false;
    pen->pen = payload[0];
    pen->x = get16(payload + 1);
    pen->y = get32(payload + 3);
    while (at < len) {
        uint16_t size = op_size(payload[at]);
        int32_t x = pen->x, y = pen->y;
        if (size == 0 || at + size > len) return false;
        if (apply(payload + at, &pen->pen, &x, &y) && line) line(ctx, pen->pen, pen->x, pen->y, x, y);
        pen->x = x;
        pen->y = y;
        at = (uint16_t)(at + size);
    }
    return true;
}
