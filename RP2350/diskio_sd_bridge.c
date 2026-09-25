/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* diskio_sd_bridge.c -- FatFs disk_*() implementation for an SD card
 * behind U5 (SC18IS602B I2C-to-SPI bridge) -- see sc18is602b.h's own top
 * comment and board_pins.h's SD comment for why this board needs this
 * instead of a native RP2350 SPI/QMI driver.
 *
 * Standard SD-over-SPI protocol (Physical Layer Simplified Spec, the
 * same command set every SD-over-SPI driver implements), just with
 * every SPI byte-transfer routed through sc18is602b_transfer() instead
 * of a real hardware_spi peripheral. Targets modern SDHC/SDXC cards
 * (CSD structure version 2.0) as the primary case, with a best-effort
 * SDSC (CSD v1.0, byte addressing, explicit CMD16 block-length set)
 * fallback for older cards -- genuine MMC (not SD) is not handled.
 *
 * CS framing caveat: this bridge asserts SS0 only for the duration of
 * each individual I2C write carrying a Function-ID-0x01 SPI transfer,
 * releasing it at the end of that one I2C message (datasheet section
 * 7.1.3) -- there's no way to hold CS asserted continuously across
 * separate I2C messages the way a direct hardware_spi driver would
 * across one held gpio_put(cs, 0). sd_command() sends the command frame
 * and its response-poll bytes as ONE combined transfer rather than
 * separate write-then-poll calls, since real cards expect CS to stay
 * asserted continuously from a command through its response -- this
 * alone did NOT fix a real, live symptom this session (a known-good
 * card, working fine in other readers, never responding to CMD0 at
 * all). The real fix (confirmed 2026-09-17, after a physical J2 rework
 * -- see board_pins.h/schematic notes) turned out to be a genuine
 * miswiring on the microSD socket itself, not a CS-continuity issue at
 * all; CMD0 now returns a correct R1 with no protocol change needed.
 * The CS-continuity concern IS real for a different reason, though:
 * once CMD0 started working, CMD8's R7 and CMD58's OCR trailing bytes
 * (4 bytes right after R1, in the same short response window) and
 * CMD9's CSD register (16 bytes after a 0xFE start token) were all
 * coming back as 0xFF -- not because the card didn't send them, but
 * because sd_command()/sd_read_data_block() were reading them via a
 * SEPARATE sc18is602b_transfer() call, which reasserts CS and gets
 * nothing back. Fixed for R7/OCR (sd_command_ext()) and for the CSD
 * (sd_read_csd()) by capturing everything -- command frame, R1, and the
 * trailing/token+data bytes -- in one combined transfer instead.
 *
 * That fix has a hard ceiling, though: the SC18IS602B's own internal
 * SPI buffer is 200 bytes (datasheet section 7.1.3), and a
 * command+R1+token-wait+512-byte data block+CRC (CMD17/CMD24) is ~530+
 * bytes -- it literally cannot be captured as one continuous CS-low
 * session on this chip regardless of chunk size, unlike CMD9's 16-byte
 * CSD. Confirmed 2026-09 against the actual Linux kernel
 * drivers/spi/spi-sc18is602.c (not just the datasheet): its own
 * sc18is602_check_transfer() rejects any single SPI message over its
 * 200-byte buffer outright (-EINVAL) rather than chunking it -- there is
 * no driver trick that holds CS across multiple I2C transactions on this
 * chip, full stop. So for real 512-byte block I/O, some CS toggling
 * mid-block is unavoidable no matter what -- confirmed live 2026-09-18
 * that a real card tolerates it (data does come back correct), just
 * unreliably/slowly with the naive separate-calls-per-step version
 * below. sd_read_block() (disk_read()'s helper) mitigates what IS
 * avoidable: it merges the command frame, R1, and the token-wait into
 * the SAME first transfer (same technique as sd_read_csd()), and grabs
 * as much real data as fits in that same chunk, before falling through
 * to normal chunked transfers (now sized close to the real 200-byte
 * ceiling instead of a conservative 128 -- see sc18is602b.c) for
 * whatever's left. This removes the two most avoidable offenders (a
 * fully separate command-phase transfer, and the old byte-at-a-time
 * token poll -- each iteration of THAT was its own extra CS pulse) on
 * top of the unavoidable per-chunk toggling, not instead of it.
 * sd_write_block() (disk_write()'s helper, added 2026-09-19) applies the
 * same idea to CMD24: command frame + R1 + the write Start Block token
 * (which, unlike a read's token, we send ourselves at a known offset --
 * no wait/search margin needed for it) + as much payload as fits, all in
 * one chunk, sized against the shared SC18IS602B_MAX_CHUNK constant
 * (sc18is602b.h) rather than a separately-hardcoded copy of it. The
 * data-response token that follows the CRC bytes DOES get a small search
 * margin, same reasoning as the read side's token -- this exact path
 * (disk_write(), CMD24) had never been confirmed against real hardware
 * before (see main_sd_test.c's own write/read-back round-trip test,
 * which explicitly existed to exercise it), so no assumption here should
 * be more confident than the read side's own, already-empirically-tested
 * equivalent.
 */
#include "ff.h"
#include "diskio.h"

#include <stdio.h>
#include <string.h>
#include "pico/time.h"

#include "board_pins.h"
#include "sc18is602b.h"
#include "mcu_log.h"
#include "monitor.h"

#define SD_CMD0_GO_IDLE_STATE        0
#define SD_CMD8_SEND_IF_COND         8
#define SD_CMD9_SEND_CSD             9
#define SD_CMD12_STOP_TRANSMISSION   12
#define SD_CMD13_SEND_STATUS         13
#define SD_CMD16_SET_BLOCKLEN        16
#define SD_CMD17_READ_SINGLE_BLOCK   17
#define SD_CMD24_WRITE_BLOCK         24
#define SD_CMD55_APP_CMD             55
#define SD_CMD58_READ_OCR            58
#define SD_ACMD41_SD_SEND_OP_COND    41

#define SD_R1_IDLE_STATE   0x01
#define SD_R1_ILLEGAL_CMD  0x04

#define SD_TOKEN_START_BLOCK 0xFE

#define SD_INIT_TIMEOUT_US   1500000u  /* ACMD41 can legitimately take up to ~1s */
#define SD_CMD_TIMEOUT_US    200000u   /* R1 response wait */
#define SD_READ_TIMEOUT_US   200000u   /* wait for the data start token */
#define SD_WRITE_BUSY_TIMEOUT_US 300000u  /* post-write card-busy wait */

/* Pacing for the token-wait/busy-wait poll loops below (2026-09-20) --
 * without this, both loops re-check as fast as the bridge chip's own
 * SPI-clock-out busy window lets them, which is exactly the pattern a
 * real-hardware retry-count diagnostic (sc18is602b.c's temporary
 * sc18is602b_get_and_reset_retry_stats()) found needing roughly one
 * SC18IS602B_BUSY_RETRY_DELAY_US retry per poll on a 32KB transfer --
 * i.e. most polls were hitting the bridge still mid-SPI-clock-out from
 * the *previous* poll, not learning anything new, just paying that
 * retry cost again. The card's own write-busy period (the SD CARD's
 * internal flash-programming latency -- a separate concern from the
 * bridge chip's own SPI-clock-out busy window that the retry mechanism
 * exists for) is typically hundreds of microseconds to a few
 * milliseconds, so a poll interval this fine-grained was never buying
 * earlier detection, only extra bridge-busy collisions. */
#define SD_POLL_INTERVAL_US 200u

static greenpak_i2c_bus_t g_sd_bus;
static bool g_sd_initialized = false;
static bool g_sd_quiet = false;           /* suppresses sd_command_ext()'s raw-byte printf --
                                             set around disk_status()'s per-call CMD13 */
static bool g_sd_is_sdhc = false;         /* block (SDHC/SDXC) vs byte (SDSC) addressing */
static uint32_t g_sd_sector_count = 0;    /* filled in from CSD at init */

static inline uint32_t elapsed_us(uint32_t since) {
    return time_us_32() - since;
}

/* Real SD cards expect CS to stay continuously asserted from the start
 * of a command through receipt of its response -- toggling it in
 * between (as separate sc18is602b_transfer() calls would, since this
 * bridge only holds SS0 for the duration of one I2C write) risks the
 * card treating the command as abandoned. So the command frame and its
 * R1-response poll bytes are sent as ONE combined transfer (well under
 * sc18is602b's 128-byte chunk size, so it's genuinely one continuous
 * CS-low I2C write+read, not internally re-chunked either) rather than
 * a separate write followed by separate poll reads. */
#define SD_R1_POLL_BYTES 16

/* Sends a 6-byte SD command frame (the two well-known CRCs, 0x95 for
 * CMD0 and 0x87 for CMD8, are the only ones ever actually checked in
 * SPI mode before CRC is enabled -- CRC checking is never enabled here,
 * so every other command's CRC byte is just a placeholder 0xFF, per the
 * spec's own default), immediately followed by SD_R1_POLL_BYTES dummy
 * bytes in the same transfer, and returns the first response byte with
 * bit 7 clear (0xFF if none of them were a valid R1).
 *
 * `extra`/`extra_len`, if given, are filled from whatever came right
 * after the R1 byte in that SAME already-captured transfer -- R7
 * (CMD8) and R3 (CMD58) are R1 followed immediately by 4 more bytes in
 * the same continuous CS-low session, and since this function already
 * captures SD_R1_POLL_BYTES bytes past the command frame in one shot,
 * that trailing data is already sitting in the response buffer. Fixing
 * this to read from there, instead of a separate follow-up transfer
 * (which reasserts CS and gets nothing but 0xFF back), is what actually
 * fixed CMD8's R7 read -- confirmed live 2026-09-17: the raw response
 * bytes for CMD8 showed the real `00 00 01 AA` echo sitting right there
 * in the same buffer the old code was throwing away. */
static uint8_t sd_command_ext(uint8_t cmd, uint32_t arg, uint8_t crc, uint8_t *extra, uint32_t extra_len) {
    uint8_t frame[6 + SD_R1_POLL_BYTES] = {
        (uint8_t)(0x40 | cmd),
        (uint8_t)(arg >> 24), (uint8_t)(arg >> 16), (uint8_t)(arg >> 8), (uint8_t)arg,
        crc,
    };
    memset(&frame[6], 0xFF, SD_R1_POLL_BYTES);

    uint8_t response[sizeof(frame)];
    if (!sc18is602b_transfer(&g_sd_bus, frame, response, sizeof(frame))) {
        printf("  [sd] sd_command(0x%02X): I2C TRANSFER TO U5 ITSELF FAILED (not an SPI/card issue)\n", cmd);
        if (extra && extra_len) memset(extra, 0xFF, extra_len);
        return 0xFF;
    }
    if (!g_sd_quiet) {
        printf("  [sd] sd_command(0x%02X) raw response bytes:", cmd);
        for (uint32_t i = 6; i < sizeof(frame); i++) printf(" %02X", response[i]);
        printf("\n");
    }

    for (uint32_t i = 6; i < sizeof(frame); i++) {
        if ((response[i] & 0x80) == 0) {
            if (extra && extra_len) {
                uint32_t avail = (uint32_t)sizeof(frame) - (i + 1);
                uint32_t n = extra_len < avail ? extra_len : avail;
                memcpy(extra, &response[i + 1], n);
                if (n < extra_len) memset(extra + n, 0xFF, extra_len - n);
            }
            return response[i];
        }
    }
    if (extra && extra_len) memset(extra, 0xFF, extra_len);
    return 0xFF;
}

static uint8_t sd_command(uint8_t cmd, uint32_t arg, uint8_t crc) {
    return sd_command_ext(cmd, arg, crc, NULL, 0);
}

/* ACMD prefix -- CMD55 then the real command, per spec. */
static uint8_t sd_acommand(uint8_t acmd, uint32_t arg) {
    uint8_t r1 = sd_command(SD_CMD55_APP_CMD, 0, 0xFF);
    if (r1 & 0x80) return r1;
    return sd_command(acmd, arg, 0xFF);
}

/* CMD17's command frame, R1, and the 0xFE start token are captured
 * together in ONE continuous CS-low transfer (same technique as
 * sd_read_csd(), see this file's own top comment for why this can't be
 * extended to the whole 512-byte block on this chip) -- whatever real
 * data bytes land in that same chunk right after the token are used
 * immediately, and only the remainder is read via ordinary chunked
 * transfers. SD_READ_FIRST_CHUNK_MARGIN mirrors SD_CSD_TOKEN_WAIT_MARGIN
 * (16), sized a little more generously (real data starts consuming the
 * same budget the instant the token is found, so a few extra margin
 * bytes here directly become a few more real data bytes captured
 * CS-continuously instead of via a later chunk boundary). */
#define SD_READ_FIRST_CHUNK_MARGIN 24

static bool sd_read_block(uint32_t addr, uint8_t *buf, uint32_t len) {
    uint8_t first[6 + SD_READ_FIRST_CHUNK_MARGIN] = {
        (uint8_t)(0x40 | SD_CMD17_READ_SINGLE_BLOCK),
        (uint8_t)(addr >> 24), (uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr,
        0xFF,
    };
    memset(&first[6], 0xFF, SD_READ_FIRST_CHUNK_MARGIN);

    uint8_t response[sizeof(first)];
    if (!sc18is602b_transfer(&g_sd_bus, first, response, sizeof(first))) {
        printf("  [sd] sd_read_block: I2C TRANSFER TO U5 ITSELF FAILED (not an SPI/card issue)\n");
        return false;
    }
    printf("  [sd] sd_read_block(0x11) raw response bytes:");
    for (uint32_t i = 6; i < sizeof(first); i++) printf(" %02X", response[i]);
    printf("\n");

    uint32_t i = 6;
    for (; i < sizeof(first); i++) if ((response[i] & 0x80) == 0) break;
    if (i >= sizeof(first)) {
        printf("  [sd] sd_read_block: no R1 found\n");
        return false;
    }
    if (response[i] != 0x00) {
        printf("  [sd] sd_read_block: R1=0x%02X (want 0x00)\n", response[i]);
        return false;
    }

    i++;
    for (; i < sizeof(first); i++) if (response[i] == SD_TOKEN_START_BLOCK) break;
    if (i >= sizeof(first)) {
        /* Token hasn't shown up within this first chunk's margin --
         * rare (only expected on a card slower than
         * SD_READ_FIRST_CHUNK_MARGIN bytes' worth of clocks to prepare
         * the block); fall back to polling for it one byte at a time,
         * same technique the pre-2026-09-18 version of this function
         * used unconditionally. */
        uint32_t start = time_us_32();
        uint8_t token = 0xFF;
        do {
            if (!sc18is602b_transfer(&g_sd_bus, NULL, &token, 1)) return false;
            if (token == SD_TOKEN_START_BLOCK) break;
            if (token != 0xFF) return false; /* a data error token -- give up */
            sleep_us(SD_POLL_INTERVAL_US);
        } while (elapsed_us(start) < SD_READ_TIMEOUT_US);
        if (token != SD_TOKEN_START_BLOCK) {
            printf("  [sd] sd_read_block: no data start token found\n");
            return false;
        }
        if (!sc18is602b_transfer(&g_sd_bus, NULL, buf, len)) return false;
        uint8_t crc[2];
        return sc18is602b_transfer(&g_sd_bus, NULL, crc, sizeof(crc));
    }

    uint32_t avail = (uint32_t)sizeof(first) - (i + 1);
    uint32_t n = avail < len ? avail : len;
    memcpy(buf, &response[i + 1], n);

    uint32_t remaining = len - n;
    if (remaining > 0 && !sc18is602b_transfer(&g_sd_bus, NULL, buf + n, remaining)) return false;

    uint8_t crc[2];
    return sc18is602b_transfer(&g_sd_bus, NULL, crc, sizeof(crc));
}

/* CMD24's command frame, R1, and the write Start Block token (0xFE) are
 * sent together in ONE continuous CS-low transfer, same technique as
 * sd_read_block() -- but simpler, since a write's token doesn't need a
 * wait/search margin the way a read's does: we choose when to send it,
 * so it goes at a fixed, known offset right after a small R1-latency
 * margin, with the token position never in question. Whatever's left of
 * the 200-byte budget (SC18IS602B_MAX_CHUNK, shared from sc18is602b.h)
 * after the command+margin+token overhead is filled with real payload
 * bytes in that SAME chunk -- SD_WRITE_FIRST_CHUNK_PAYLOAD computes this
 * so the whole first request is guaranteed to land in exactly one
 * sc18is602b_transfer() chunk, not get silently re-split by it. The
 * remaining payload, CRC, and data-response token follow via ordinary
 * (necessarily CS-toggling past the 200-byte ceiling -- see this file's
 * own top comment) transfers, then the busy-wait poll same as before. */
#define SD_WRITE_R1_MARGIN 8
#define SD_WRITE_FIRST_CHUNK_PAYLOAD \
    (SC18IS602B_MAX_CHUNK - 6 - SD_WRITE_R1_MARGIN - 1)

static bool sd_write_block(uint32_t addr, const uint8_t *buf, uint32_t len) {
    uint32_t firstPayload = len < SD_WRITE_FIRST_CHUNK_PAYLOAD ? len : SD_WRITE_FIRST_CHUNK_PAYLOAD;
    uint8_t first[6 + SD_WRITE_R1_MARGIN + 1 + SD_WRITE_FIRST_CHUNK_PAYLOAD];
    uint32_t firstLen = 6 + SD_WRITE_R1_MARGIN + 1 + firstPayload;
    first[0] = (uint8_t)(0x40 | SD_CMD24_WRITE_BLOCK);
    first[1] = (uint8_t)(addr >> 24);
    first[2] = (uint8_t)(addr >> 16);
    first[3] = (uint8_t)(addr >> 8);
    first[4] = (uint8_t)addr;
    first[5] = 0xFF;
    memset(&first[6], 0xFF, SD_WRITE_R1_MARGIN);
    first[6 + SD_WRITE_R1_MARGIN] = SD_TOKEN_START_BLOCK;
    memcpy(&first[6 + SD_WRITE_R1_MARGIN + 1], buf, firstPayload);

    uint8_t response[sizeof(first)];
    if (!sc18is602b_transfer(&g_sd_bus, first, response, firstLen)) {
        printf("  [sd] sd_write_block: I2C TRANSFER TO U5 ITSELF FAILED (not an SPI/card issue)\n");
        return false;
    }
    printf("  [sd] sd_write_block(0x18) raw response bytes:");
    for (uint32_t i = 6; i < 6 + SD_WRITE_R1_MARGIN; i++) printf(" %02X", response[i]);
    printf("\n");

    uint32_t i = 6;
    for (; i < 6 + SD_WRITE_R1_MARGIN; i++) if ((response[i] & 0x80) == 0) break;
    if (i >= 6 + SD_WRITE_R1_MARGIN) {
        printf("  [sd] sd_write_block: no R1 found\n");
        return false;
    }
    if (response[i] != 0x00) {
        printf("  [sd] sd_write_block: R1=0x%02X (want 0x00)\n", response[i]);
        return false;
    }

    uint32_t remaining = len - firstPayload;
    if (remaining > 0 && !sc18is602b_transfer(&g_sd_bus, buf + firstPayload, NULL, remaining)) return false;

    /* CRC (dummy, unchecked -- CRC checking is never enabled, see this
     * file's own top comment) plus a small search margin for the
     * data-response token in one transfer -- spec says it comes
     * "immediately" after CRC, but this exact path was never confirmed
     * against real hardware (see this file's own top comment), and the
     * read side's own equivalent token needed real search margin in
     * practice, so don't assume a fixed byte position here either. */
#define SD_WRITE_RESPONSE_TOKEN_MARGIN 8
    uint8_t crcAndResp[2 + SD_WRITE_RESPONSE_TOKEN_MARGIN];
    memset(crcAndResp, 0xFF, sizeof(crcAndResp));
    uint8_t crcAndRespRx[sizeof(crcAndResp)];
    if (!sc18is602b_transfer(&g_sd_bus, crcAndResp, crcAndRespRx, sizeof(crcAndResp))) return false;

    uint32_t j = 2; /* skip the 2 CRC byte-times */
    for (; j < sizeof(crcAndRespRx); j++) if (crcAndRespRx[j] != 0xFF) break;
    if (j >= sizeof(crcAndRespRx)) {
        printf("  [sd] sd_write_block: no data response token found\n");
        return false;
    }
    if ((crcAndRespRx[j] & 0x1F) != 0x05) {
        printf("  [sd] sd_write_block: data response token=0x%02X (want low 5 bits = 0x05)\n",
               crcAndRespRx[j]);
        return false; /* not "data accepted" */
    }

    uint32_t start = time_us_32();
    uint8_t busy = 0x00;
    do {
        if (!sc18is602b_transfer(&g_sd_bus, NULL, &busy, 1)) return false;
        if (busy != 0x00) return true;
        sleep_us(SD_POLL_INTERVAL_US);
    } while (elapsed_us(start) < SD_WRITE_BUSY_TIMEOUT_US);
    return false; /* still busy -- timed out */
}

/* CMD9's command frame, R1, the 0xFE data start token, and the 16-byte
 * CSD payload all fit comfortably within the SC18IS602B's own 200-byte
 * internal SPI buffer (6 + 16 + 16 + 2 = 40 bytes here), so -- unlike a
 * full 512-byte CMD17 data block, which can't fit regardless of chunk
 * size -- this can be captured as ONE genuinely continuous CS-low
 * transfer. Same fix as sd_command_ext()'s R7/OCR trailing-byte read:
 * confirmed live 2026-09-17 that the OLD separate-transfer
 * sd_read_data_block() call here was throwing away a buffer that
 * already contained the 0xFE token and real CSD bytes (raw response
 * `FF 00 FF FE 00 7F FF 32 5F 59 83 CB 76 DB DF FF` -- R1 at index 1,
 * token at index 3), and disk_initialize() was failing right here as a
 * result. */
#define SD_CSD_TOKEN_WAIT_MARGIN 16

static bool sd_read_csd(uint8_t csd_out[16]) {
    uint8_t frame[6 + SD_CSD_TOKEN_WAIT_MARGIN + 16 + 2] = {
        (uint8_t)(0x40 | SD_CMD9_SEND_CSD), 0, 0, 0, 0, 0xFF,
    };
    memset(&frame[6], 0xFF, sizeof(frame) - 6);

    uint8_t response[sizeof(frame)];
    if (!sc18is602b_transfer(&g_sd_bus, frame, response, sizeof(frame))) {
        printf("  [sd] sd_command(0x09): I2C TRANSFER TO U5 ITSELF FAILED (not an SPI/card issue)\n");
        return false;
    }
    printf("  [sd] sd_command(0x09) raw response bytes:");
    for (uint32_t i = 6; i < sizeof(frame); i++) printf(" %02X", response[i]);
    printf("\n");

    uint32_t i = 6;
    for (; i < sizeof(frame); i++) if ((response[i] & 0x80) == 0) break;
    if (i >= sizeof(frame)) {
        printf("  [sd] CMD9 (SEND_CSD): no R1 found\n");
        return false;
    }
    uint8_t r1 = response[i];
    printf("  [sd] CMD9 (SEND_CSD) -> R1=0x%02X\n", r1);
    if (r1 != 0x00) return false;

    i++;
    for (; i < sizeof(frame); i++) if (response[i] == SD_TOKEN_START_BLOCK) break;
    if (i >= sizeof(frame)) {
        printf("  [sd] CMD9: no data start token found in captured window\n");
        return false;
    }

    uint32_t avail = (uint32_t)sizeof(frame) - (i + 1);
    if (avail < 16) {
        printf("  [sd] CMD9: start token found too late, only %lu bytes captured after it\n", (unsigned long)avail);
        return false;
    }
    memcpy(csd_out, &response[i + 1], 16);
    return true;
}

/* Computes the card's total sector count from the CSD register (CMD9).
 * Handles both CSD structure versions: 2.0 (SDHC/SDXC -- simple
 * C_SIZE-based capacity) and 1.0 (SDSC -- C_SIZE/C_SIZE_MULT/
 * READ_BL_LEN formula). See the SD Physical Layer Simplified
 * Specification's own CSD register section for these exact bit fields
 * -- not re-derived here, just implemented as documented. */
static bool sd_read_capacity(void) {
    uint8_t csd[16];
    if (!sd_read_csd(csd)) return false;

    uint8_t csd_structure = (uint8_t)(csd[0] >> 6);
    if (csd_structure == 1) {
        /* CSD v2.0 (SDHC/SDXC): C_SIZE is bits [69:48], 22 bits wide. */
        uint32_t c_size = (((uint32_t)csd[7] & 0x3F) << 16) | ((uint32_t)csd[8] << 8) | csd[9];
        g_sd_sector_count = (c_size + 1u) * 1024u; /* (C_SIZE+1)*512KB / 512B-per-sector */
    } else {
        /* CSD v1.0 (SDSC): C_SIZE [73:62] (12 bits), C_SIZE_MULT [49:47]
         * (3 bits), READ_BL_LEN [83:80] (4 bits). */
        uint32_t c_size = (((uint32_t)csd[6] & 0x03) << 10) | ((uint32_t)csd[7] << 2) |
                           (((uint32_t)csd[8] >> 6) & 0x03);
        uint32_t c_size_mult = (((uint32_t)csd[9] & 0x03) << 1) | ((csd[10] >> 7) & 0x01);
        uint32_t read_bl_len = csd[5] & 0x0F;
        uint32_t block_len = 1u << read_bl_len;
        uint32_t mult = 1u << (c_size_mult + 2u);
        uint32_t capacity_bytes = (c_size + 1u) * mult * block_len;
        g_sd_sector_count = capacity_bytes / 512u;
    }
    return g_sd_sector_count > 0;
}

/* Card hot-swap (2026-09-25) -- there's no card-detect pin, so this asks
 * the card itself. FatFs calls disk_status() at the start of every file
 * operation (mount_volume() for path calls, validate() for open handles),
 * so a CMD13 (SEND_STATUS) here catches a removed or swapped card before
 * anything touches it: a removed card doesn't answer (0xFF), and a newly
 * inserted one hasn't been put into SPI mode yet, so it doesn't answer
 * either, or reports the idle bit if it did get a CMD0. Either way this
 * returns STA_NOINIT, FatFs re-mounts through disk_initialize() on the
 * next path call, and handles opened on the old card fail with
 * FR_INVALID_OBJECT instead of reading the wrong card.
 * monitor_sd_card_changed() drops monitor.c's own open-file bookkeeping to
 * match. Costs one short SPI exchange through the bridge per file
 * operation. */
DSTATUS disk_status(BYTE pdrv) {
    (void)pdrv;
    if (!g_sd_initialized) return STA_NOINIT;
    uint8_t r2;
    g_sd_quiet = true;
    uint8_t r1 = sd_command_ext(SD_CMD13_SEND_STATUS, 0, 0xFF, &r2, 1);
    g_sd_quiet = false;
    if (r1 == 0xFF || (r1 & SD_R1_IDLE_STATE)) {
        g_sd_initialized = false;
        mcu_log_info("SD card removed/changed");
        monitor_sd_card_changed();
        return STA_NOINIT;
    }
    return 0;
}

/* Logs a disk_initialize() failure to MLOG -- only when the failing step
 * differs from the last one logged, so an empty slot doesn't write flash
 * on every SD command. A successful init resets it. */
static const char *g_sd_last_init_fail = NULL;
static DSTATUS sd_init_failed(const char *why) {
    if (why != g_sd_last_init_fail) {
        mcu_log_error(why);
        g_sd_last_init_fail = why;
    }
    return STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv) {
    (void)pdrv;
    g_sd_initialized = false;
    g_sd_is_sdhc = false;
    g_sd_sector_count = 0;

    g_sd_bus.sda_gpio = PIN_GREENPAK1_SDA;
    g_sd_bus.scl_gpio = PIN_GREENPAK1_SCL;
    greenpak_i2c_init(&g_sd_bus); /* idempotent -- shared bus, GreenPAKs init it too */

    if (!sc18is602b_configure(&g_sd_bus, SC18IS602B_MODE_CPOL0_CPHA0, SC18IS602B_CLK_58KHZ)) {
        printf("  [sd] sc18is602b_configure() FAILED -- no I2C ACK from U5 at 0x28\n");
        return sd_init_failed("SD bridge no answer");
    }
    printf("  [sd] sc18is602b_configure() OK -- U5 ACKed\n");

    /* >=74 dummy clocks (>=10 bytes) with CS actually deasserted, per
     * spec -- see sc18is602b_clock_only()'s own comment for how, and why
     * this matters: without it, this card never responded to anything,
     * confirmed live 2026-09-17. */
    if (!sc18is602b_clock_only(&g_sd_bus, 10)) {
        printf("  [sd] dummy-clock transfer FAILED\n");
        return sd_init_failed("SD dummy clocks failed");
    }

    uint32_t dbgDummy1, dbgDummy2;
    sc18is602b_get_and_reset_retry_stats(&dbgDummy1, &dbgDummy2); /* clear preamble's own noise */
    uint32_t dbgImm, dbgWaited, dbgTimeout;
    sc18is602b_get_and_reset_int_stats(&dbgImm, &dbgWaited, &dbgTimeout);

    uint8_t r1 = 0xFF;
    for (int i = 0; i < 10 && r1 != SD_R1_IDLE_STATE; i++) {
        r1 = sd_command(SD_CMD0_GO_IDLE_STATE, 0, 0x95);
    }
    printf("  [sd] CMD0 (GO_IDLE_STATE) -> R1=0x%02X (want 0x01)\n", r1);
    sc18is602b_get_and_reset_int_stats(&dbgImm, &dbgWaited, &dbgTimeout);
    printf("  [sd] CMD0 attempt(s) INT imm/wait/timeout: %lu/%lu/%lu\n",
           (unsigned long)dbgImm, (unsigned long)dbgWaited, (unsigned long)dbgTimeout);
    if (r1 != SD_R1_IDLE_STATE) return sd_init_failed("SD no card (CMD0)"); /* or not responding */

    bool is_v2 = false;
    uint8_t r7[4];
    r1 = sd_command_ext(SD_CMD8_SEND_IF_COND, 0x1AA, 0x87, r7, sizeof(r7));
    printf("  [sd] CMD8 (SEND_IF_COND) -> R1=0x%02X\n", r1);
    if ((r1 & SD_R1_ILLEGAL_CMD) == 0) {
        printf("  [sd] CMD8 R7 trailing bytes: %02X %02X %02X %02X (want ..01 AA)\n",
               r7[0], r7[1], r7[2], r7[3]);
        if (r7[2] != 0x01 || r7[3] != 0xAA) return sd_init_failed("SD CMD8 bad echo"); /* voltage mismatch */
        is_v2 = true;
    } else {
        printf("  [sd] CMD8 not recognized -- treating as SD v1\n");
    }

    uint32_t start = time_us_32();
    int acmd41_tries = 0;
    do {
        r1 = sd_acommand(SD_ACMD41_SD_SEND_OP_COND, is_v2 ? 0x40000000u : 0);
        acmd41_tries++;
    } while (r1 == SD_R1_IDLE_STATE && elapsed_us(start) < SD_INIT_TIMEOUT_US);
    printf("  [sd] ACMD41 (SD_SEND_OP_COND) -> R1=0x%02X after %d tries, %lu us\n",
           r1, acmd41_tries, (unsigned long)elapsed_us(start));
    if (r1 != 0x00) return sd_init_failed("SD ACMD41 timed out"); /* never left idle state */

    if (is_v2) {
        uint8_t ocr[4];
        r1 = sd_command_ext(SD_CMD58_READ_OCR, 0, 0xFF, ocr, sizeof(ocr));
        printf("  [sd] CMD58 (READ_OCR) -> R1=0x%02X\n", r1);
        if (r1 & 0x80) return sd_init_failed("SD CMD58 failed");
        printf("  [sd] OCR: %02X %02X %02X %02X (CCS bit = %d)\n",
               ocr[0], ocr[1], ocr[2], ocr[3], (ocr[0] & 0x40) != 0);
        g_sd_is_sdhc = (ocr[0] & 0x40) != 0; /* CCS bit */
    }
    if (!g_sd_is_sdhc) {
        /* SDSC (or a v1 card that never confirmed HCS) -- fix the block
         * length explicitly; SDHC/SDXC are always fixed at 512 bytes. */
        if (sd_command(SD_CMD16_SET_BLOCKLEN, 512, 0xFF) != 0x00) return sd_init_failed("SD CMD16 failed");
    }

    if (!sd_read_capacity()) return sd_init_failed("SD CSD read failed");

    if (!sc18is602b_configure(&g_sd_bus, SC18IS602B_MODE_CPOL0_CPHA0, SC18IS602B_CLK_1843KHZ)) {
        return sd_init_failed("SD clock switch failed");
    }

    g_sd_last_init_fail = NULL;
    g_sd_initialized = true;
    return 0;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    (void)pdrv;
    if (!g_sd_initialized) return RES_NOTRDY;

    for (UINT i = 0; i < count; i++) {
        uint32_t addr = g_sd_is_sdhc ? (uint32_t)(sector + i) : (uint32_t)(sector + i) * 512u;
        if (!sd_read_block(addr, buff + (size_t)i * 512, 512)) return RES_ERROR;
    }
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    (void)pdrv;
    if (!g_sd_initialized) return RES_NOTRDY;

    for (UINT i = 0; i < count; i++) {
        uint32_t addr = g_sd_is_sdhc ? (uint32_t)(sector + i) : (uint32_t)(sector + i) * 512u;
        if (!sd_write_block(addr, buff + (size_t)i * 512, 512)) return RES_ERROR;
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    (void)pdrv;
    if (!g_sd_initialized) return RES_NOTRDY;

    switch (cmd) {
        case CTRL_SYNC:
            return RES_OK; /* every write above already blocks until the card is done */
        case GET_SECTOR_COUNT:
            *(LBA_t *)buff = g_sd_sector_count;
            return RES_OK;
        case GET_SECTOR_SIZE:
            *(WORD *)buff = 512;
            return RES_OK;
        case GET_BLOCK_SIZE:
            *(DWORD *)buff = 1; /* erase block size unknown -- FatFs's documented safe default */
            return RES_OK;
        default:
            return RES_PARERR;
    }
}
