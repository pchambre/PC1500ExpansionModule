/* keywords.h
 *
 * MCU side of the expansion ROM's keyword executor (2026-09-25). The ROM
 * hands every keyword's raw argument text to the MCU (EXP_COMMAND_KEYWORD)
 * and then just carries out the actions it's given back -- see pc_exp.h's
 * EXP_KW_* block for the wire format and rom.asm's KW_START for the other
 * end. All the argument parsing and command sequencing that used to be
 * LH5801 code lives in keywords.c instead.
 *
 * Portable C with no Pico SDK dependencies: pc1500emu's ExpansionMock
 * compiles this same file, so the emulator's keyword tests exercise the
 * real parser rather than a copy.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Runs one ordinary EXP_COMMAND_* against the data window and returns its
 * final EXP_STATUS_* -- the firmware's DoCommand(), the emulator's
 * dispatchCommand(). Must not publish that status to the LH5801: the
 * keyword command itself is still in progress. */
typedef uint8_t (*kw_command_fn)(uint8_t command, void *ctx);

/* Handles EXP_COMMAND_KEYWORD and EXP_COMMAND_KEYWORD_CONTINUE. `window`
 * is the 2K data window (offset 0 = 0x8000). Returns the status to publish:
 * EXP_STATUS_SUCCESS with an action block written, or EXP_STATUS_ERROR for
 * an unknown keyword id or a CONTINUE with nothing to continue. */
uint8_t kw_command(uint8_t command, uint8_t *window, kw_command_fn run, void *ctx);

#ifdef __cplusplus
}
#endif
