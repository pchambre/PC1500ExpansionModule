/* qmi_cs1_sd.h
 *
 * SD card SPI-mode protocol driver, running over qmi_cs1_spi.h's
 * QMI-CS1-based transport instead of a normal hardware SPI peripheral.
 * Implements just enough of the SD Physical Layer spec's SPI mode to
 * back a FatFs block device: CMD0/CMD8 reset+interface check,
 * CMD55+ACMD41 initialization, CMD58 OCR/CCS check, CMD9 CSD read (for
 * capacity), CMD17/CMD24 single-block read/write.
 *
 * Single card, fixed to the GPIO configured via qmi_cs1_spi_init() at
 * sd_init() time -- this project only ever has one SD card, so there's
 * no card-selection abstraction here (unlike vendored multi-card
 * libraries).
 *
 * See qmi_cs1_spi.h and this directory's README.md for the QMI-CS1
 * transport this rides on, including the "unverified against real
 * hardware" caveat that applies equally here.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SD_CARD_TYPE_NONE = 0,
    SD_CARD_TYPE_SDSC,  /* byte-addressed -- CMD17/24 args are byte offsets */
    SD_CARD_TYPE_SDHC,  /* block-addressed (SDHC/SDXC) -- CMD17/24 args are 512-byte block numbers */
} sd_card_type_t;

/* Runs the full SPI-mode init sequence (>=74 dummy clocks with CS
 * released, CMD0, CMD8, CMD55+ACMD41 polling with timeout, CMD58 for
 * CCS, CMD16 SET_BLOCKLEN for SDSC cards) at the slow init clock rate,
 * then switches to `operating_baud_hz` for all subsequent transfers.
 * `sys_clk_hz` is the current clk_sys frequency, needed to compute the
 * QMI clock divisor. Returns false if the card doesn't respond or init
 * times out. Must be called after qmi_cs1_spi_init(). */
bool sd_init(uint32_t sys_clk_hz, uint32_t operating_baud_hz);

sd_card_type_t sd_get_card_type(void);

/* Total capacity in 512-byte sectors, from the card's CSD register
 * (read once during sd_init() and cached). 0 if sd_init() hasn't
 * succeeded. */
uint32_t sd_get_sector_count(void);

/* Read/write exactly one 512-byte sector. `sector` is a 512-byte block
 * number regardless of card type -- SDSC's byte-offset addressing is
 * handled internally (sector * 512). Returns false on any protocol-level
 * failure (no data token, bad write response, timeout). */
bool sd_read_sector(uint32_t sector, uint8_t buf[512]);
bool sd_write_sector(uint32_t sector, const uint8_t buf[512]);

#ifdef __cplusplus
}
#endif
