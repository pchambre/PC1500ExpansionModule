/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* net_time.h -- the time from the Internet (2026-10-08): one SNTP query
 * (RFC 4330) to pool.ntp.org, for WFCON setting the PC-1500's clock
 * (time_sync.h). No background polling: the clock is set once, when Wi-Fi
 * connects.
 *
 * Threading, as net_ping.h: a UDP pcb on core0 whose callback notes the
 * answer; the query and the wait on core1. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* core1, with Wi-Fi connected: UTC now, in Unix milliseconds. False
 * (logged) if no server answers within a few seconds. */
bool net_time_get(int64_t *utc_ms);
