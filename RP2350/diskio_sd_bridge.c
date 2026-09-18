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
 * command+R1+token-wait+512-byte data block+CRC (CMD17/CMD24, via
 * sd_read_data_block()/sd_write_data_block() below) is ~530+ bytes --
 * it literally cannot be captured as one continuous CS-low session on
 * this chip regardless of chunk size, unlike CMD9's 16-byte CSD. Those
 * two functions still use separate calls per step; whether a real card
 * tolerates the resulting brief CS-high gaps mid-block on THIS bridge
 * (as opposed to a dedicated hardware_spi peripheral that never
 * releases CS at all) is not yet confirmed against hardware -- test
 * disk_read()/disk_write() specifically before trusting file I/O, even
 * though disk_initialize() no longer depends on it.
 */
#include "ff.h"
#include "diskio.h"

#include <stdio.h>
#include <string.h>
#include "pico/time.h"

#include "board_pins.h"
#include "sc18is602b.h"

#define SD_CMD0_GO_IDLE_STATE        0
#define SD_CMD8_SEND_IF_COND         8
#define SD_CMD9_SEND_CSD             9
#define SD_CMD12_STOP_TRANSMISSION   12
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

static greenpak_i2c_bus_t g_sd_bus;
static bool g_sd_initialized = false;
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
    printf("  [sd] sd_command(0x%02X) raw response bytes:", cmd);
    for (uint32_t i = 6; i < sizeof(frame); i++) printf(" %02X", response[i]);
    printf("\n");

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

/* Waits for the data start token (0xFE), then reads exactly `len` data
 * bytes plus the trailing 2 (unchecked) CRC bytes. */
static bool sd_read_data_block(uint8_t *buf, uint32_t len) {
    uint32_t start = time_us_32();
    uint8_t token = 0xFF;
    do {
        if (!sc18is602b_transfer(&g_sd_bus, NULL, &token, 1)) return false;
        if (token == SD_TOKEN_START_BLOCK) break;
        if (token != 0xFF) return false; /* a data error token -- give up */
    } while (elapsed_us(start) < SD_READ_TIMEOUT_US);
    if (token != SD_TOKEN_START_BLOCK) return false;

    if (!sc18is602b_transfer(&g_sd_bus, NULL, buf, len)) return false;
    uint8_t crc[2];
    return sc18is602b_transfer(&g_sd_bus, NULL, crc, sizeof(crc));
}

/* Sends one data block (start token + payload + dummy CRC), checks the
 * data-response token, then waits out the card's internal write-busy
 * period (MISO held low until it's done, per spec -- polled here as
 * repeated 0xFF reads until a non-zero byte comes back). */
static bool sd_write_data_block(const uint8_t *buf, uint32_t len) {
    uint8_t token = SD_TOKEN_START_BLOCK;
    if (!sc18is602b_transfer(&g_sd_bus, &token, NULL, 1)) return false;
    if (!sc18is602b_transfer(&g_sd_bus, buf, NULL, len)) return false;
    uint8_t crc[2] = { 0xFF, 0xFF };
    if (!sc18is602b_transfer(&g_sd_bus, crc, NULL, sizeof(crc))) return false;

    uint8_t resp = 0xFF;
    if (!sc18is602b_transfer(&g_sd_bus, NULL, &resp, 1)) return false;
    if ((resp & 0x1F) != 0x05) return false; /* not "data accepted" */

    uint32_t start = time_us_32();
    uint8_t busy = 0x00;
    do {
        if (!sc18is602b_transfer(&g_sd_bus, NULL, &busy, 1)) return false;
        if (busy != 0x00) return true;
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

DSTATUS disk_status(BYTE pdrv) {
    (void)pdrv;
    return g_sd_initialized ? 0 : STA_NOINIT;
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
        return STA_NOINIT;
    }
    printf("  [sd] sc18is602b_configure() OK -- U5 ACKed\n");

    /* >=74 dummy clocks (>=10 bytes) with CS actually deasserted, per
     * spec -- see sc18is602b_clock_only()'s own comment for how, and why
     * this matters: without it, this card never responded to anything,
     * confirmed live 2026-09-17. */
    if (!sc18is602b_clock_only(&g_sd_bus, 10)) {
        printf("  [sd] dummy-clock transfer FAILED\n");
        return STA_NOINIT;
    }

    uint8_t r1 = 0xFF;
    for (int i = 0; i < 10 && r1 != SD_R1_IDLE_STATE; i++) {
        r1 = sd_command(SD_CMD0_GO_IDLE_STATE, 0, 0x95);
    }
    printf("  [sd] CMD0 (GO_IDLE_STATE) -> R1=0x%02X (want 0x01)\n", r1);
    if (r1 != SD_R1_IDLE_STATE) return STA_NOINIT; /* no card, or not responding */

    bool is_v2 = false;
    uint8_t r7[4];
    r1 = sd_command_ext(SD_CMD8_SEND_IF_COND, 0x1AA, 0x87, r7, sizeof(r7));
    printf("  [sd] CMD8 (SEND_IF_COND) -> R1=0x%02X\n", r1);
    if ((r1 & SD_R1_ILLEGAL_CMD) == 0) {
        printf("  [sd] CMD8 R7 trailing bytes: %02X %02X %02X %02X (want ..01 AA)\n",
               r7[0], r7[1], r7[2], r7[3]);
        if (r7[2] != 0x01 || r7[3] != 0xAA) return STA_NOINIT; /* voltage mismatch */
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
    if (r1 != 0x00) return STA_NOINIT; /* never left idle state -- init failed */

    if (is_v2) {
        uint8_t ocr[4];
        r1 = sd_command_ext(SD_CMD58_READ_OCR, 0, 0xFF, ocr, sizeof(ocr));
        printf("  [sd] CMD58 (READ_OCR) -> R1=0x%02X\n", r1);
        if (r1 & 0x80) return STA_NOINIT;
        printf("  [sd] OCR: %02X %02X %02X %02X (CCS bit = %d)\n",
               ocr[0], ocr[1], ocr[2], ocr[3], (ocr[0] & 0x40) != 0);
        g_sd_is_sdhc = (ocr[0] & 0x40) != 0; /* CCS bit */
    }
    if (!g_sd_is_sdhc) {
        /* SDSC (or a v1 card that never confirmed HCS) -- fix the block
         * length explicitly; SDHC/SDXC are always fixed at 512 bytes. */
        if (sd_command(SD_CMD16_SET_BLOCKLEN, 512, 0xFF) != 0x00) return STA_NOINIT;
    }

    if (!sd_read_capacity()) return STA_NOINIT;

    if (!sc18is602b_configure(&g_sd_bus, SC18IS602B_MODE_CPOL0_CPHA0, SC18IS602B_CLK_1843KHZ)) {
        return STA_NOINIT;
    }

    g_sd_initialized = true;
    return 0;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    (void)pdrv;
    if (!g_sd_initialized) return RES_NOTRDY;

    for (UINT i = 0; i < count; i++) {
        uint32_t addr = g_sd_is_sdhc ? (uint32_t)(sector + i) : (uint32_t)(sector + i) * 512u;
        uint8_t r1 = sd_command(SD_CMD17_READ_SINGLE_BLOCK, addr, 0xFF);
        if (r1 != 0x00) return RES_ERROR;
        if (!sd_read_data_block(buff + (size_t)i * 512, 512)) return RES_ERROR;
    }
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    (void)pdrv;
    if (!g_sd_initialized) return RES_NOTRDY;

    for (UINT i = 0; i < count; i++) {
        uint32_t addr = g_sd_is_sdhc ? (uint32_t)(sector + i) : (uint32_t)(sector + i) * 512u;
        uint8_t r1 = sd_command(SD_CMD24_WRITE_BLOCK, addr, 0xFF);
        if (r1 != 0x00) return RES_ERROR;
        if (!sd_write_data_block(buff + (size_t)i * 512, 512)) return RES_ERROR;
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
