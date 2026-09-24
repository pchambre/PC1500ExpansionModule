/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* main_sd_test.c -- standalone bring-up/test firmware for the SD card
 * behind U5 (SC18IS602B I2C-to-SPI bridge), completely independent of
 * the PC-1500 bus loop. Separate build/flash from
 * pc1500_expansion_rp2350 -- built specifically so the SD driver can be
 * tested over USB serial with the dongle sitting on the bench, not
 * plugged into the PC-1500 at all, after SDLS (and, separately, ECVER --
 * pure ROM dispatch, no I2C involved at all) both stopped responding on
 * real hardware, suggesting the whole single-core firmware hangs
 * somewhere in the new SD path rather than the SD command failing
 * cleanly.
 *
 * Reports every step (I2C bus init, disk_initialize(), card capacity,
 * a directory listing, a one-block raw read) over USB serial via
 * printf, then loops re-announcing the final result -- same
 * reconnect-friendly pattern as GreenPak_Provision's main_provision.c
 * (missing the boot-time output is easy to do while switching to a
 * terminal after a flash-triggered reboot).
 *
 * Multi-size write/read-back round trip + raw sector round trip
 * (2026-09-20): added after a real PC-1500 session where a small
 * SDSAVE/SDLOAD round trip worked fine but a large one (DUNGEON2.BAS)
 * came back corrupted -- suggesting a size-dependent bug somewhere in
 * the disk_read()/disk_write() chain (each spans many more
 * sc18is602b_transfer() chunks/CS toggles for a large file than a small
 * one ever exercised). Two complementary tests: RoundTripTest() goes
 * through real FatFs file I/O at several sizes (one sector up to 32KB) --
 * the same path real SDSAVE/SDLOAD uses; RawSectorRoundTripTest() goes
 * around FatFs entirely, writing/reading raw LBAs directly via
 * disk_write()/disk_read(), the purest test of "does data move correctly
 * between the MCU and the I2C/SPI bridge/card" with no filesystem
 * interpretation in the way. Both repeat several times per size to catch
 * intermittent hardware flakiness a single pass could miss -- this is as
 * much a hardware reliability check as a software correctness one.
 * The raw test uses the LAST N sectors of the card (confirmed with the
 * board owner: the data on this card is replaceable) -- still avoids
 * sector 0 (MBR/boot sector) specifically so a failure here can't affect
 * FatFs's ability to even recognize the volume, only this test's own
 * scratch area. Both report not just pass/fail but the first mismatching
 * byte's offset (and which 512-byte sector it falls in, to help
 * correlate with a chunk-boundary-shaped bug) and the total mismatch
 * count, so a failure actually points somewhere instead of just saying
 * "corrupted."
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/stdio_usb.h"
#include "pico/time.h"

#include "ff.h"
#include "diskio.h"
#include "sc18is602b.h"
#include "board_pins.h"

#ifdef PIN_SD_BRIDGE_INT
#include "hardware/gpio.h"
#endif
#include "hardware/i2c.h"

/* Same construction as diskio_sd_bridge.c's own internal g_sd_bus --
 * used only by the chunk-size benchmark below, which needs its own
 * greenpak_i2c_bus_t to call sc18is602b_transfer_bench() directly
 * (that internal g_sd_bus isn't exposed, and doesn't need to be for
 * anything else). greenpak_i2c_init() is idempotent (shared bus,
 * disk_initialize() already calls it too) so calling it again here is
 * harmless. */
static const greenpak_i2c_bus_t g_sdBusForBench = {
    .sda_gpio = PIN_GREENPAK1_SDA,
    .scl_gpio = PIN_GREENPAK1_SCL,
};

static void wait_for_usb_terminal(void) {
    for (int waited_ms = 0; !stdio_usb_connected() && waited_ms < 10000; waited_ms += 100) {
        sleep_ms(100);
    }
    sleep_ms(200);
}

static FATFS g_fatfs;

/* How many times each round-trip test repeats -- intermittent hardware
 * flakiness (this project's own history this session) can easily pass
 * once and fail the next time, so a single pass proves much less than a
 * real hardware reliability check needs. */
#define REPEAT_COUNT 3

/* Deterministic but non-linear (unlike a plain i*37+11 ramp) -- avoids
 * accidentally aliasing with any periodic corruption pattern (e.g. one
 * tied to a fixed chunk size) the way a simple linear ramp could. */
static void FillPattern(uint8_t *buf, size_t len, uint32_t seed) {
    for (size_t i = 0; i < len; i++) {
        uint32_t x = ((uint32_t)i ^ seed) * 2654435761u;
        x ^= x >> 15;
        buf[i] = (uint8_t)(x ^ (x >> 7) ^ (uint32_t)(i >> 9));
    }
}

/* Static (not stack) -- the largest size tested is 32KB, and this runs
 * on the same stack main() itself uses. */
#define ROUND_TRIP_MAX_SIZE (32u * 1024u)
static uint8_t g_writeBuf[ROUND_TRIP_MAX_SIZE];
static uint8_t g_readBuf[ROUND_TRIP_MAX_SIZE];

/* Writes `size` bytes of a known pattern to `path`, reads them back, and
 * reports byte-for-byte agreement. Returns true (and leaves
 * *outSummary alone) only on a full pass; on any failure, writes a short
 * description into outSummary/outSummarySize and returns false. Cleans
 * up its own throwaway file either way. */
static bool RoundTripTest(const char *path, size_t size, uint32_t seed, char *outSummary,
                           size_t outSummarySize) {
    FillPattern(g_writeBuf, size, seed);

    uint32_t dummyRetries, dummyCalls, dummyImm, dummyWaited, dummyTimeout;
    sc18is602b_get_and_reset_retry_stats(&dummyRetries, &dummyCalls); /* clear leftover */
    sc18is602b_get_and_reset_int_stats(&dummyImm, &dummyWaited, &dummyTimeout);

    FIL wf;
    FRESULT wfr = f_open(&wf, path, FA_CREATE_ALWAYS | FA_WRITE);
    if (wfr != FR_OK) {
        snprintf(outSummary, outSummarySize, "f_open(write) FAILED (FRESULT=%d)", wfr);
        return false;
    }
    UINT written = 0;
    uint32_t writeStartUs = time_us_32();
    wfr = f_write(&wf, g_writeBuf, (UINT)size, &written);
    uint32_t writeUs = time_us_32() - writeStartUs;
    f_close(&wf);
    uint32_t writeRetries, writeCalls;
    sc18is602b_get_and_reset_retry_stats(&writeRetries, &writeCalls);
    uint32_t writeIntImm, writeIntWaited, writeIntTimeout;
    sc18is602b_get_and_reset_int_stats(&writeIntImm, &writeIntWaited, &writeIntTimeout);
    if (wfr != FR_OK || written != size) {
        snprintf(outSummary, outSummarySize, "f_write() FAILED (FRESULT=%d, wrote %u/%u bytes)",
                 wfr, (unsigned)written, (unsigned)size);
        f_unlink(path);
        return false;
    }

    FIL rf;
    FRESULT rfr = f_open(&rf, path, FA_READ);
    if (rfr != FR_OK) {
        snprintf(outSummary, outSummarySize, "read-back f_open() FAILED (FRESULT=%d)", rfr);
        f_unlink(path);
        return false;
    }
    memset(g_readBuf, 0, size);
    UINT got = 0;
    uint32_t readStartUs = time_us_32();
    rfr = f_read(&rf, g_readBuf, (UINT)size, &got);
    uint32_t readUs = time_us_32() - readStartUs;
    f_close(&rf);
    f_unlink(path);
    uint32_t readRetries, readCalls;
    sc18is602b_get_and_reset_retry_stats(&readRetries, &readCalls);
    uint32_t readIntImm, readIntWaited, readIntTimeout;
    sc18is602b_get_and_reset_int_stats(&readIntImm, &readIntWaited, &readIntTimeout);
    if (rfr != FR_OK || got != size) {
        snprintf(outSummary, outSummarySize, "read-back f_read() FAILED (FRESULT=%d, got %u/%u bytes)",
                 rfr, (unsigned)got, (unsigned)size);
        return false;
    }
    printf("    (%u bytes: write retries %lu/%lu calls, read retries %lu/%lu calls)\n", (unsigned)size,
           (unsigned long)writeRetries, (unsigned long)writeCalls, (unsigned long)readRetries,
           (unsigned long)readCalls);
    printf("    (%u bytes: write INT imm/wait/timeout %lu/%lu/%lu, read INT imm/wait/timeout %lu/%lu/%lu)\n",
           (unsigned)size, (unsigned long)writeIntImm, (unsigned long)writeIntWaited,
           (unsigned long)writeIntTimeout, (unsigned long)readIntImm, (unsigned long)readIntWaited,
           (unsigned long)readIntTimeout);
    /* KB/s = bytes / 1024 / seconds = bytes * 1e6 / 1024 / us. */
    printf("    (%u bytes: write %lu us = %.2f KB/s, read %lu us = %.2f KB/s)\n", (unsigned)size,
           (unsigned long)writeUs, writeUs ? (double)size * 1000000.0 / 1024.0 / (double)writeUs : 0.0,
           (unsigned long)readUs, readUs ? (double)size * 1000000.0 / 1024.0 / (double)readUs : 0.0);

    size_t firstMismatch = (size_t)-1;
    size_t mismatchCount = 0;
    for (size_t i = 0; i < size; i++) {
        if (g_writeBuf[i] != g_readBuf[i]) {
            if (firstMismatch == (size_t)-1) firstMismatch = i;
            mismatchCount++;
        }
    }
    if (mismatchCount > 0) {
        snprintf(outSummary, outSummarySize,
                 "MISMATCH: %u/%u bytes differ, first at offset %u (sector %u, byte %u) -- "
                 "wrote 0x%02X, read 0x%02X",
                 (unsigned)mismatchCount, (unsigned)size, (unsigned)firstMismatch,
                 (unsigned)(firstMismatch / 512), (unsigned)(firstMismatch % 512),
                 g_writeBuf[firstMismatch], g_readBuf[firstMismatch]);
        return false;
    }
    return true;
}

/* Same idea as RoundTripTest(), but goes straight to disk_write()/
 * disk_read() at a fixed LBA -- no FatFs, no filesystem interpretation,
 * the most direct possible test of the MCU<->bridge<->card data path.
 * `sectorCount*512` must be <= ROUND_TRIP_MAX_SIZE. Overwrites real
 * sectors at `baseSector` -- only call this with a range confirmed safe
 * to overwrite (see this file's own top comment: the board owner
 * confirmed the data on this card is replaceable, and this always
 * targets the LAST sectors of the card, never sector 0). */
static bool RawSectorRoundTripTest(uint32_t baseSector, uint32_t sectorCount, uint32_t seed,
                                    char *outSummary, size_t outSummarySize) {
    size_t size = (size_t)sectorCount * 512u;
    FillPattern(g_writeBuf, size, seed);

    uint32_t writeStartUs = time_us_32();
    DRESULT wr = disk_write(0, g_writeBuf, baseSector, sectorCount);
    uint32_t writeUs = time_us_32() - writeStartUs;
    if (wr != RES_OK) {
        snprintf(outSummary, outSummarySize, "disk_write() FAILED (result=%d)", wr);
        return false;
    }
    memset(g_readBuf, 0, size);
    uint32_t readStartUs = time_us_32();
    DRESULT rr = disk_read(0, g_readBuf, baseSector, sectorCount);
    uint32_t readUs = time_us_32() - readStartUs;
    if (rr != RES_OK) {
        snprintf(outSummary, outSummarySize, "disk_read() FAILED (result=%d)", rr);
        return false;
    }
    printf("    (%u bytes: raw write %lu us = %.2f KB/s, raw read %lu us = %.2f KB/s)\n",
           (unsigned)size, (unsigned long)writeUs,
           writeUs ? (double)size * 1000000.0 / 1024.0 / (double)writeUs : 0.0,
           (unsigned long)readUs, readUs ? (double)size * 1000000.0 / 1024.0 / (double)readUs : 0.0);

    size_t firstMismatch = (size_t)-1;
    size_t mismatchCount = 0;
    for (size_t i = 0; i < size; i++) {
        if (g_writeBuf[i] != g_readBuf[i]) {
            if (firstMismatch == (size_t)-1) firstMismatch = i;
            mismatchCount++;
        }
    }
    if (mismatchCount > 0) {
        snprintf(outSummary, outSummarySize,
                 "MISMATCH: %u/%u bytes differ, first at offset %u (sector %u, byte %u) -- "
                 "wrote 0x%02X, read 0x%02X",
                 (unsigned)mismatchCount, (unsigned)size, (unsigned)firstMismatch,
                 (unsigned)(baseSector + firstMismatch / 512), (unsigned)(firstMismatch % 512),
                 g_writeBuf[firstMismatch], g_readBuf[firstMismatch]);
        return false;
    }
    return true;
}

/* Isolated bridge+I2C chunk-size benchmark (2026-09-20) -- see this
 * file's own top comment. Runs `totalBytes` worth of write+read-back
 * traffic through sc18is602b_transfer_bench() at SS1 (unconnected --
 * completely isolated from the real SD card, so this can run freely
 * without any risk to card state), split into `chunkSize`-byte pieces,
 * and reports timing/throughput -- no correctness check is meaningful
 * here (SS1 has nothing attached, so read-back data is undefined), only
 * timing. */
static void BenchBridgeOnlyAtChunkSize(uint32_t chunkSize, uint32_t totalBytes) {
    if (totalBytes > ROUND_TRIP_MAX_SIZE) totalBytes = ROUND_TRIP_MAX_SIZE;
    FillPattern(g_writeBuf, totalBytes, 0xC001C0DEu);

    uint32_t writeStartUs = time_us_32();
    bool wok = sc18is602b_transfer_bench(&g_sdBusForBench, SC18IS602B_SS1, g_writeBuf, NULL, totalBytes,
                                          chunkSize);
    uint32_t writeUs = time_us_32() - writeStartUs;

    uint32_t readStartUs = time_us_32();
    bool rok = sc18is602b_transfer_bench(&g_sdBusForBench, SC18IS602B_SS1, NULL, g_readBuf, totalBytes,
                                          chunkSize);
    uint32_t readUs = time_us_32() - readStartUs;

    if (!wok || !rok) {
        printf("  chunk=%3u: FAILED (write ok=%d, read ok=%d)\n", (unsigned)chunkSize, wok, rok);
        return;
    }
    printf("  chunk=%3u: write %6lu us = %6.2f KB/s, read %6lu us = %6.2f KB/s\n", (unsigned)chunkSize,
           (unsigned long)writeUs, writeUs ? (double)totalBytes * 1000000.0 / 1024.0 / (double)writeUs : 0.0,
           (unsigned long)readUs, readUs ? (double)totalBytes * 1000000.0 / 1024.0 / (double)readUs : 0.0);
}

/* Datasheet-conformance test (2026-09-20) -- the INT-pin investigation
 * this session (see this project's own memory notes) built the
 * Clear-Interrupt fix by inference from the NXP SC18IS602B datasheet
 * (rev 7, 21 Oct 2019) section 8's text, but never actually replicated
 * that section's own worked example -- configure SPI, write, Clear
 * Interrupt, write again, Clear Interrupt, read, Clear Interrupt -- at
 * the reference clock rate (115 kHz) it's demonstrated at, and never
 * isolated it from the real SD card's own CMD0 behavior. This does that:
 * three SPI-shaped transactions (a 1-byte write-only, an 8-byte
 * write-only, an 8-byte read) through SS1 (unconnected on this board --
 * nothing needs to actually respond; this checks the BRIDGE's own
 * INT-assert/Clear-Interrupt bookkeeping, not real slave data, so
 * read-back content is meaningless and never checked). Each step is one
 * sc18is602b_transfer_bench() call, which -- same as production code --
 * internally does write, wait_int_ready(), optional read-back,
 * clear_interrupt() as one cycle; this just makes that cycle's actual
 * pin behavior and diagnostic counters visible per step instead of
 * hidden inside a bulk transfer. `pinIdleAfter` reads the raw GPIO
 * directly after each step: if Clear Interrupt actually worked, INT
 * (active-low) should read back HIGH/idle every time, not just "not
 * timed out." Requires PIN_SD_BRIDGE_INT (sd_bridge_test's own
 * CMakeLists.txt target_compile_definitions -- NOT defined for the
 * production pc1500_expansion_rp2350 build). */
#ifdef PIN_SD_BRIDGE_INT
static bool DatasheetSequenceStep(const char *label, const uint8_t *tx, uint8_t *rx, uint32_t len) {
    uint32_t dImm, dWaited, dTimeout;
    sc18is602b_get_and_reset_int_stats(&dImm, &dWaited, &dTimeout); /* clear leftover from any prior step */
    bool ok = sc18is602b_transfer_bench(&g_sdBusForBench, SC18IS602B_SS1, tx, rx, len, len);
    uint32_t imm, waited, timeout;
    sc18is602b_get_and_reset_int_stats(&imm, &waited, &timeout);
    bool pinIdleAfter = gpio_get(PIN_SD_BRIDGE_INT) != 0; /* active-low: idle/cleared reads HIGH */
    printf("    %-26s %-8s INT imm/wait/timeout %lu/%lu/%lu, pin-after=%s\n", label, ok ? "OK" : "I2C FAIL",
           (unsigned long)imm, (unsigned long)waited, (unsigned long)timeout,
           pinIdleAfter ? "HIGH(idle)" : "LOW(still asserted!)");
    return ok && timeout == 0 && pinIdleAfter;
}

/* Returns true only if every step passed cleanly (real I2C success, zero
 * INT timeouts, pin reads idle/HIGH after each Clear Interrupt). */
static bool TestDatasheetIntSequence(const char *clockName, uint8_t clockRate) {
    printf("\n-- Datasheet INT/Clear-Interrupt sequence @ %s (SS1, isolated) --\n", clockName);
    if (!sc18is602b_configure(&g_sdBusForBench, SC18IS602B_MODE_CPOL0_CPHA0, clockRate)) {
        printf("    sc18is602b_configure() FAILED -- aborting this run\n");
        return false;
    }
    /* Byte VALUES are arbitrary (nothing is listening on SS1) -- only the
     * transaction SHAPE (write-only, write-only, read) matters, matching
     * datasheet section 8 steps 2/4/6 (its own EEPROM write-enable byte,
     * 8-byte write, and 8-byte read). */
    uint8_t writeEnable[1] = { 0x06 };
    uint8_t eightBytes[8] = { 0x02, 0x00, 0x30, 0x01, 0x02, 0x03, 0x04, 0x05 };
    uint8_t readBuf[8];

    bool step2 = DatasheetSequenceStep("write-enable (1 byte)", writeEnable, NULL, sizeof(writeEnable));

    /* Follow-up isolation (2026-09-21), per the board owner's own question
     * after seeing step2/step4 leave INT asserted despite a Clear
     * Interrupt: does a bare read of the (already-captured, unread) buffer
     * -- with NO new SPI write, and BEFORE any Clear Interrupt -- clear
     * INT on its own? If so, the pin tracks "buffer has unread data," not
     * just "a transfer completed and hasn't been acknowledged" -- Clear
     * Interrupt alone wouldn't be expected to release it while a write-only
     * transfer's own captured (but never-read) byte is still sitting
     * there. This reads back step2's own 1 pending byte directly (no
     * Function ID, matching datasheet section 8 step 8's own "read the
     * data buffer" framing), checks the pin BEFORE any Clear Interrupt,
     * then sends Clear Interrupt anyway and checks again, to see both
     * halves independently rather than inferring one from the other. */
    /* Runs regardless of step2's own pass/fail -- step2 "fails" precisely
     * because the pin didn't clear, which is exactly the condition this
     * isolates, not a reason to skip it. Only a real I2C error would make
     * this meaningless, and greenpak_i2c_read() below reports that itself. */
    {
        uint8_t drainBuf[1];
        bool drainOk = greenpak_i2c_read(&g_sdBusForBench, SC18IS602B_I2C_ADDR, drainBuf, sizeof(drainBuf));
        bool pinAfterBareRead = gpio_get(PIN_SD_BRIDGE_INT) != 0;
        printf("    bare read (no write, no clear)  %s   pin-after=%s\n", drainOk ? "OK" : "I2C FAIL",
               pinAfterBareRead ? "HIGH(idle)" : "LOW(still asserted!)");

        uint8_t clearCmd = 0xF1; /* SC18IS602B_FUNC_CLEAR_INTERRUPT, private to sc18is602b.c --
                                     hardcoded here rather than exposing it just for this one-off test */
        bool clearOk = greenpak_i2c_write(&g_sdBusForBench, SC18IS602B_I2C_ADDR, &clearCmd, 1);
        bool pinAfterClear = gpio_get(PIN_SD_BRIDGE_INT) != 0;
        printf("    ...then explicit Clear Interrupt %s   pin-after=%s\n", clearOk ? "OK" : "I2C FAIL",
               pinAfterClear ? "HIGH(idle)" : "LOW(still asserted!)");
    }

    bool step4 = DatasheetSequenceStep("write 8 bytes", eightBytes, NULL, sizeof(eightBytes));
    bool step6 = DatasheetSequenceStep("read 8 bytes (dummy clock)", NULL, readBuf, sizeof(readBuf));

    bool allOk = step2 && step4 && step6;
    printf("  Result @ %s: %s\n", clockName,
           allOk ? "PASS -- INT/Clear-Interrupt behaved as documented" : "FAIL -- see steps above");
    return allOk;
}
#endif

/* I2C-side overclock experiment -- REMOVED (2026-09-21). Was tried per the
 * board owner's own request ("seeing whether the SC18IS602B can work at
 * 1MHz on the I2C side, even though this is beyond its rating"): pushed
 * the RP2350's own I2C1 peripheral to 1 MHz against U5, restoring to
 * 400kHz afterward. Answer is confirmed and doesn't need re-asking on
 * every boot: it does NOT work -- every attempt got a hard failure (both
 * write and read "FAIL after retries", not just slow/degraded), 400kHz
 * really is close to a hard ceiling for this chip. Worse: live evidence
 * this same session strongly suggests the failed attempt leaves U5's own
 * I2C interface wedged for the rest of that boot (a clean datasheet-test
 * pass immediately followed by this test's own hard failure, immediately
 * followed by disk_initialize() also failing, repeatably) -- U5 has no
 * software-reachable power control or RESET pin on this board (shares the
 * Pico module's own 3V3 rail, confirmed via the real KiCad schematic; the
 * SC18IS602B's RESET pin isn't wired to any RP2350 GPIO), so there's no
 * cheap way to un-wedge it after deliberately provoking a hard I2C
 * failure. Not worth running unconditionally on every boot just to keep
 * re-confirming a known answer at the cost of destabilizing every test
 * that runs afterward. See greenpak_i2c_bus_recover() (greenpak_i2c.c) if
 * a real bus recovery is ever needed again. */

/* Per-byte I2C timing instrumentation (2026-09-21), per the board owner's
 * own request: pin down how much of the gap between the raw I2C bit-rate
 * ceiling (~43 KB/s @ 400kHz, 9 bits/byte including ACK, zero other
 * overhead) and the measured bulk write rate (~23 KB/s in the isolated
 * bench) is the SC18IS602B's own documented per-byte clock-stretching
 * ("it does have the ability to hold the SCL line LOW between bytes to
 * complete its internal processes" -- datasheet section 7.1, page 4) --
 * a real protocol-level floor DMA cannot bypass -- versus RP2350-side
 * software/polling overhead, which DMA genuinely could help with.
 *
 * Deliberately NOT built on the pico-sdk's own i2c_write_blocking()/
 * i2c_write_timeout_per_char_us(): both disable and re-enable the WHOLE
 * I2C peripheral on every single call (to reset IC_TAR, see
 * i2c_write_blocking_internal() in the SDK's own hardware_i2c/i2c.c) --
 * calling that once per byte to get per-byte timestamps would tear one
 * real, continuous 200-byte transaction into 200 separate tiny ones (each
 * with its own START), not measure what an actual bulk write chunk
 * experiences. This instead hand-rolls the exact same inner loop that
 * function uses internally (same DATA_CMD/STOP sequencing, same
 * TX_EMPTY wait, same abort handling) via the public i2c_get_hw()
 * accessor, just bracketing each byte's own TX_EMPTY wait with
 * time_us_32(). Always ends with a real STOP (last byte), so the bridge
 * sees a normal, complete, valid write -- this isn't a protocol
 * shortcut, just where the clock ticks happen to get read from. */
static void MeasureI2CByteTiming(uint8_t addr7, const uint8_t *data, uint32_t len) {
    i2c_hw_t *hw = i2c_get_hw(i2c1);
    hw->enable = 0;
    hw->tar = addr7;
    hw->enable = 1;

    static uint32_t byteUs[SC18IS602B_MAX_CHUNK + 1];
    bool abort = false;
    uint32_t abortReason = 0;
    uint32_t sentCount = 0;

    for (uint32_t i = 0; i < len; i++) {
        bool last = (i == len - 1);
        uint32_t startUs = time_us_32();
        hw->data_cmd = (last ? (1u << I2C_IC_DATA_CMD_STOP_LSB) : 0u) | data[i];
        while (!(hw->raw_intr_stat & I2C_IC_RAW_INTR_STAT_TX_EMPTY_BITS)) {
            tight_loop_contents();
        }
        byteUs[i] = time_us_32() - startUs;
        sentCount++;

        abortReason = hw->tx_abrt_source;
        if (abortReason) {
            (void)hw->clr_tx_abrt;
            abort = true;
        }
        if (abort || last) {
            while (!(hw->raw_intr_stat & I2C_IC_RAW_INTR_STAT_STOP_DET_BITS)) {
                tight_loop_contents();
            }
            (void)hw->clr_stop_det;
        }
        if (abort) break;
    }

    if (abort) {
        printf("  MeasureI2CByteTiming: ABORTED after %lu/%lu bytes (tx_abrt_source=0x%08lx)\n",
               (unsigned long)sentCount, (unsigned long)len, (unsigned long)abortReason);
        return;
    }

    uint32_t minUs = 0xFFFFFFFFu, maxUs = 0, sumUs = 0;
    uint32_t bucket25 = 0, bucket50 = 0, bucket100 = 0, bucketOver = 0;
    for (uint32_t i = 0; i < len; i++) {
        uint32_t t = byteUs[i];
        if (t < minUs) minUs = t;
        if (t > maxUs) maxUs = t;
        sumUs += t;
        if (t <= 25) bucket25++;
        else if (t <= 50) bucket50++;
        else if (t <= 100) bucket100++;
        else bucketOver++;
    }
    printf("\n-- Per-byte I2C write timing (%lu bytes, addr=0x%02X) --\n", (unsigned long)len,
           addr7);
    printf("  min=%lu us, max=%lu us, avg=%.2f us (theoretical min @400kHz ~22.5 us/byte)\n",
           (unsigned long)minUs, (unsigned long)maxUs, (double)sumUs / (double)len);
    printf("  distribution: <=25us:%lu  26-50us:%lu  51-100us:%lu  >100us:%lu\n",
           (unsigned long)bucket25, (unsigned long)bucket50, (unsigned long)bucket100,
           (unsigned long)bucketOver);
}

int main(void) {
    stdio_init_all();
    setvbuf(stdout, NULL, _IONBF, 0);
    wait_for_usb_terminal();

    printf("\n=== SD-over-I2C-bridge (SC18IS602B/U5) standalone test ===\n");

    /* Datasheet INT/Clear-Interrupt conformance check -- runs BEFORE
     * disk_initialize() and entirely over SS1 (unconnected), so it can't
     * be affected by, or affect, the real card's own CMD0 behavior.
     *
     * No longer gates the 1.8432 MHz run on the 115 kHz baseline "passing"
     * (2026-09-21) -- root-caused why 115 kHz reports FAIL: a write-only
     * transaction never clears INT via Clear Interrupt alone (only an
     * actual read of the buffer does, confirmed via the bare-read
     * isolation below), which isn't a real problem -- 5/5 real-SD-card
     * round-trip runs across independent power cycles all passed clean
     * with INT enabled. That "FAIL" was never a sign 115 kHz doesn't work,
     * just that this test's own pass criteria were stricter than what
     * actually matters. Runs both rates unconditionally now. */
#ifdef PIN_SD_BRIDGE_INT
    /* Normally disk_initialize() is what calls greenpak_i2c_init() first
     * (see g_sdBusForBench's own comment) -- this test runs BEFORE that,
     * so it has to do its own init or every I2C call fails outright
     * (confirmed live: "no I2C ACK from U5" on the very first configure()
     * call, not a real hardware fault, just unconditional bus setup never
     * having happened yet). Idempotent, so disk_initialize()'s own later
     * call is still harmless. */
    greenpak_i2c_init(&g_sdBusForBench);
    TestDatasheetIntSequence("115 kHz (datasheet reference)", SC18IS602B_CLK_115KHZ);
    TestDatasheetIntSequence("1.8432 MHz (production max)", SC18IS602B_CLK_1843KHZ);
    printf("\n");
#else
    printf("\n(Datasheet INT sequence test SKIPPED -- PIN_SD_BRIDGE_INT not defined for this build)\n");
#endif

    /* Needs greenpak_i2c_init() to have run at least once first (idempotent,
     * see its own callers' comments) -- unconditional here since, unlike
     * the datasheet INT sequence above, the per-byte timing test below
     * doesn't depend on PIN_SD_BRIDGE_INT at all. */
    greenpak_i2c_init(&g_sdBusForBench);
    /* Explicit SPI-side configure here too -- don't rely on the (optional,
     * PIN_SD_BRIDGE_INT-gated) datasheet INT sequence above having already
     * done this. */
    sc18is602b_configure(&g_sdBusForBench, SC18IS602B_MODE_CPOL0_CPHA0, SC18IS602B_CLK_1843KHZ);

    /* Per-byte timing, real chunk shape (SS1 select byte + 199 data
     * bytes, matching SC18IS602B_MAX_CHUNK) at the real 400 kHz spec
     * rate. */
    {
        uint8_t payload[1 + SC18IS602B_MAX_CHUNK];
        payload[0] = SC18IS602B_SS1;
        FillPattern(payload + 1, SC18IS602B_MAX_CHUNK, 0xB17E1234u);
        MeasureI2CByteTiming(SC18IS602B_I2C_ADDR, payload, sizeof(payload));
#ifdef PIN_SD_BRIDGE_INT
        /* This diagnostic wrote raw bytes straight to hardware, bypassing
         * sc18is602b_transfer_ss()'s own wait/drain/clear machinery
         * entirely -- if INT is enabled, that write's own completion
         * interrupt is now sitting unconsumed. Drain it here (see
         * sc18is602b.c's own INT section comment for why a bare read,
         * not Clear Interrupt, is what actually does this) so the very
         * next real transfer (disk_initialize(), right below) doesn't
         * see stale asserted state on its own first wait_int_ready(). */
        uint8_t drain[1];
        greenpak_i2c_read(&g_sdBusForBench, SC18IS602B_I2C_ADDR, drain, sizeof(drain));
#endif
    }

    const char *result_summary = "did not reach a conclusion";

    DSTATUS ds = disk_initialize(0);
    if (ds & STA_NOINIT) {
        printf("disk_initialize(): FAILED (status=0x%02X) -- no card detected, or the\n"
               "  bridge/card didn't respond as expected. Check: card seated, U5's\n"
               "  SDA/SCL actually reach the Pico (GP26/GP27), U5's I2C address\n"
               "  strapping (A0/A1/A2 -> GND -> 0x28).\n", ds);
        result_summary = "disk_initialize() FAILED";
    } else {
        printf("disk_initialize(): OK\n");

        DWORD sector_count = 0;
        if (disk_ioctl(0, GET_SECTOR_COUNT, &sector_count) == RES_OK) {
            printf("Card capacity: %lu sectors (%.1f MB)\n", (unsigned long)sector_count,
                   (double)sector_count * 512.0 / (1024.0 * 1024.0));
        } else {
            printf("GET_SECTOR_COUNT failed\n");
        }

        uint8_t block[512];
        DRESULT dr = disk_read(0, block, 0, 1);
        if (dr == RES_OK) {
            printf("disk_read(sector 0): OK -- first 16 bytes:");
            for (int i = 0; i < 16; i++) printf(" %02X", block[i]);
            printf("\n");
        } else {
            printf("disk_read(sector 0): FAILED (result=%d)\n", dr);
        }

        /* Isolated bridge+I2C chunk-size sweep (2026-09-20) -- see
         * BenchBridgeOnlyAtChunkSize()'s own comment. Runs entirely
         * through SS1 (unconnected), so this is completely safe to run
         * here regardless of what happens below. 8KB per chunk size is
         * enough to average out per-call jitter without taking too
         * long. 128 is included specifically because it divides evenly
         * into 512 (a real SD sector) -- see this session's own
         * discussion of whether that alignment matters. */
        printf("\nBridge+I2C-only chunk-size sweep (SS1, isolated from the SD card):\n");
        static const uint32_t kBenchChunkSizes[] = { 32, 64, 100, 128, 150, 199 };
        for (size_t c = 0; c < sizeof(kBenchChunkSizes) / sizeof(kBenchChunkSizes[0]); c++) {
            BenchBridgeOnlyAtChunkSize(kBenchChunkSizes[c], 8192);
        }
        printf("\n");

        FRESULT fr = f_mount(&g_fatfs, "", 1);
        if (fr != FR_OK) {
            printf("f_mount(): FAILED (FRESULT=%d) -- disk_read() worked above, so the\n"
                   "  bridge/card link is fine; this points at the filesystem itself\n"
                   "  (unformatted, or a filesystem type FatFs doesn't recognize).\n", fr);
            result_summary = "disk_read() OK, f_mount() FAILED";
        } else {
            printf("f_mount(): OK -- directory listing of /:\n");
            DIR dir;
            FILINFO fno;
            int count = 0;
            if (f_opendir(&dir, "/") == FR_OK) {
                while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != 0) {
                    printf("  %s%s  %lu bytes\n", fno.fname, (fno.fattrib & AM_DIR) ? "/" : "",
                           (unsigned long)fno.fsize);
                    count++;
                }
                f_closedir(&dir);
                printf("(%d entries)\n", count);
                result_summary = "f_mount() OK, directory listing OK";

                /* Multi-size round trip -- exercises disk_write()/disk_read()
                 * (see this file's own top comment) at sizes from one sector
                 * up to 32KB, specifically to find whether/where a
                 * size-dependent corruption threshold exists. Repeated
                 * REPEAT_COUNT times per size (not just once) to catch
                 * intermittent hardware flakiness a single pass could
                 * miss -- this is a hardware reliability check as much as a
                 * software correctness one. Uses a throwaway filename,
                 * cleaned up after each attempt regardless of outcome, so
                 * this never touches any real file on the card. */
                static const size_t kSizes[] = { 512, 1024, 2048, 4096, 8192, 16384, 32768 };
                static const char testPath[] = "CLAUDEWR.TST";
                bool allPassed = true;
                size_t firstFailedSize = 0;
                for (size_t s = 0; s < sizeof(kSizes) / sizeof(kSizes[0]); s++) {
                    int passCount = 0;
                    char lastFailDetail[160];
                    lastFailDetail[0] = 0;
                    for (int attempt = 0; attempt < REPEAT_COUNT; attempt++) {
                        char detail[160];
                        detail[0] = 0;
                        /* Seed varies by size AND attempt -- a bug that only
                         * shows up for a specific byte VALUE pattern
                         * (unlikely, but costs nothing to guard against)
                         * wouldn't hide behind every attempt using
                         * identical bytes. */
                        bool ok = RoundTripTest(
                            testPath, kSizes[s],
                            (uint32_t)(0xA5A50000u + kSizes[s] + (uint32_t)attempt * 97u), detail,
                            sizeof(detail));
                        if (ok) {
                            passCount++;
                        } else {
                            strncpy(lastFailDetail, detail, sizeof(lastFailDetail) - 1);
                        }
                    }
                    if (passCount == REPEAT_COUNT) {
                        printf("round-trip %6u bytes: PASS (%d/%d)\n", (unsigned)kSizes[s], passCount,
                               REPEAT_COUNT);
                    } else {
                        printf("round-trip %6u bytes: FAIL (%d/%d passed) -- last failure: %s\n",
                               (unsigned)kSizes[s], passCount, REPEAT_COUNT, lastFailDetail);
                        allPassed = false;
                        if (firstFailedSize == 0) firstFailedSize = kSizes[s];
                    }
                }

                /* Raw sector round trip -- see this file's own top comment
                 * for why this one goes around FatFs entirely, and why it's
                 * safe to run here (last sectors of the card, never sector
                 * 0, board owner confirmed this card's data is
                 * replaceable). */
                DWORD totalSectors = 0;
                bool rawTestRan = false;
                bool rawAllPassed = true;
                if (disk_ioctl(0, GET_SECTOR_COUNT, &totalSectors) == RES_OK && totalSectors > 128) {
                    uint32_t rawSectorCount = ROUND_TRIP_MAX_SIZE / 512u; /* 64 sectors = 32KB */
                    uint32_t rawBaseSector = (uint32_t)totalSectors - rawSectorCount - 1u;
                    rawTestRan = true;
                    for (int attempt = 0; attempt < REPEAT_COUNT; attempt++) {
                        char detail[160];
                        detail[0] = 0;
                        bool ok = RawSectorRoundTripTest(
                            rawBaseSector, rawSectorCount,
                            (uint32_t)(0x5A5A0000u + (uint32_t)attempt * 131u), detail, sizeof(detail));
                        if (ok) {
                            printf("raw sector round-trip (LBA %lu, %u sectors) attempt %d/%d: PASS\n",
                                   (unsigned long)rawBaseSector, (unsigned)rawSectorCount, attempt + 1,
                                   REPEAT_COUNT);
                        } else {
                            printf(
                                "raw sector round-trip (LBA %lu, %u sectors) attempt %d/%d: FAIL -- %s\n",
                                (unsigned long)rawBaseSector, (unsigned)rawSectorCount, attempt + 1,
                                REPEAT_COUNT, detail);
                            rawAllPassed = false;
                        }
                    }
                } else {
                    printf("raw sector round-trip: SKIPPED (GET_SECTOR_COUNT failed or card too small)\n");
                }

                if (allPassed && (!rawTestRan || rawAllPassed)) {
                    result_summary = "all round-trip tests PASS";
                } else if (!allPassed) {
                    static char summaryBuf[64];
                    snprintf(summaryBuf, sizeof(summaryBuf),
                             "file round-trip FAILED starting at %u bytes", (unsigned)firstFailedSize);
                    result_summary = summaryBuf;
                } else {
                    result_summary = "raw sector round-trip FAILED";
                }
            } else {
                printf("f_opendir(\"/\") FAILED\n");
                result_summary = "f_mount() OK, f_opendir() FAILED";
            }
        }
    }

    printf("\nDone: %s\n", result_summary);
    while (true) {
        printf("\n[idle] SD bridge test -- result: %s (reconnect the serial monitor any time)\n",
               result_summary);
        sleep_ms(5000);
    }
}
