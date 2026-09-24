/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* mcu_log.h -- small, durable (flash-backed) rolling log of internal MCU
 * failures, for the MLOG VIEW / MLOG INFO ON/OFF / MLOG CLEAR BASIC keywords
 * (rom.asm's MLOG_ROUTINE). Intended for things a user can't otherwise see
 * any evidence of -- e.g. a GreenPAK virtual-input readback that didn't
 * match what was just written (see greenpak_virtual_io.c's own retry
 * comment) -- not a high-frequency data logger. See mcu_log.c's own top
 * comment for the storage design and why its "erase + rewrite the whole
 * thing on every call" approach is fine for that usage pattern but would
 * NOT be for a busier one.
 *
 * WARN/ERROR are always recorded. INFO is gated on mcu_log_set_info_enabled()
 * (persisted in flash, default OFF per the board owner) -- meant for
 * deliberately turning on more verbose tracing while chasing something
 * specific, then back off again, rather than always running.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Longest message text actually stored (truncated beyond this) -- sized
 * to fit next to a short level prefix ("E: "/"W: "/"I: ") within the
 * PC-1500 display's own 26-character line width (see rom.asm's
 * SD_LIST_LINE_WIDTH, reused verbatim for MLOG VIEW). */
#define MCU_LOG_MSG_MAX 23

/* Rolling cap -- oldest entry is dropped once this many are stored.
 * "maybe 50, configurable" per the board owner; this is that knob. */
#define MCU_LOG_MAX_ENTRIES 50

typedef enum {
    MCU_LOG_LEVEL_INFO = 1,
    MCU_LOG_LEVEL_WARN = 2,
    MCU_LOG_LEVEL_ERROR = 3,
} mcu_log_level_t;

/* Reads the flash-backed log header into RAM (or initializes a fresh one
 * if the reserved flash region has never been written -- reads as all-0xFF).
 * Call once at boot, before any mcu_log_*() call. Cheap (no flash write). */
void mcu_log_init(void);

/* Always recorded, regardless of mcu_log_set_info_enabled(). `msg` is a
 * plain C string, truncated to MCU_LOG_MSG_MAX if longer. */
void mcu_log_error(const char *msg);
void mcu_log_warn(const char *msg);

/* Recorded only while mcu_log_set_info_enabled(true) is in effect --
 * otherwise a cheap no-op (no flash write at all). */
void mcu_log_info(const char *msg);

/* Persists to flash immediately. */
void mcu_log_set_info_enabled(bool enabled);
bool mcu_log_get_info_enabled(void);

/* Erases all entries (does NOT change the info-enabled setting). */
void mcu_log_clear(void);

/* How many entries currently exist, 0..MCU_LOG_MAX_ENTRIES. */
uint8_t mcu_log_get_count(void);

/* Fetches entry `indexFromNewest` (0 = most recently logged) into
 * *levelOut/msgOut (a plain, NUL-terminated C string, buffer must be at
 * least MCU_LOG_MSG_MAX+1 bytes). Returns false if indexFromNewest is out
 * of range (>= mcu_log_get_count()). */
bool mcu_log_get_entry(uint8_t indexFromNewest, uint8_t *levelOut, char *msgOut);
