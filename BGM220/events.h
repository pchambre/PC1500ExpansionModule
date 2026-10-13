/* events.h -- the event queue behind POLL and HOST_WAKE
 * (MODULE_PROTOCOL.md sec.4, sec.7). */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    EV_NONE = 0x00,
    EV_SCAN_REPORT = 0x01,
    EV_SCAN_DONE = 0x02,
    EV_LINK_UP = 0x03,
    EV_LINK_FAILED = 0x04,
    EV_LINK_DOWN = 0x05,
    EV_LINK_RX = 0x06,
    EV_KBD_STATE = 0x10,
    EV_KBD_PASSKEY = 0x11,
    EV_KBD_REPORT = 0x12,
    EV_LOG = 0x7F,
};

void events_init(void);
/* Queue an event; raises HOST_WAKE. False if the queue was full (the event
 * is dropped and a LOG saying so takes the reserved slot). */
bool events_push(uint8_t type, const uint8_t *data, size_t len);
/* A LOG event with printf formatting: level 1 (info, which the MCU keeps
 * only with MLOG VERBOSE) or 2 (error, always kept). */
void events_log(const char *fmt, ...);
void events_log_error(const char *fmt, ...);
/* The oldest event into `out` (type, then payload); its length, or 1 with
 * EV_NONE when empty. Lowers HOST_WAKE when that empties the queue. */
size_t events_pop(uint8_t *out, size_t max);
