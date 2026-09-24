/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
#include "sc18is602b.h"

#include <stdio.h>
#include <string.h>

#include "pico/time.h"
#include "board_pins.h"

#ifdef PIN_SD_BRIDGE_INT
#include "hardware/gpio.h"
#endif

#define SC18IS602B_FUNC_CONFIGURE_SPI 0xF0
#define SC18IS602B_FUNC_CLEAR_INTERRUPT 0xF1

/* "When SC18IS602B is busy after the address byte is transmitted, it
 * will not acknowledge its address" (datasheet section 7.1.1) -- it's
 * still physically clocking out the PREVIOUS chunk's bytes on the real
 * SPI bus when a new I2C transaction's address byte arrives. Confirmed
 * live 2026-09-17: a 22-byte transfer's write and/or read-back phase
 * would intermittently NACK (alternating between the two across
 * retries) with no delay here at all -- short transfers (a handful of
 * bytes, no read-back) never hit this, longer ones did. Retry the
 * address-ACK itself rather than just failing outright.
 *
 * Delay reduced 500->100 (2026-09-20): the original 500 was sized
 * against the 58kHz *init-time* clock's own worst case (~17.7ms for a
 * full 128-byte chunk) -- stale reasoning once bulk transfers moved to
 * the 1.8432MHz clock and 199-byte chunks (SC18IS602B_MAX_CHUNK,
 * sc18is602b.h), whose real worst-case busy window is only ~864us
 * (199*8 bits / 1.8432MHz). A real-hardware retry-count diagnostic
 * (temporary counters in sc18is602b_write_retry()/read_retry(), see
 * sc18is602b_get_and_reset_retry_stats()) found a 32KB transfer needing
 * roughly one retry per call on average -- meaning most retries were
 * paying the full 500us even though the actual busy window they were
 * waiting out is nowhere near that long, wasting real throughput. 100us
 * still comfortably covers the true ~864us worst case within a handful
 * of retries (well under SC18IS602B_BUSY_RETRY_COUNT), while costing
 * ~5x less per retry in the common case. */
#define SC18IS602B_BUSY_RETRY_COUNT 50
#define SC18IS602B_BUSY_RETRY_DELAY_US 100

#ifdef PIN_SD_BRIDGE_INT
/* U5's INT pin (active LOW, open-drain -- datasheet section 6.2)
 * asserts automatically whenever an SPI transmission completes. This
 * lets us know the bridge is ready for the next I2C transaction with a
 * plain GPIO read instead of guessing with I2C busy-retries -- see
 * sc18is602b_transfer_ss() below, which waits on it after every SPI
 * transmission (not just ones followed by a read-back).
 *
 * THE REAL CLEARING MECHANISM (root-caused 2026-09-21, via direct
 * hardware isolation, not just reading the datasheet): **reading the
 * captured buffer is what deasserts INT, not the Clear Interrupt command
 * (Function ID 0xF1)**. Confirmed live: a write-only transaction left
 * INT asserted (LOW) even after sc18is602b_clear_interrupt() below was
 * sent; a bare I2C read of the buffer -- no new SPI write, no Clear
 * Interrupt at all -- flipped it to idle (HIGH) immediately on its own.
 * The datasheet's own text (section 8, page 13) frames Clear Interrupt
 * as the reset mechanism and a read as separate/optional, and its own
 * worked example never isolates a write-only-then-clear-with-no-read
 * case, which is why an earlier round of this investigation (working
 * from that text, not this direct isolation) got this backwards: it
 * correctly found that a write-only transfer needs ITS OWN completion
 * consumed somehow before the next wait_int_ready() call (previously
 * miscategorized as "you must call Clear Interrupt for write-only
 * transfers too"), but the actual fix that matters is the drain read
 * below, not the 0xF1 call. sc18is602b_clear_interrupt() is still sent
 * (harmless, matches the datasheet's own documented usage), but is not
 * what's relied on for correctness here. */
#define SC18IS602B_INT_TIMEOUT_US 5000u

static bool g_intPinInitialized = false;

static void sc18is602b_init_int_pin(void) {
    if (g_intPinInitialized) return;
    gpio_init(PIN_SD_BRIDGE_INT);
    gpio_set_dir(PIN_SD_BRIDGE_INT, GPIO_IN);
    gpio_pull_up(PIN_SD_BRIDGE_INT);
    g_intPinInitialized = true;
}

/* TEMPORARY DIAGNOSTIC (2026-09-20): distinguishes "INT already low when
 * checked" (expected common case if INT tracks real bridge busy state)
 * from "had to actually wait" from "timed out" -- observed throughput
 * regression after enabling this path needs to know which of these
 * dominates before it can be explained. */
static uint32_t g_intImmediateCount = 0;
static uint32_t g_intWaitedCount = 0;
static uint32_t g_intTimeoutCount = 0;

void sc18is602b_get_and_reset_int_stats(uint32_t *outImmediate, uint32_t *outWaited, uint32_t *outTimeout) {
    *outImmediate = g_intImmediateCount;
    *outWaited = g_intWaitedCount;
    *outTimeout = g_intTimeoutCount;
    g_intImmediateCount = 0;
    g_intWaitedCount = 0;
    g_intTimeoutCount = 0;
}

/* Blocks until INT reads LOW (bridge ready) or the timeout elapses.
 * Always returns true if INT is already low when called -- this is a
 * genuine wait, not a fixed delay, so it resolves as fast as the
 * hardware actually allows instead of guessing. */
static bool sc18is602b_wait_int_ready(void) {
    if (!gpio_get(PIN_SD_BRIDGE_INT)) {
        g_intImmediateCount++;
        return true;
    }
    uint32_t start = time_us_32();
    while (gpio_get(PIN_SD_BRIDGE_INT)) {
        if (time_us_32() - start > SC18IS602B_INT_TIMEOUT_US) {
            g_intTimeoutCount++;
            return false;
        }
    }
    g_intWaitedCount++;
    return true;
}

/* Function ID 0xF1, no data byte -- must be sent after every transmission
 * whose completion we detected via INT (whether or not a read-back
 * followed), or INT stays latched low and every subsequent poll sees
 * stale state instead of that transfer's own completion. Datasheet
 * page 13. */
static void sc18is602b_clear_interrupt(const greenpak_i2c_bus_t *bus) {
    uint8_t cmd = SC18IS602B_FUNC_CLEAR_INTERRUPT;
    greenpak_i2c_write(bus, SC18IS602B_I2C_ADDR, &cmd, 1);
}
#else
void sc18is602b_get_and_reset_int_stats(uint32_t *outImmediate, uint32_t *outWaited, uint32_t *outTimeout) {
    *outImmediate = 0;
    *outWaited = 0;
    *outTimeout = 0;
}
#endif

/* Forward-declared -- defined below, reused here rather than duplicated.
 * See its own definition for the retry rationale (datasheet section
 * 7.1.1: the bridge NAKs its own address while still busy). */
static bool sc18is602b_write_retry(const greenpak_i2c_bus_t *bus, const uint8_t *buf, uint32_t len);

bool sc18is602b_configure(const greenpak_i2c_bus_t *bus, uint8_t mode, uint8_t clock_rate) {
#ifdef PIN_SD_BRIDGE_INT
    sc18is602b_init_int_pin();
#endif
    uint8_t payload[2] = { SC18IS602B_FUNC_CONFIGURE_SPI, (uint8_t)(mode | clock_rate) };
    /* Retried (2026-09-21), unlike every prior revision of this function --
     * every OTHER I2C write in this file already tolerates the bridge's
     * own documented busy-NAK behavior via sc18is602b_write_retry(), but
     * this one (the very FIRST command issued to the chip on any given
     * boot) never did. Confirmed live: this specific call, and only this
     * one, intermittently failed outright (no ACK) on a fresh boot --
     * every subsequent real transaction on the same boot worked fine every
     * time. Root cause not confirmed (this isn't a claim about WHY), but
     * the existing retry pattern already used everywhere else in this
     * file is a well-precedented, low-risk fix for "this specific I2C
     * write didn't ACK, try again" regardless of the underlying reason. */
    return sc18is602b_write_retry(bus, payload, sizeof(payload));
}

/* TEMPORARY DIAGNOSTIC (2026-09-20): counts how many retries the busy-wait
 * loops below actually need, to check whether SC18IS602B_BUSY_RETRY_DELAY_US
 * is the real throughput bottleneck (suspected after switching
 * greenpak_i2c.c from bit-banged to real hardware I2C changed measured
 * SD throughput almost not at all). Call sc18is602b_get_and_reset_retry_stats()
 * to read and clear. Remove once the real bottleneck is confirmed. */
static uint32_t g_retryCount = 0;
static uint32_t g_retryCallCount = 0;

void sc18is602b_get_and_reset_retry_stats(uint32_t *outRetries, uint32_t *outCalls) {
    *outRetries = g_retryCount;
    *outCalls = g_retryCallCount;
    g_retryCount = 0;
    g_retryCallCount = 0;
}

static bool sc18is602b_write_retry(const greenpak_i2c_bus_t *bus, const uint8_t *buf, uint32_t len) {
    g_retryCallCount++;
    /* No wait_int_ready() here (2026-09-20, removed after live data showed
     * it net-negative): a write starting a NEW SPI transfer only ever
     * follows either (a) the FIRST chunk of a transfer (no prior INT event
     * exists to wait on at all) or (b) a prior chunk's read_retry(), which
     * -- by successfully retrieving that chunk's captured MISO data --
     * already proves the bridge is idle. Waiting here means polling for a
     * fresh low-to-high-to-low INT transition that can't happen until this
     * very write is issued, i.e. it can only ever time out; live data
     * confirmed ~26% of calls burned the full 5ms timeout for exactly this
     * reason with zero benefit. */
    for (int attempt = 0; attempt < SC18IS602B_BUSY_RETRY_COUNT; attempt++) {
        if (greenpak_i2c_write(bus, SC18IS602B_I2C_ADDR, buf, len)) return true;
        g_retryCount++;
        sleep_us(SC18IS602B_BUSY_RETRY_DELAY_US);
    }
    return false;
}

static bool sc18is602b_read_retry(const greenpak_i2c_bus_t *bus, uint8_t *buf, uint32_t len) {
    g_retryCallCount++;
    for (int attempt = 0; attempt < SC18IS602B_BUSY_RETRY_COUNT; attempt++) {
        if (greenpak_i2c_read(bus, SC18IS602B_I2C_ADDR, buf, len)) return true;
        g_retryCount++;
        sleep_us(SC18IS602B_BUSY_RETRY_DELAY_US);
    }
    return false;
}

static bool sc18is602b_transfer_ss(const greenpak_i2c_bus_t *bus, uint8_t ss_select,
                                    const uint8_t *tx, uint8_t *rx, uint32_t len, uint32_t chunkSize) {
    uint8_t chunk[1 + SC18IS602B_MAX_CHUNK];

    for (uint32_t offset = 0; offset < len;) {
        uint32_t n = len - offset;
        if (n > chunkSize) n = chunkSize;

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

        /* Every SPI transmission (this write, whether or not a read-back
         * follows) raises INT on completion -- wait for it before doing
         * anything that assumes the bridge is idle again. See this file's
         * own INT section comment for the real clearing mechanism. */
#ifdef PIN_SD_BRIDGE_INT
        sc18is602b_wait_int_ready();
#endif

        if (rx) {
            if (!sc18is602b_read_retry(bus, rx + offset, n)) {
                printf("    [sc18is602b] READ-BACK FAILED after retries (ss=0x%02X, n=%lu)\n", ss_select, (unsigned long)n);
                return false;
            }
#ifdef PIN_SD_BRIDGE_INT
        } else {
            /* Write-only transfer: nothing above actually reads the
             * buffer, and reading is what deasserts INT (not Clear
             * Interrupt -- see this file's own INT section comment).
             * Without this, INT stays latched low and the NEXT
             * transaction's wait_int_ready() would see this stale,
             * already-consumed completion instead of its own, real one.
             * Drains with the cheapest possible read (1 byte, not this
             * chunk's own `n`) since the captured content is discarded
             * either way. */
            uint8_t drain[1];
            sc18is602b_read_retry(bus, drain, sizeof(drain));
#endif
        }

#ifdef PIN_SD_BRIDGE_INT
        sc18is602b_clear_interrupt(bus);
#endif

        offset += n;
    }
    return true;
}

bool sc18is602b_transfer(const greenpak_i2c_bus_t *bus, const uint8_t *tx, uint8_t *rx, uint32_t len) {
    return sc18is602b_transfer_ss(bus, SC18IS602B_SS0, tx, rx, len, SC18IS602B_MAX_CHUNK);
}

bool sc18is602b_clock_only(const greenpak_i2c_bus_t *bus, uint32_t len) {
    return sc18is602b_transfer_ss(bus, SC18IS602B_SS1, NULL, NULL, len, SC18IS602B_MAX_CHUNK);
}

bool sc18is602b_transfer_bench(const greenpak_i2c_bus_t *bus, uint8_t ss_select, const uint8_t *tx,
                                uint8_t *rx, uint32_t len, uint32_t chunkSize) {
    if (chunkSize > SC18IS602B_MAX_CHUNK) chunkSize = SC18IS602B_MAX_CHUNK;
    return sc18is602b_transfer_ss(bus, ss_select, tx, rx, len, chunkSize);
}
