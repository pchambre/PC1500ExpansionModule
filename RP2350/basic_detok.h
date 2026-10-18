/* basic_detok.h -- one line of tokenized BASIC back to text (2026-10-17),
 * for the text watch (text_snoop.h): BASIC tokenizes the line it keeps at
 * 7BB0H in place on ENTER, and LIST copies program lines there still
 * tokenized -- the ROM only turns tokens into text as LCD dots, never as
 * characters in RAM.
 *
 * Keyword names: BASIC's and the CE-150's/peripherals' from the TRM sec.5-2
 * table (the same as pc1500emu's src/basic/basic_tokens.cpp); E1xx -- this
 * module's own keywords -- from a lookup the caller supplies (text_snoop.c
 * reads them out of the module ROM's keyword table). Spacing is pc1500emu's
 * detokenizer's convention, not the ROM's LIST: one space around each
 * keyword unless it would sit next to ( ) , ; : or a quote.
 *
 * Portable C, no Pico SDK. */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* Name for a code BASIC's table doesn't have (the module's E1xx), or NULL. */
typedef const char *(*basic_detok_extra_fn)(uint16_t code);

/* The name of a built-in code (F0xx/F1xx/E6xx/E7xx/E8xx), or NULL. */
const char *basic_detok_builtin(uint16_t code);

/* src[0..len) up to the first 0DH (or len) -> text in out, always
 * NUL-terminated, cut short if out is too small. ASCII passes through, so
 * a line still being typed comes out as typed. Returns the text's length. */
size_t basic_detok_line(const uint8_t *src, size_t len, char *out, size_t outsz, basic_detok_extra_fn extra);
