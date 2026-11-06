/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* time_sync.h -- setting the PC-1500's clock (2026-10-08). WFCON and BLCON
 * (keywords.c) ask for the time once they're connected, and the ROM sets
 * the uPD1990AC with it (EXP_KW_ACTION_SETTIME):
 *
 *   Wi-Fi: UTC from SNTP (net_time.h), in MCONF TZ's local time
 *          (time_zone.h);
 *   BLE:   the host app's own local time (ble_link_host_time()) -- only
 *          from the app, never from another PC-1500.
 *
 * MCONF TIMESYNC=0 turns it off. A failure only means the clock is left as
 * it was: logged, never an error to BASIC. */
#pragma once

#include <stdint.h>

/* core1: EXP_COMMAND_TIME_GET, EXP_COMMAND_CONFIG_TZ_GET/SET (pc_exp.h).
 * Returns the status. */
uint8_t time_sync_command(uint8_t command, uint8_t *window);
