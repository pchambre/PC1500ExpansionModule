/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* greenpak_i2c.c -- see greenpak_i2c.h for the why. Standard open-drain
 * software I2C bit-banging: a pin drives low by switching to output-low,
 * and releases to high-Z (pulled up externally/internally) by switching
 * back to input -- never actively driven high, so two devices can never
 * fight over the bus. Ported from RP2350B/greenpak_i2c.c -- keep the two
 * in sync by hand.
 */
#include "greenpak_i2c.h"

#include "pico/stdlib.h"
#include "hardware/gpio.h"

/* ~10kHz -- deliberately slow, not the ~100kHz this was originally set to.
 * Real-hardware NVM programming test (2026-09-14) showed every I2C_READ
 * (the only operation using a REPEATED START -- writes and the erase
 * command only ever do a single START/STOP) coming back with stale data
 * from the previous successful transaction, while writes/erases ACKed
 * normally. Root cause theory: this board's GreenPAK link runs through
 * U8 (a TCA9406DC level shifter, Pico-side 3.3V to GreenPAK-side VGG/5V
 * -- see PC1500-Pico2W-Dongle schematic), and scl_release()'s "wait for
 * SCL high" check samples the Pico's OWN pin, which can read high before
 * the level-shifted 5V side has actually settled -- margin a plain START
 * off an idle bus has plenty of, but a repeated start (coming right
 * after a byte transfer, far less slack) may not. Widening this margin
 * 10x is the cheap way to test that theory before reaching for a scope.
 * Revisit/tighten once confirmed -- this is not a tuned final value. */
#define I2C_DELAY_US 50

static inline void sda_low(const greenpak_i2c_bus_t *bus) {
    gpio_set_dir(bus->sda_gpio, GPIO_OUT);
    gpio_put(bus->sda_gpio, 0);
}
static inline void sda_release(const greenpak_i2c_bus_t *bus) {
    gpio_set_dir(bus->sda_gpio, GPIO_IN);
}
static inline void scl_low(const greenpak_i2c_bus_t *bus) {
    gpio_set_dir(bus->scl_gpio, GPIO_OUT);
    gpio_put(bus->scl_gpio, 0);
}
static inline void scl_release(const greenpak_i2c_bus_t *bus) {
    gpio_set_dir(bus->scl_gpio, GPIO_IN);
    /* Clock stretching: wait for the slave to release SCL high itself. */
    while (!gpio_get(bus->scl_gpio)) tight_loop_contents();
}
static inline bool sda_read(const greenpak_i2c_bus_t *bus) { return gpio_get(bus->sda_gpio); }

void greenpak_i2c_init(const greenpak_i2c_bus_t *bus) {
    gpio_init(bus->sda_gpio);
    gpio_init(bus->scl_gpio);
    gpio_pull_up(bus->sda_gpio);
    gpio_pull_up(bus->scl_gpio);
    sda_release(bus);
    scl_release(bus);
}

static void i2c_start(const greenpak_i2c_bus_t *bus) {
    sda_release(bus);
    scl_release(bus);
    sleep_us(I2C_DELAY_US);
    sda_low(bus);
    sleep_us(I2C_DELAY_US);
    scl_low(bus);
}

static void i2c_stop(const greenpak_i2c_bus_t *bus) {
    sda_low(bus);
    sleep_us(I2C_DELAY_US);
    scl_release(bus);
    sleep_us(I2C_DELAY_US);
    sda_release(bus);
    sleep_us(I2C_DELAY_US);
}

/* Clocks out one bit (MSB-first convention handled by the caller) and
 * pulses SCL; must be called with SCL already low. */
static void i2c_write_bit(const greenpak_i2c_bus_t *bus, bool bit) {
    if (bit) sda_release(bus); else sda_low(bus);
    sleep_us(I2C_DELAY_US);
    scl_release(bus);
    sleep_us(I2C_DELAY_US);
    scl_low(bus);
}

static bool i2c_read_bit(const greenpak_i2c_bus_t *bus) {
    sda_release(bus);
    sleep_us(I2C_DELAY_US);
    scl_release(bus);
    sleep_us(I2C_DELAY_US);
    bool bit = sda_read(bus);
    scl_low(bus);
    return bit;
}

/* Returns true if the addressed/written byte was ACKed. */
static bool i2c_write_byte(const greenpak_i2c_bus_t *bus, uint8_t byte) {
    for (int i = 7; i >= 0; i--) i2c_write_bit(bus, (byte >> i) & 1);
    return !i2c_read_bit(bus); /* ACK is SDA low */
}

static uint8_t i2c_read_byte(const greenpak_i2c_bus_t *bus, bool ack) {
    uint8_t byte = 0;
    for (int i = 7; i >= 0; i--) byte = (uint8_t)((byte << 1) | (i2c_read_bit(bus) ? 1 : 0));
    i2c_write_bit(bus, !ack); /* master drives ACK (0) or NAK (1) */
    return byte;
}

bool greenpak_i2c_write(const greenpak_i2c_bus_t *bus, uint8_t addr7, const uint8_t *buf, uint32_t len) {
    i2c_start(bus);
    if (!i2c_write_byte(bus, (uint8_t)(addr7 << 1))) {
        i2c_stop(bus);
        return false;
    }
    for (uint32_t i = 0; i < len; i++) {
        if (!i2c_write_byte(bus, buf[i])) {
            i2c_stop(bus);
            return false;
        }
    }
    i2c_stop(bus);
    return true;
}

bool greenpak_i2c_read(const greenpak_i2c_bus_t *bus, uint8_t addr7, uint8_t *buf, uint32_t len) {
    i2c_start(bus);
    if (!i2c_write_byte(bus, (uint8_t)((addr7 << 1) | 1))) {
        i2c_stop(bus);
        return false;
    }
    for (uint32_t i = 0; i < len; i++) buf[i] = i2c_read_byte(bus, i + 1 < len);
    i2c_stop(bus);
    return true;
}

bool greenpak_i2c_write_reg(const greenpak_i2c_bus_t *bus, uint8_t addr7, uint8_t reg, uint8_t value) {
    uint8_t payload[2] = { reg, value };
    return greenpak_i2c_write(bus, addr7, payload, sizeof(payload));
}

bool greenpak_i2c_write_reg_tolerate_nak(const greenpak_i2c_bus_t *bus, uint8_t addr7, uint8_t reg, uint8_t value) {
    i2c_start(bus);
    if (!i2c_write_byte(bus, (uint8_t)(addr7 << 1))) {
        i2c_stop(bus);
        return false;
    }
    if (!i2c_write_byte(bus, reg)) {
        i2c_stop(bus);
        return false;
    }
    i2c_write_byte(bus, value); /* NAK expected/tolerated here -- see header comment */
    i2c_stop(bus);
    return true;
}

bool greenpak_i2c_read_reg(const greenpak_i2c_bus_t *bus, uint8_t addr7, uint8_t reg, uint8_t *buf, uint32_t len) {
    i2c_start(bus);
    if (!i2c_write_byte(bus, (uint8_t)(addr7 << 1))) {
        i2c_stop(bus);
        return false;
    }
    if (!i2c_write_byte(bus, reg)) {
        i2c_stop(bus);
        return false;
    }
    i2c_start(bus); /* repeated start, no intervening stop */
    if (!i2c_write_byte(bus, (uint8_t)((addr7 << 1) | 1))) {
        i2c_stop(bus);
        return false;
    }
    for (uint32_t i = 0; i < len; i++) buf[i] = i2c_read_byte(bus, i + 1 < len);
    i2c_stop(bus);
    return true;
}
