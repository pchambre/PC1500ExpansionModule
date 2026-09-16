/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
#include "greenpak_nvm.h"

#include "pico/stdlib.h"

#include <stdio.h>
#include <string.h>

#define GREENPAK_REG_BYTE_WRITE_MASK 0xC9 /* same masking register greenpak_virtual_io.c uses */
#define ACK_POLL_RETRY_US 500u

bool greenpak_nvm_probe(const greenpak_i2c_bus_t *bus, uint8_t addr7) {
    return greenpak_i2c_write(bus, addr7, NULL, 0);
}

bool greenpak_nvm_wait_ready(const greenpak_i2c_bus_t *bus, uint8_t addr7, uint32_t timeout_ms) {
    uint64_t deadline = time_us_64() + (uint64_t)timeout_ms * 1000u;
    do {
        if (greenpak_nvm_probe(bus, addr7)) return true;
        sleep_us(ACK_POLL_RETRY_US);
    } while (time_us_64() < deadline);
    return false;
}

bool greenpak_nvm_erase_page(const greenpak_i2c_bus_t *bus, uint8_t control_code, uint8_t page) {
    uint8_t reg_addr = greenpak_nvm_reg_addr(control_code);
    uint8_t value = (uint8_t)(0x80 | (page & 0x0F)); /* b7=ERSE, b3:0=ERSEB3:0 (page); ERSEB4=0 selects the NVM-config region */
    /* greenpak_i2c_write_reg_tolerate_nak(), not the strict
     * greenpak_i2c_write_reg() -- see that function's header comment:
     * the erase register's data byte is a documented, deliberate NAK on
     * real hardware (Renesas SLG46824/6/7-A errata), not a failure. */
    if (!greenpak_i2c_write_reg_tolerate_nak(bus, reg_addr, GREENPAK_REG_ERASE, value)) return false;
    return greenpak_nvm_wait_ready(bus, reg_addr, 50);
}

bool greenpak_nvm_write_page(const greenpak_i2c_bus_t *bus, uint8_t control_code, uint8_t page,
                              const uint8_t data[GREENPAK_NVM_PAGE_SIZE]) {
    uint8_t nvm_addr = greenpak_nvm_nvm_addr(control_code);
    uint8_t payload[1 + GREENPAK_NVM_PAGE_SIZE];
    payload[0] = (uint8_t)(page * GREENPAK_NVM_PAGE_SIZE); /* page-aligned word address */
    memcpy(&payload[1], data, GREENPAK_NVM_PAGE_SIZE);

    if (!greenpak_i2c_write(bus, nvm_addr, payload, sizeof(payload))) return false;
    return greenpak_nvm_wait_ready(bus, nvm_addr, 50);
}

bool greenpak_nvm_read_page(const greenpak_i2c_bus_t *bus, uint8_t control_code, uint8_t page,
                             uint8_t data[GREENPAK_NVM_PAGE_SIZE]) {
    uint8_t nvm_addr = greenpak_nvm_nvm_addr(control_code);
    uint8_t word_addr = (uint8_t)(page * GREENPAK_NVM_PAGE_SIZE);
    /* Deliberately NOT greenpak_i2c_read_reg() (repeated START, no
     * intervening STOP) -- real hardware testing (2026-09-14) showed
     * every page but the first reading back identical, stale data
     * regardless of I2C clock speed (ruled out a repeated-start timing
     * margin theory: 10x slower made no difference), strongly
     * suggesting this chip's NVM read doesn't treat a repeated start as
     * a real address-pointer update and just keeps serving whatever its
     * internal read pointer already had. Two fully separate
     * transactions (address write + real STOP, then a fresh START to
     * read) is the next-cheapest thing to try before reaching for a
     * datasheet/logic analyzer. */
    if (!greenpak_i2c_write(bus, nvm_addr, &word_addr, 1)) return false;
    return greenpak_i2c_read(bus, nvm_addr, data, GREENPAK_NVM_PAGE_SIZE);
}

bool greenpak_nvm_reset_and_reload(const greenpak_i2c_bus_t *bus, uint8_t control_code) {
    uint8_t reg_addr = greenpak_nvm_reg_addr(control_code);

    /* Register 0xC8 packs multiple unrelated control bits (e.g. bit2 =
     * Outputs Latching During I2C Write, per the datasheet's I2C
     * Serial Command Register Map) -- byte-write-bit-masking so only
     * bit1 (the reset bit) is touched, matching greenpak_virtual_io.c's
     * pattern for the same reason. */
    uint8_t mask = (uint8_t)~GREENPAK_I2C_RESET_BIT;
    if (!greenpak_i2c_write_reg(bus, reg_addr, GREENPAK_REG_BYTE_WRITE_MASK, mask)) return false;
    return greenpak_i2c_write_reg(bus, reg_addr, GREENPAK_REG_I2C_RESET, GREENPAK_I2C_RESET_BIT);
}

/* Diagnostic printf instrumentation below -- this is a provisioning/debug
 * tool, not size/time-constrained production firmware, and the previous
 * plain true/false return gave zero insight into WHICH step (erase vs
 * write vs read vs verify) or WHICH page/byte actually failed on real
 * hardware. Left in permanently rather than behind a flag: a failed
 * program attempt should always be diagnosable from the serial log. */
bool greenpak_nvm_program_image(const greenpak_i2c_bus_t *bus, uint8_t control_code,
                                 const uint8_t image[GREENPAK_NVM_IMAGE_SIZE], bool dry_run) {
    if (!dry_run) {
        /* GREENPAK_NVM_PROGRAMMABLE_PAGE_COUNT, not PAGE_COUNT -- the
         * last page is deliberately never erased/written, see that
         * constant's own comment. */
        for (uint8_t page = 0; page < GREENPAK_NVM_PROGRAMMABLE_PAGE_COUNT; page++) {
            if (!greenpak_nvm_erase_page(bus, control_code, page)) {
                printf("    greenpak_nvm_program_image: erase FAILED on page %u (cc=0x%X)\n", page, control_code);
                return false;
            }
            if (!greenpak_nvm_write_page(bus, control_code, page, &image[page * GREENPAK_NVM_PAGE_SIZE])) {
                printf("    greenpak_nvm_program_image: write FAILED on page %u (cc=0x%X)\n", page, control_code);
                return false;
            }

            /* Immediate per-page read-back, BEFORE touching the next
             * page -- narrows a mismatch down to "wrong at write time"
             * vs. "corrupted later by a subsequent page's erase/write",
             * which the old end-of-run-only bulk verify couldn't tell
             * apart. Diagnostic only: doesn't return false by itself, so
             * a real hardware quirk here doesn't block the rest of the
             * run -- the final bulk verify below is still the real
             * pass/fail gate. */
            uint8_t immediate[GREENPAK_NVM_PAGE_SIZE];
            if (!greenpak_nvm_read_page(bus, control_code, page, immediate)) {
                printf("    greenpak_nvm_program_image: immediate read-back FAILED right after writing page %u (cc=0x%X)\n",
                       page, control_code);
            } else if (memcmp(immediate, &image[page * GREENPAK_NVM_PAGE_SIZE], GREENPAK_NVM_PAGE_SIZE) != 0) {
                printf("    greenpak_nvm_program_image: page %u WRONG immediately after its own write (cc=0x%X):\n",
                       page, control_code);
                printf("      expected:");
                for (int j = 0; j < GREENPAK_NVM_PAGE_SIZE; j++) printf(" %02X", image[page * GREENPAK_NVM_PAGE_SIZE + j]);
                printf("\n      read:    ");
                for (int j = 0; j < GREENPAK_NVM_PAGE_SIZE; j++) printf(" %02X", immediate[j]);
                printf("\n");
            } else {
                printf("    greenpak_nvm_program_image: page %u OK immediately after its own write (cc=0x%X)\n", page, control_code);
            }
        }
    }

    uint8_t readback[GREENPAK_NVM_IMAGE_SIZE];
    /* Same GREENPAK_NVM_PROGRAMMABLE_PAGE_COUNT bound as above -- the
     * last page's real content is never read back or verified, since
     * this tool never claims to control it. */
    for (uint8_t page = 0; page < GREENPAK_NVM_PROGRAMMABLE_PAGE_COUNT; page++) {
        if (!greenpak_nvm_read_page(bus, control_code, page, &readback[page * GREENPAK_NVM_PAGE_SIZE])) {
            printf("    greenpak_nvm_program_image: read-back FAILED on page %u (cc=0x%X)\n", page, control_code);
            return false;
        }
    }
    const uint32_t programmable_size = GREENPAK_NVM_PROGRAMMABLE_PAGE_COUNT * GREENPAK_NVM_PAGE_SIZE;
    if (memcmp(readback, image, programmable_size) != 0) {
        printf("    greenpak_nvm_program_image: verify MISMATCH (cc=0x%X) -- first differing bytes:\n", control_code);
        int shown = 0;
        for (uint32_t i = 0; i < programmable_size && shown < 8; i++) {
            if (readback[i] != image[i]) {
                printf("      byte %3d (page %2d, offset %2d): expected 0x%02X, read 0x%02X\n",
                       (int)i, (int)(i / GREENPAK_NVM_PAGE_SIZE), (int)(i % GREENPAK_NVM_PAGE_SIZE), image[i], readback[i]);
                shown++;
            }
        }
        return false;
    }

    if (dry_run) return true;

    if (!greenpak_nvm_reset_and_reload(bus, control_code)) {
        printf("    greenpak_nvm_program_image: reset_and_reload FAILED (cc=0x%X)\n", control_code);
        return false;
    }
    return true;
}
