/* hostlink.c -- see hostlink.h. */
#include "hostlink.h"

#include <stdbool.h>
#include <string.h>

#include "module_config.h"
#include "sl_iostream.h"
#include "sl_iostream_init_usart_instances.h"

#define SOF 0xA5
#define HDR 5 /* SOF type seq len16 */

static uint8_t rx[HDR + FRAME_MAX_PAYLOAD + 2];
static size_t rx_len;

/* The last answer, resent as-is for a repeated request (sec.5). */
static uint8_t last[HDR + FRAME_MAX_PAYLOAD + 2];
static size_t last_len;
static bool have_last;

static uint16_t crc16(const uint8_t *p, size_t n) {
    uint16_t c = 0xFFFF; /* CRC-16/CCITT-FALSE */
    while (n--) {
        c ^= (uint16_t)(*p++ << 8);
        for (int i = 0; i < 8; i++) {
            c = (uint16_t)((c & 0x8000) ? (c << 1) ^ 0x1021 : c << 1);
        }
    }
    return c;
}

void hostlink_init(void) {
    rx_len = 0;
    have_last = false;
}

static void send(const uint8_t *p, size_t n) {
    sl_iostream_write(sl_iostream_host_handle, p, n);
}

static void answer(uint8_t type, uint8_t seq, const uint8_t *payload, size_t len) {
    last[0] = SOF;
    last[1] = (uint8_t)(type | 0x80);
    last[2] = seq;
    last[3] = (uint8_t)len;
    last[4] = (uint8_t)(len >> 8);
    memcpy(last + HDR, payload, len);
    uint16_t c = crc16(last + 1, HDR - 1 + len);
    last[HDR + len] = (uint8_t)c;
    last[HDR + len + 1] = (uint8_t)(c >> 8);
    last_len = HDR + len + 2;
    have_last = true;
    send(last, last_len);
}

static void handle_frame(void) {
    uint8_t type = rx[1], seq = rx[2];
    size_t len = rx[3] | (size_t)rx[4] << 8;
    if (have_last && last[2] == seq && (last[1] & 0x7F) == type) {
        send(last, last_len); /* a retry: same answer, nothing done twice */
        return;
    }
    static uint8_t out[FRAME_MAX_PAYLOAD];
    size_t out_len = 0;
    out[0] = hostlink_handle(type, rx + HDR, len, out + 1, &out_len);
    answer(type, seq, out, out_len + 1);
}

/* Drop bytes up to the next SOF after a bad frame (sec.5). */
static void resync(size_t from) {
    size_t i = from;
    while (i < rx_len && rx[i] != SOF) {
        i++;
    }
    memmove(rx, rx + i, rx_len - i);
    rx_len -= i;
}

void hostlink_poll(void) {
    for (;;) {
        size_t got = 0;
        if (rx_len < sizeof rx) {
            sl_iostream_read(sl_iostream_host_handle, rx + rx_len, sizeof rx - rx_len, &got);
        }
        rx_len += got;
        if (rx_len && rx[0] != SOF) {
            resync(0);
        }
        if (rx_len < HDR) {
            return;
        }
        size_t len = rx[3] | (size_t)rx[4] << 8;
        if (len > FRAME_MAX_PAYLOAD) {
            resync(1);
            continue;
        }
        if (rx_len < HDR + len + 2) {
            if (got == 0) {
                return; /* wait for the rest */
            }
            continue;
        }
        uint16_t c = rx[HDR + len] | (uint16_t)(rx[HDR + len + 1] << 8);
        if (c != crc16(rx + 1, HDR - 1 + len) || (rx[1] & 0x80)) {
            resync(1); /* the MCU resends after 50 ms */
            continue;
        }
        handle_frame();
        size_t used = HDR + len + 2;
        memmove(rx, rx + used, rx_len - used);
        rx_len -= used;
    }
}
