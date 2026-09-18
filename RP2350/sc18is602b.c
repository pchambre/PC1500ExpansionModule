/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
#include "sc18is602b.h"

#include <stdio.h>
#include <string.h>

#include "pico/time.h"

#define SC18IS602B_FUNC_CONFIGURE_SPI 0xF0
#define SC18IS602B_SS0 0x01
#define SC18IS602B_SS1 0x02  /* unconnected on this board -- see sc18is602b_clock_only() */

/* Conservative margin under the chip's real 200-byte buffer -- leaves
 * room for the Function ID byte itself plus slack, no need to cut it
 * exactly to 199. */
#define SC18IS602B_MAX_CHUNK 128

/* "When SC18IS602B is busy after the address byte is transmitted, it
 * will not acknowledge its address" (datasheet section 7.1.1) -- it's
 * still physically clocking out the PREVIOUS chunk's bytes on the real
 * SPI bus (up to ~17.7ms for a full 128-byte chunk at the slowest
 * 58kHz init-time clock) when a new I2C transaction's address byte
 * arrives. Confirmed live 2026-09-17: a 22-byte transfer's write and/or
 * read-back phase would intermittently NACK (alternating between the
 * two across retries) with no delay here at all -- short transfers
 * (a handful of bytes, no read-back) never hit this, longer ones did.
 * Retry the address-ACK itself rather than just failing outright. */
#define SC18IS602B_BUSY_RETRY_COUNT 50
#define SC18IS602B_BUSY_RETRY_DELAY_US 500

bool sc18is602b_configure(const greenpak_i2c_bus_t *bus, uint8_t mode, uint8_t clock_rate) {
    uint8_t payload[2] = { SC18IS602B_FUNC_CONFIGURE_SPI, (uint8_t)(mode | clock_rate) };
    return greenpak_i2c_write(bus, SC18IS602B_I2C_ADDR, payload, sizeof(payload));
}

static bool sc18is602b_write_retry(const greenpak_i2c_bus_t *bus, const uint8_t *buf, uint32_t len) {
    for (int attempt = 0; attempt < SC18IS602B_BUSY_RETRY_COUNT; attempt++) {
        if (greenpak_i2c_write(bus, SC18IS602B_I2C_ADDR, buf, len)) return true;
        sleep_us(SC18IS602B_BUSY_RETRY_DELAY_US);
    }
    return false;
}

static bool sc18is602b_read_retry(const greenpak_i2c_bus_t *bus, uint8_t *buf, uint32_t len) {
    for (int attempt = 0; attempt < SC18IS602B_BUSY_RETRY_COUNT; attempt++) {
        if (greenpak_i2c_read(bus, SC18IS602B_I2C_ADDR, buf, len)) return true;
        sleep_us(SC18IS602B_BUSY_RETRY_DELAY_US);
    }
    return false;
}

static bool sc18is602b_transfer_ss(const greenpak_i2c_bus_t *bus, uint8_t ss_select,
                                    const uint8_t *tx, uint8_t *rx, uint32_t len) {
    uint8_t chunk[1 + SC18IS602B_MAX_CHUNK];

    for (uint32_t offset = 0; offset < len;) {
        uint32_t n = len - offset;
        if (n > SC18IS602B_MAX_CHUNK) n = SC18IS602B_MAX_CHUNK;

        chunk[0] = ss_select;
        if (tx) {
            memcpy(&chunk[1], tx + offset, n);
        } else {
            memset(&chunk[1], 0xFF, n);
        }
        if (!sc18is602b_write_retry(bus, chunk, 1 + n)) {
            printf("    [sc18is602b] WRITE FAILED after retries (ss=0x%02X, n=%lu)\n", ss_select, (unsigned long)n);
            return false;
        }

        if (rx && !sc18is602b_read_retry(bus, rx + offset, n)) {
            printf("    [sc18is602b] READ-BACK FAILED after retries (ss=0x%02X, n=%lu)\n", ss_select, (unsigned long)n);
            return false;
        }

        offset += n;
    }
    return true;
}

bool sc18is602b_transfer(const greenpak_i2c_bus_t *bus, const uint8_t *tx, uint8_t *rx, uint32_t len) {
    return sc18is602b_transfer_ss(bus, SC18IS602B_SS0, tx, rx, len);
}

bool sc18is602b_clock_only(const greenpak_i2c_bus_t *bus, uint32_t len) {
    return sc18is602b_transfer_ss(bus, SC18IS602B_SS1, NULL, NULL, len);
}
