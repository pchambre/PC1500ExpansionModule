/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* debug_fwd.h -- MCONF BLDEBUG (2026-11-14): the module's serial debug
 * output and its MCU log's lines, to the linked app's Debug pane
 * (BLE_PROTOCOL.md "Debug text").
 *
 * A stdio driver copies everything printed, from either core, into a 16 KB
 * ring while BLDEBUG=1; mcu_log.c adds each line it records ("MLOG E: ...").
 * core1 sends the ring between commands (debug_fwd_poll()), as the screen
 * mirror does, so a keyword is never held up by it. What doesn't fit is
 * dropped and counted, and a "[n bytes dropped]" line says so. Nothing is
 * kept while BLDEBUG=0. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The stdio driver; core0, after stdio_init_all(). */
void debug_fwd_init(void);

/* An MCU log line ("E", "W", "I", "U"), from mcu_log.c. */
void debug_fwd_mlog(char level, const char *msg);

/* core1, between commands: the ring to the peer, one frame at a time.
 * True if it sent one (call again). */
bool debug_fwd_poll(void);

/* There is text waiting for a link that takes it: don't WFE without a
 * timeout. */
bool debug_fwd_pending(void);
