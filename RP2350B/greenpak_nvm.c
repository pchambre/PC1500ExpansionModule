/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
#include "greenpak_nvm.h"

#include "pico/stdlib.h"

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
    if (!greenpak_i2c_write_reg(bus, reg_addr, GREENPAK_REG_ERASE, value)) return false;
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
    return greenpak_i2c_read_reg(bus, nvm_addr, word_addr, data, GREENPAK_NVM_PAGE_SIZE);
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

bool greenpak_nvm_program_image(const greenpak_i2c_bus_t *bus, uint8_t control_code,
                                 const uint8_t image[GREENPAK_NVM_IMAGE_SIZE], bool dry_run) {
    if (!dry_run) {
        for (uint8_t page = 0; page < GREENPAK_NVM_PAGE_COUNT; page++) {
            if (!greenpak_nvm_erase_page(bus, control_code, page)) return false;
            if (!greenpak_nvm_write_page(bus, control_code, page, &image[page * GREENPAK_NVM_PAGE_SIZE])) return false;
        }
    }

    uint8_t readback[GREENPAK_NVM_IMAGE_SIZE];
    for (uint8_t page = 0; page < GREENPAK_NVM_PAGE_COUNT; page++) {
        if (!greenpak_nvm_read_page(bus, control_code, page, &readback[page * GREENPAK_NVM_PAGE_SIZE])) return false;
    }
    if (memcmp(readback, image, GREENPAK_NVM_IMAGE_SIZE) != 0) return false;

    if (dry_run) return true;
    return greenpak_nvm_reset_and_reload(bus, control_code);
}
