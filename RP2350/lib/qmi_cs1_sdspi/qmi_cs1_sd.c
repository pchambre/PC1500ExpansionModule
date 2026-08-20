/* qmi_cs1_sd.c -- see qmi_cs1_sd.h.
 *
 * Standard SD Physical Layer SPI-mode sequence (widely documented --
 * this follows the same shape as e.g. ChaN's own reference MMC/SD SPI
 * driver): >=74 init clocks with CS released, CMD0 to reset into idle
 * state, CMD8 to distinguish SDv2 (and get the voltage/check-pattern
 * echo) from SDv1/MMC, CMD55+ACMD41 polled until the card leaves idle
 * state, CMD58 to read OCR/CCS (block- vs byte-addressed), CMD16 to fix
 * the block length at 512 for SDSC cards, then CMD9 to read the CSD
 * register for capacity.
 *
 * Timeouts here are bounded retry-loop counts, not wall-clock timers --
 * kept simple deliberately (no pico_time dependency, no concern about
 * whether a timer read is safely RAM-resident during a QMI direct-mode
 * transaction). These counts are generous relative to typical SD command
 * timing at the driver's slow init clock, but have not been calibrated
 * against a real card -- see the module README's hardware-bring-up note.
 *
 * RAM RESIDENCY: every function here that runs code between a
 * qmi_cs1_spi_transaction_begin()/end() pair is marked
 * __not_in_flash_func, not just qmi_cs1_spi.c's own primitives. Reason:
 * once direct mode is enabled, ANY flash instruction fetch stalls
 * (see qmi_cs1_spi.h's top comment) -- including fetching the next
 * instruction of the loop that's driving the transfer, which would
 * stall waiting for QMI to be free, while QMI is waiting for that same
 * code to issue its next transfer. Marking whole functions RAM-resident
 * (rather than trying to carve out exactly the code between begin/end)
 * is the simple, obviously-correct choice here -- the RAM cost (roughly
 * a KB of code) is trivial against RP2350's 520KB SRAM.
 */
#include "qmi_cs1_sd.h"
#include "qmi_cs1_spi.h"
#include "pico.h" /* pulls in pico/platform.h correctly -- including it directly errors out */

#define CMD0  0   /* GO_IDLE_STATE */
#define CMD8  8   /* SEND_IF_COND */
#define CMD9  9   /* SEND_CSD */
#define CMD16 16  /* SET_BLOCKLEN */
#define CMD17 17  /* READ_SINGLE_BLOCK */
#define CMD24 24  /* WRITE_BLOCK */
#define CMD55 55  /* APP_CMD */
#define CMD58 58  /* READ_OCR */
#define ACMD41 41 /* SD_SEND_OP_COND (after CMD55) */

#define R1_IDLE_STATE      0x01u
#define R1_ILLEGAL_COMMAND 0x04u

static sd_card_type_t g_card_type = SD_CARD_TYPE_NONE;
static uint32_t g_sector_count = 0;

static uint8_t __not_in_flash_func(crc7)(const uint8_t *data, int len) {
    uint8_t crc = 0;
    for (int i = 0; i < len; i++) {
        uint8_t d = data[i];
        for (int b = 0; b < 8; b++) {
            crc <<= 1;
            if ((d ^ crc) & 0x80u) crc ^= 0x09u;
            d <<= 1;
        }
    }
    return (uint8_t)((crc << 1) | 1u);
}

/* Sends a 6-byte command frame and polls for the R1 response byte
 * (MSB clear). Must be called with a transaction already begun (CS1
 * asserted). Does not end the transaction -- callers that need trailing
 * response bytes (R7/R3) or a data phase keep the transaction open. */
static uint8_t __not_in_flash_func(sd_cmd)(uint8_t cmd, uint32_t arg) {
    uint8_t frame[6];
    frame[0] = (uint8_t)(0x40u | cmd);
    frame[1] = (uint8_t)(arg >> 24);
    frame[2] = (uint8_t)(arg >> 16);
    frame[3] = (uint8_t)(arg >> 8);
    frame[4] = (uint8_t)(arg);
    frame[5] = crc7(frame, 5);
    for (int i = 0; i < 6; i++) qmi_cs1_spi_transfer_byte(frame[i]);

    uint8_t r1 = 0xFFu;
    for (int i = 0; i < 10; i++) {
        r1 = qmi_cs1_spi_read_byte();
        if ((r1 & 0x80u) == 0) break;
    }
    return r1;
}

/* CMD55 (APP_CMD) followed by the given ACMD, as one logical operation
 * spanning two command frames within the same transaction. */
static uint8_t __not_in_flash_func(sd_acmd)(uint8_t acmd, uint32_t arg) {
    uint8_t r1 = sd_cmd(CMD55, 0);
    if (r1 & R1_ILLEGAL_COMMAND) return r1;
    return sd_cmd(acmd, arg);
}

static bool __not_in_flash_func(sd_read_csd)(uint8_t csd[16]) {
    qmi_cs1_spi_transaction_begin();
    qmi_cs1_spi_read_byte(); /* NCS setup pad */
    uint8_t r1 = sd_cmd(CMD9, 0);
    bool ok = false;
    if (r1 == 0x00u) {
        uint8_t token = 0xFFu;
        for (int i = 0; i < 20000 && token == 0xFFu; i++) token = qmi_cs1_spi_read_byte();
        if (token == 0xFEu) {
            for (int i = 0; i < 16; i++) csd[i] = qmi_cs1_spi_read_byte();
            qmi_cs1_spi_read_byte();
            qmi_cs1_spi_read_byte(); /* trailing CRC16, discarded */
            ok = true;
        }
    }
    qmi_cs1_spi_transaction_end();
    return ok;
}

static uint32_t csd_to_sector_count(const uint8_t csd[16], sd_card_type_t type) {
    if (type == SD_CARD_TYPE_SDHC) {
        /* CSD version 2.0: C_SIZE is a 22-bit field across csd[7..9]. */
        uint32_t c_size = (((uint32_t)csd[7] & 0x3Fu) << 16) | ((uint32_t)csd[8] << 8) | csd[9];
        return (c_size + 1u) * 1024u; /* each unit = 512KiB = 1024 sectors of 512B */
    } else {
        /* CSD version 1.0 (SDSC): standard READ_BL_LEN/C_SIZE/C_SIZE_MULT formula. */
        uint32_t read_bl_len = csd[5] & 0x0Fu;
        uint32_t c_size = ((uint32_t)(csd[6] & 0x03u) << 10) | ((uint32_t)csd[7] << 2) | (csd[8] >> 6);
        uint32_t c_size_mult = ((uint32_t)(csd[9] & 0x03u) << 1) | (csd[10] >> 7);
        uint32_t block_nr = (c_size + 1u) << (c_size_mult + 2u);
        uint32_t block_len = 1u << read_bl_len;
        return (block_nr * block_len) / 512u;
    }
}

bool __not_in_flash_func(sd_init)(uint32_t sys_clk_hz, uint32_t operating_baud_hz) {
    g_card_type = SD_CARD_TYPE_NONE;
    g_sector_count = 0;

    qmi_cs1_spi_send_init_clocks(10); /* >=74 clocks (10 bytes = 80) with CS released */

    qmi_cs1_spi_transaction_begin();
    qmi_cs1_spi_read_byte();
    uint8_t r1 = sd_cmd(CMD0, 0);
    qmi_cs1_spi_transaction_end();
    if (r1 != R1_IDLE_STATE) return false; /* card didn't respond / isn't in SPI mode */

    qmi_cs1_spi_transaction_begin();
    qmi_cs1_spi_read_byte();
    r1 = sd_cmd(CMD8, 0x000001AAu);
    bool is_v2 = false;
    if (r1 == R1_IDLE_STATE) {
        uint8_t r7[4];
        for (int i = 0; i < 4; i++) r7[i] = qmi_cs1_spi_read_byte();
        is_v2 = (r7[2] == 0x01u && r7[3] == 0xAAu);
    }
    qmi_cs1_spi_transaction_end();
    /* r1 with R1_ILLEGAL_COMMAND set (or a non-idle failure) means an
     * SDv1 or MMC card that doesn't understand CMD8 -- proceed without
     * the HCS (high-capacity) hint in ACMD41 below; these old cards are
     * always SDSC-style byte-addressed. */

    bool acmd41_ready = false;
    for (int i = 0; i < 2000 && !acmd41_ready; i++) {
        qmi_cs1_spi_transaction_begin();
        qmi_cs1_spi_read_byte();
        uint8_t ar1 = sd_acmd(ACMD41, is_v2 ? 0x40000000u : 0x00000000u);
        qmi_cs1_spi_transaction_end();
        if (ar1 == 0x00u) acmd41_ready = true;
        else if (ar1 & 0xFEu) break; /* real error bits, not just "still idle" */
    }
    if (!acmd41_ready) return false;

    if (is_v2) {
        qmi_cs1_spi_transaction_begin();
        qmi_cs1_spi_read_byte();
        r1 = sd_cmd(CMD58, 0);
        uint8_t ocr[4] = {0, 0, 0, 0};
        if (r1 == 0x00u) {
            for (int i = 0; i < 4; i++) ocr[i] = qmi_cs1_spi_read_byte();
        }
        qmi_cs1_spi_transaction_end();
        g_card_type = (ocr[0] & 0x40u) ? SD_CARD_TYPE_SDHC : SD_CARD_TYPE_SDSC;
    } else {
        g_card_type = SD_CARD_TYPE_SDSC;
    }

    if (g_card_type == SD_CARD_TYPE_SDSC) {
        qmi_cs1_spi_transaction_begin();
        qmi_cs1_spi_read_byte();
        sd_cmd(CMD16, 512);
        qmi_cs1_spi_transaction_end();
    }

    uint8_t csd[16];
    if (sd_read_csd(csd)) {
        g_sector_count = csd_to_sector_count(csd, g_card_type);
    }

    qmi_cs1_spi_set_baudrate(sys_clk_hz, operating_baud_hz);
    return g_sector_count > 0;
}

sd_card_type_t sd_get_card_type(void) { return g_card_type; }
uint32_t sd_get_sector_count(void) { return g_sector_count; }

bool __not_in_flash_func(sd_read_sector)(uint32_t sector, uint8_t buf[512]) {
    uint32_t addr = (g_card_type == SD_CARD_TYPE_SDHC) ? sector : sector * 512u;
    qmi_cs1_spi_transaction_begin();
    qmi_cs1_spi_read_byte();
    uint8_t r1 = sd_cmd(CMD17, addr);
    bool ok = false;
    if (r1 == 0x00u) {
        uint8_t token = 0xFFu;
        for (int i = 0; i < 100000 && token == 0xFFu; i++) token = qmi_cs1_spi_read_byte();
        if (token == 0xFEu) {
            for (int i = 0; i < 512; i++) buf[i] = qmi_cs1_spi_read_byte();
            qmi_cs1_spi_read_byte();
            qmi_cs1_spi_read_byte(); /* trailing CRC16, discarded */
            ok = true;
        }
    }
    qmi_cs1_spi_transaction_end();
    return ok;
}

bool __not_in_flash_func(sd_write_sector)(uint32_t sector, const uint8_t buf[512]) {
    uint32_t addr = (g_card_type == SD_CARD_TYPE_SDHC) ? sector : sector * 512u;
    qmi_cs1_spi_transaction_begin();
    qmi_cs1_spi_read_byte();
    uint8_t r1 = sd_cmd(CMD24, addr);
    bool ok = false;
    if (r1 == 0x00u) {
        qmi_cs1_spi_transfer_byte(0xFEu); /* start token */
        for (int i = 0; i < 512; i++) qmi_cs1_spi_transfer_byte(buf[i]);
        qmi_cs1_spi_transfer_byte(0xFFu); /* dummy CRC16 (CRC checking not enabled) */
        qmi_cs1_spi_transfer_byte(0xFFu);
        uint8_t resp = qmi_cs1_spi_read_byte();
        if ((resp & 0x1Fu) == 0x05u) {
            uint8_t busy = 0x00u;
            for (int i = 0; i < 200000 && busy == 0x00u; i++) busy = qmi_cs1_spi_read_byte();
            ok = (busy != 0x00u);
        }
    }
    qmi_cs1_spi_transaction_end();
    return ok;
}
