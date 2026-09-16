/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
#include "greenpak_i2c.h"

#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "pico/error.h"

#define GREENPAK_I2C_INSTANCE i2c0

uint8_t greenpak_dut_addr = GREENPAK_GP1_I2C_ADDR;

void greenpak_i2c_init(uint32_t baudrate_hz) {
    i2c_init(GREENPAK_I2C_INSTANCE, baudrate_hz);
    gpio_set_function(GREENPAK_I2C_SDA_GPIO, GPIO_FUNC_I2C);
    gpio_set_function(GREENPAK_I2C_SCL_GPIO, GPIO_FUNC_I2C);
    gpio_pull_up(GREENPAK_I2C_SDA_GPIO);
    gpio_pull_up(GREENPAK_I2C_SCL_GPIO);
}

bool greenpak_i2c_write(uint8_t addr7, const uint8_t *buf, size_t len) {
    int ret = i2c_write_blocking(GREENPAK_I2C_INSTANCE, addr7, buf, len, false);
    return ret == (int)len;
}

bool greenpak_i2c_read(uint8_t addr7, uint8_t *buf, size_t len) {
    int ret = i2c_read_blocking(GREENPAK_I2C_INSTANCE, addr7, buf, len, false);
    return ret == (int)len;
}

bool greenpak_i2c_write_reg(uint8_t addr7, uint8_t reg, uint8_t value) {
    uint8_t payload[2] = { reg, value };
    return greenpak_i2c_write(addr7, payload, sizeof(payload));
}

bool greenpak_i2c_read_reg(uint8_t addr7, uint8_t reg, uint8_t *buf, size_t len) {
    int wret = i2c_write_blocking(GREENPAK_I2C_INSTANCE, addr7, &reg, 1, true /* nostop, repeated start follows */);
    if (wret != 1) return false;
    int rret = i2c_read_blocking(GREENPAK_I2C_INSTANCE, addr7, buf, len, false);
    return rret == (int)len;
}

bool greenpak_i2c_probe(uint8_t addr7) {
    /* A zero-length i2c_write_blocking is NOT a valid presence probe on
     * this SDK: i2c_write_blocking_internal() has
     * invalid_params_if(HARDWARE_I2C, len == 0), which only fires in
     * debug builds (PARAM_ASSERTIONS_ENABLED) -- in this project's
     * Release build it's compiled to nothing, so a len=0 write proceeds
     * without ever clocking out the address byte and just reports
     * success unconditionally. Confirmed against
     * hardware_i2c/i2c.c:139 in the installed Pico SDK. A real 1-byte
     * read (matching pico-examples/i2c/bus_scan's own approach) is a
     * genuine Start+Address+ACK/NAK transaction, and is read-only/
     * side-effect-free. */
    uint8_t dummy;
    int ret = i2c_read_blocking(GREENPAK_I2C_INSTANCE, addr7, &dummy, 1, false);
    return ret == 1;
}

greenpak_dut_t greenpak_i2c_detect_dut(void) {
    if (greenpak_i2c_probe(GREENPAK_GP1_I2C_ADDR)) {
        greenpak_dut_addr = GREENPAK_GP1_I2C_ADDR;
        return GREENPAK_DUT_GP1;
    }
    if (greenpak_i2c_probe(GREENPAK_GP2_I2C_ADDR)) {
        greenpak_dut_addr = GREENPAK_GP2_I2C_ADDR;
        return GREENPAK_DUT_GP2;
    }
    return GREENPAK_DUT_NONE;
}
