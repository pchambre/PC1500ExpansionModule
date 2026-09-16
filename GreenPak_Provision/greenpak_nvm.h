/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* greenpak_nvm.h
 *
 * SLG46826 NVM (chip configuration) programming over I2C (datasheet
 * rev 3.25 section 16, and section 14 for the POR/reload behavior
 * this module's reset_and_reload depends on).
 *
 * NVM is 2048 bits = 16 pages of 16 bytes. The 7-bit I2C address is
 * [4-bit control code][3-bit block address]; every function here takes
 * an explicit `control_code` rather than assuming a fixed chip address,
 * since GreenPAK1 and GreenPAK2 share one physical bus and must be
 * addressed distinctly (see board_pins.h). Block `000` is the runtime
 * register space (used for the Erase Register, Byte Write Bit Masking,
 * and the I2C Serial Reset Command); block `010` is the NVM
 * configuration space this module programs.
 *
 * Programming can only flip bits 0->1 ("data '1' cannot be
 * reprogrammed as data '0' without erasure" -- section 16.1); a page
 * must be erased before a fresh image is written to it, or bytes that
 * need to end up 0x00 won't actually clear (a page-write byte of 0x00
 * is a no-op, OR-in semantics). greenpak_nvm_program_image() always
 * erases before writing, and always verifies by reading the full image
 * back afterward -- I2C ACK success alone does not prove the written
 * data is correct.
 *
 * A freshly written NVM image does NOT take effect until the chip's
 * next POR (power-on-reset) sequence -- confirmed against datasheet
 * section 14, which describes NVM reload as strictly part of POR, not
 * triggered by an ordinary NVM write completing. Section 15.4.6's I2C
 * Serial Reset Command (register bit [1601]=1, self-clearing) forces
 * that same POR reload without a physical power-cycle -- see
 * greenpak_nvm_reset_and_reload().
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "greenpak_i2c.h"

#define GREENPAK_NVM_PAGE_SIZE  16
#define GREENPAK_NVM_PAGE_COUNT 16
#define GREENPAK_NVM_IMAGE_SIZE (GREENPAK_NVM_PAGE_SIZE * GREENPAK_NVM_PAGE_COUNT)

/* The last page (bytes 240-255) is never touched by program_image() --
 * confirmed against real hardware (2026-09-14) and the SLG46826
 * datasheet (Renesas Rev 3.18): erase/write commands for it ACK
 * normally but its actual stored content never changes, while every
 * other page erases/writes correctly. Table 54's Note 2 explains why --
 * part of the NVM config space is factory analog trim/calibration data,
 * gated behind a separate "Trim mode enable" bit this tool never sets
 * (and shouldn't: it has no real per-die trim values to write back, and
 * a .gp6 export's placeholder content for that region is not something
 * to program over real calibration data). Page-write's own "a 0x00 byte
 * is a no-op, not a clear-to-zero" rule (datasheet 16.1) plus this
 * chip's erase-to-0/program-only-sets-1s polarity is exactly why a
 * blocked erase there manifests as "writes ACK but nothing changes":
 * the near-all-zero expected content for that page was relying entirely
 * on the erase actually happening. */
#define GREENPAK_NVM_PROGRAMMABLE_PAGE_COUNT (GREENPAK_NVM_PAGE_COUNT - 1)

#define GREENPAK_DEFAULT_CONTROL_CODE 0x1 /* factory-blank chip, 0b0001 */

/* I2C block addresses (A10:A8, ignoring A10 which is unused on this
 * part -- see section 16.1's block-addressing figure). */
#define GREENPAK_BLOCK_REGISTERS 0x0 /* 0b000 */
#define GREENPAK_BLOCK_NVM       0x2 /* 0b010 */

/* Register-space (block 000) word addresses used by this module. */
#define GREENPAK_REG_ERASE       0xE3
#define GREENPAK_REG_I2C_RESET   0xC8 /* bit0 = register [1601], self-clearing */
#define GREENPAK_I2C_RESET_BIT   0x02 /* register [1601] is bit1 of byte 0xC8 (1601 = 8*200+1) */

static inline uint8_t greenpak_nvm_reg_addr(uint8_t control_code) {
    return (uint8_t)((control_code << 3) | GREENPAK_BLOCK_REGISTERS);
}
static inline uint8_t greenpak_nvm_nvm_addr(uint8_t control_code) {
    return (uint8_t)((control_code << 3) | GREENPAK_BLOCK_NVM);
}

/* Acknowledge-polls `addr7` (zero-length write, per datasheet 16.4)
 * until it ACKs or `timeout_ms` elapses. Returns false on timeout. */
bool greenpak_nvm_wait_ready(const greenpak_i2c_bus_t *bus, uint8_t addr7, uint32_t timeout_ms);

/* Probes whether any device ACKs at `addr7` right now (single
 * zero-length write, no retries/polling). Used for bus-scan/presence
 * checks, not completion waits. */
bool greenpak_nvm_probe(const greenpak_i2c_bus_t *bus, uint8_t addr7);

/* Erases NVM page `page` (0..15) on the chip at `control_code`. */
bool greenpak_nvm_erase_page(const greenpak_i2c_bus_t *bus, uint8_t control_code, uint8_t page);

/* Writes 16 bytes to NVM page `page` (0..15). Does NOT erase first --
 * call greenpak_nvm_erase_page() beforehand for correct results. */
bool greenpak_nvm_write_page(const greenpak_i2c_bus_t *bus, uint8_t control_code, uint8_t page,
                              const uint8_t data[GREENPAK_NVM_PAGE_SIZE]);

/* Reads 16 bytes back from NVM page `page` (0..15). */
bool greenpak_nvm_read_page(const greenpak_i2c_bus_t *bus, uint8_t control_code, uint8_t page,
                             uint8_t data[GREENPAK_NVM_PAGE_SIZE]);

/* Issues the I2C Serial Reset Command at the chip's current address
 * (`control_code`), forcing an immediate POR reload of NVM into live
 * registers -- including, for a page-0 write that changed the control
 * code, the chip's own I2C address. Self-clearing; no follow-up write
 * needed. Does not itself wait for the POR sequence to complete --
 * callers should allow real startup time (datasheet Table 3.4) before
 * probing the chip's new address. */
bool greenpak_nvm_reset_and_reload(const greenpak_i2c_bus_t *bus, uint8_t control_code);

/* Full-image program: erases and writes all 16 pages (in that order,
 * per page -- not "erase all 16 then write all 16"), then reads the
 * whole image back and compares byte-for-byte against `image` as the
 * real correctness proof, then issues greenpak_nvm_reset_and_reload().
 * `dry_run=true` skips erase/write/reset and only performs the
 * read-back-and-compare step, for verifying wiring/addressing against
 * whatever is already on the chip with zero write risk. Returns true
 * only if every step succeeded (and, for a real run, the read-back
 * matched exactly). */
bool greenpak_nvm_program_image(const greenpak_i2c_bus_t *bus, uint8_t control_code,
                                 const uint8_t image[GREENPAK_NVM_IMAGE_SIZE], bool dry_run);
