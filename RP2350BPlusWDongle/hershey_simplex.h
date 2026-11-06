/* hershey_simplex.h -- the Hershey Roman Simplex stroke font, for the CE-150
 * plotter emulation's lettering (2026-09-30). Generated data: see
 * hershey_simplex.c and fonts/jhf_to_c.py; licence and acknowledgements in
 * fonts/NOTICE-hershey.txt.
 *
 * Portable C, no Pico SDK: pc1500emu compiles it too. */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HERSHEY_FIRST 0x20 /* printable ASCII 0x20-0x7E, and a degree sign at 0x7F */
#define HERSHEY_COUNT 96
#define HERSHEY_PEN_UP (-128) /* an x of this lifts the pen before the next point */

/* One glyph: its left and right extent (the advance is right - left), and
 * `count` points at kHersheyPoints[2 * offset], as (x, y) pairs in Hershey
 * units -- x to the right, y DOWN; capitals span about y = -12..+9, the
 * baseline is +9. The pen goes down at the first point after a pen-up
 * (and at the very first point). */
typedef struct {
    int8_t left, right;
    uint8_t count;
    uint16_t offset;
} hershey_glyph_t;

extern const hershey_glyph_t kHersheyGlyphs[HERSHEY_COUNT];
extern const int8_t kHersheyPoints[];

#ifdef __cplusplus
}
#endif
