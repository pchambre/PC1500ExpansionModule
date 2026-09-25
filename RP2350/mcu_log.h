/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* mcu_log.h -- small, durable (flash-backed) rolling log of internal MCU
 * failures, for the MLOG VIEW / MLOG INFO ON/OFF / MLOG CLEAR BASIC keywords
 * (rom.asm's MLOG_ROUTINE). Intended for things a user can't otherwise see
 * any evidence of -- e.g. a GreenPAK virtual-input readback that didn't
 * match what was just written (see greenpak_virtual_io.c's own retry
 * comment) -- not a high-frequency data logger. See mcu_log.c's own top
 * comment for the storage design: an append-only ring of flash sectors,
 * sized by MCONF LOGSIZE (default 100KB), whose oldest entries are dropped a
 * sector (127 entries) at a time once it's full.
 *
 * WARN/ERROR are always recorded. INFO is gated on mcu_log_set_info_enabled()
 * (persisted with the MCONF settings, default OFF per the board owner) -- meant for
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

/* The smallest log MCONF LOGSIZE may set (KB): two sectors, so dropping
 * the oldest one never empties the log. */
#define MCU_LOG_MIN_SIZE_KB 8

typedef enum {
    MCU_LOG_LEVEL_INFO = 1,
    MCU_LOG_LEVEL_WARN = 2,
    MCU_LOG_LEVEL_ERROR = 3,
    MCU_LOG_LEVEL_USER = 4, /* MLOG "text" from BASIC (2026-09-24) -- shown as "U:" */
} mcu_log_level_t;

/* Finds the log in flash (a fresh, empty one if there's none yet). Call
 * once at boot, after mcu_config_init() and before any mcu_log_*() call.
 * Cheap (no flash write). */
void mcu_log_init(void);

/* The largest LOGSIZE (KB) that fits between the firmware image (plus room
 * for it to grow) and the settings sector. */
uint16_t mcu_log_max_size_kb(void);

/* Call after MCONF changes LOGSIZE: starts a fresh log of the new size (the
 * old entries are dropped). */
void mcu_log_resize(void);

/* Always recorded, regardless of mcu_log_set_info_enabled(). `msg` is a
 * plain C string, truncated to MCU_LOG_MSG_MAX if longer. */
void mcu_log_error(const char *msg);
void mcu_log_warn(const char *msg);

/* A note typed by the user (MLOG "text") -- always recorded, like
 * WARN/ERROR, since it's an explicit request rather than tracing. */
void mcu_log_user(const char *msg);

/* Recorded only while mcu_log_set_info_enabled(true) is in effect --
 * otherwise a cheap no-op (no flash write at all). */
void mcu_log_info(const char *msg);

/* Persists to flash immediately (an MCONF setting, MCU_CONFIG_LOGINFO). */
void mcu_log_set_info_enabled(bool enabled);
bool mcu_log_get_info_enabled(void);

/* Empties the log (does NOT change the info-enabled setting). Writes a
 * marker rather than erasing, so it's quick at any LOGSIZE. */
void mcu_log_clear(void);

/* How many entries currently exist. Walks the log -- cheap XIP reads, and
 * a full log is a few thousand entries at the default size (127 per 4K). */
uint32_t mcu_log_get_count(void);

/* Fetches entry `indexFromNewest` (0 = most recently logged) into
 * *levelOut/msgOut (a plain, NUL-terminated C string, buffer must be at
 * least MCU_LOG_MSG_MAX+1 bytes). Returns false if indexFromNewest is out
 * of range (>= mcu_log_get_count()). */
bool mcu_log_get_entry(uint32_t indexFromNewest, uint8_t *levelOut, char *msgOut);
