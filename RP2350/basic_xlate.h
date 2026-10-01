/* basic_xlate.h -- the CE-150's seven E6xx keyword codes in a BASIC program
 * being saved or loaded (2026-10-01).
 *
 * This module's stand-ins for CSIZE, GRAPH, GLCURSOR, LCURSOR, SORGN, ROTATE
 * and TEXT have their own codes, E1C0-E1C6: BASIC looks for an E6xx code
 * only on the CE-150's own page, so a program typed here holds E1Cx where
 * one typed with a CE-150 attached holds E68x. So that a file means the
 * same with or without a CE-150:
 * - a save always writes the CE-150's codes (E1C0-E1C6 -> E680-E686);
 * - a load turns them into this module's (E680-E686 -> E1C0-E1C6) only
 *   when no CE-150 is attached -- with one, its own codes work as they are.
 *
 * keywords.c turns it on for the BASIC program's SAVE or LOAD action (not
 * for M files, STSAVE, or a file copied between the card and a peer); the
 * command dispatcher then passes the bytes of WRITE_TO_SD_FILE and
 * READ_FROM_SD_FILE through it, card or BLE alike, and CLOSE_SD_FILE ends
 * it. Only real tokens change: it follows the program's line structure
 * ([number hi][lo][length][statements, ending in CR], FF after the last;
 * TRM sec.5-3-5) and skips quoted text, across chunk boundaries -- a token
 * split between two chunks is held back to the next.
 *
 * Portable C, no Pico SDK: pc1500emu compiles it too. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { BASIC_XLATE_OFF, BASIC_XLATE_SAVE, BASIC_XLATE_LOAD };

void basic_xlate_begin(uint8_t mode);
void basic_xlate_end(void);
uint8_t basic_xlate_mode(void);

/* A chunk about to be written, translated in place; returns how many bytes
 * to write now (it may be one more or one fewer than `len`: `data` needs
 * room for len + 1). */
uint16_t basic_xlate_write(uint8_t *data, uint16_t len);

/* At the close of a save: a byte still held back (a malformed program's
 * last), into data[0]; returns 0 or 1. */
uint16_t basic_xlate_flush(uint8_t *data);

/* A chunk just read, translated in place; returns how many bytes to hand
 * on (one more or one fewer than `len`, never 0 for a `len` above 0: room
 * for len + 1). */
uint16_t basic_xlate_read(uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif
