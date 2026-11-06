/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* time_sync.c -- see time_sync.h. */
#include "time_sync.h"

#include <stdio.h>
#include <string.h>

#include "ble_link.h"
#include "mcu_config.h"
#include "mcu_log.h"
#include "net_time.h"
#include "pc_exp.h"
#include "time_zone.h"

/* The clock's bytes for local time `local_ms`, rounded to the second. */
static void clock_bytes(int64_t local_ms, uint8_t *w, const char *source) {
    char msg[MCU_LOG_MSG_MAX + 1];
    int64_t local = local_ms >= 0 ? (local_ms + 500) / 1000 : -((-local_ms + 500) / 1000);
    tz_clock_bytes(local, w);
    snprintf(msg, sizeof msg, "TIME %u/%02X %02X:%02X:%02X %s", (unsigned)(w[0] >> 4), w[1], w[2], w[3], w[4], source);
    mcu_log_info(msg);
}

static uint8_t time_get(uint8_t *w) {
    int64_t utc_ms;
    int32_t offset;
    if (mcu_config_get(MCU_CONFIG_TIMESYNC) == 0) return EXP_STATUS_ERROR;
    if (w[0] == EXP_TIME_SOURCE_BLE) {
        if (!ble_link_host_time(&utc_ms, &offset)) return EXP_STATUS_ERROR;
        clock_bytes(utc_ms + (int64_t)offset * 1000, w, "BL");
        return EXP_STATUS_SUCCESS;
    }
    if (w[0] == EXP_TIME_SOURCE_WIFI) {
        const char *tz = mcu_config_get_tz();
        char canonical[TZ_NAME_MAX + 1];
        tz_zone_t zone;
        if (!tz_find(tz, (uint8_t)strlen(tz), canonical, &zone)) { /* a table that lost the name since */
            mcu_log_warn("TIME: MCONF TZ unknown");
            return EXP_STATUS_ERROR;
        }
        if (!net_time_get(&utc_ms)) return EXP_STATUS_ERROR;
        offset = tz_offset_at(&zone, utc_ms / 1000);
        clock_bytes(utc_ms + (int64_t)offset * 1000, w, "WF");
        return EXP_STATUS_SUCCESS;
    }
    return EXP_STATUS_ERROR;
}

uint8_t time_sync_command(uint8_t command, uint8_t *w) {
    switch (command) {
        case EXP_COMMAND_TIME_GET:
            return time_get(w);
        case EXP_COMMAND_CONFIG_TZ_GET: {
            const char *tz = mcu_config_get_tz();
            w[0] = (uint8_t)strlen(tz);
            memcpy(w + 1, tz, w[0]);
            return EXP_STATUS_SUCCESS;
        }
        case EXP_COMMAND_CONFIG_TZ_SET: {
            /* stored as the table spells it, or the rule as typed */
            char canonical[TZ_NAME_MAX + 1];
            tz_zone_t zone;
            if (w[0] > TZ_NAME_MAX || !tz_find((const char *)w + 1, w[0], canonical, &zone)) return EXP_STATUS_ERROR;
            return mcu_config_set_tz(canonical, (uint8_t)strlen(canonical)) ? EXP_STATUS_SUCCESS : EXP_STATUS_ERROR;
        }
        default:
            return EXP_STATUS_NOT_IMPLEMENTED;
    }
}
