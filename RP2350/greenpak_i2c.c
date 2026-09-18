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
#include "pico/time.h"
#include "hardware/gpio.h"

/* ~10kHz -- deliberately slow, not the ~100kHz this was originally set to.
 * Real-hardware NVM programming test (2026-09-14, on the PC1500-Pico2W-
 * Dongle board, GreenPak_Provision copy of this file) showed every
 * I2C_READ (the only operation using a REPEATED START -- writes and the
 * erase command only ever do a single START/STOP) coming back with
 * stale data from the previous successful transaction, while writes/
 * erases ACKed normally. Root cause theory: this board's GreenPAK link
 * runs through U8, a TCA9406DC level shifter (Pico-side 3.3V to
 * GreenPAK-side VGG/5V -- confirmed 2026-09-16 against
 * PC1500-Pico2W-Dongle.kicad_sch, same schematic board_pins.h's own pin
 * table is read from -- this RP2350 firmware and "the Dongle board" are
 * the same board, not two separate designs), and scl_release()'s "wait
 * for SCL high" check samples the Pico's OWN pin, which can read high
 * before the level-shifted 5V side has actually settled -- margin a
 * plain START off an idle bus has plenty of, but a repeated start
 * (coming right after a byte transfer, far less slack) may not.
 * Widening this margin 10x is the cheap way to test that theory before
 * reaching for a scope; not yet confirmed with a scope. */
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
/* Clock-stretch wait bound -- generous relative to this bus's own 50us
 * bit period, but finite. Without this, a stuck SCL (electrical noise
 * coupling from the LH5801 bus lines now toggling right next to this I2C
 * pair under the dual-core architecture -- see monitor.c's own "WHY TWO
 * CORES" comment -- or a genuine SC18IS602B/GreenPAK fault) hung
 * DoCommand() forever on core1: confirmed live 2026-09-17, SDLS going
 * BUSY and never returning, freezing the LH5801's own SD_LIST_POLL
 * busy-wait (rom.asm) with no escape, since that loop has no timeout of
 * its own and no keyboard check -- the whole PC-1500 stopped responding
 * to any key, including OFF. This was the only genuinely unbounded loop
 * anywhere in the I2C/SD call chain (sc18is602b.c's retries and
 * diskio_sd_bridge.c's token/busy waits are all already bounded) -- a
 * timeout here converts a permanent hang into a clean, propagated I2C
 * failure instead. */
#define I2C_SCL_TIMEOUT_US 1000

static inline bool scl_release(const greenpak_i2c_bus_t *bus) {
    gpio_set_dir(bus->scl_gpio, GPIO_IN);
    /* Clock stretching: wait for the slave to release SCL high itself. */
    uint32_t start = time_us_32();
    while (!gpio_get(bus->scl_gpio)) {
        if (time_us_32() - start > I2C_SCL_TIMEOUT_US) return false;
        tight_loop_contents();
    }
    return true;
}
static inline bool sda_read(const greenpak_i2c_bus_t *bus) { return gpio_get(bus->sda_gpio); }

void greenpak_i2c_init(const greenpak_i2c_bus_t *bus) {
    gpio_init(bus->sda_gpio);
    gpio_init(bus->scl_gpio);
    gpio_pull_up(bus->sda_gpio);
    gpio_pull_up(bus->scl_gpio);
    sda_release(bus);
    (void)scl_release(bus);
}

static bool i2c_start(const greenpak_i2c_bus_t *bus) {
    sda_release(bus);
    if (!scl_release(bus)) return false;
    sleep_us(I2C_DELAY_US);
    sda_low(bus);
    sleep_us(I2C_DELAY_US);
    scl_low(bus);
    return true;
}

/* Always leaves SDA released regardless of the timeout outcome -- callers
 * that reach i2c_stop() after an earlier failure still want the bus left
 * in its normal idle (released) state, not stuck holding SDA low. */
static bool i2c_stop(const greenpak_i2c_bus_t *bus) {
    sda_low(bus);
    sleep_us(I2C_DELAY_US);
    bool ok = scl_release(bus);
    sleep_us(I2C_DELAY_US);
    sda_release(bus);
    sleep_us(I2C_DELAY_US);
    return ok;
}

/* Clocks out one bit (MSB-first convention handled by the caller) and
 * pulses SCL; must be called with SCL already low. */
static bool i2c_write_bit(const greenpak_i2c_bus_t *bus, bool bit) {
    if (bit) sda_release(bus); else sda_low(bus);
    sleep_us(I2C_DELAY_US);
    if (!scl_release(bus)) return false;
    sleep_us(I2C_DELAY_US);
    scl_low(bus);
    return true;
}

static bool i2c_read_bit(const greenpak_i2c_bus_t *bus, bool *bit_out) {
    sda_release(bus);
    sleep_us(I2C_DELAY_US);
    if (!scl_release(bus)) return false;
    sleep_us(I2C_DELAY_US);
    *bit_out = sda_read(bus);
    scl_low(bus);
    return true;
}

/* `*acked` is only meaningful when this returns true -- a false return
 * means a stuck bus, not a NAK. */
static bool i2c_write_byte(const greenpak_i2c_bus_t *bus, uint8_t byte, bool *acked) {
    for (int i = 7; i >= 0; i--) {
        if (!i2c_write_bit(bus, (byte >> i) & 1)) return false;
    }
    bool nak_bit;
    if (!i2c_read_bit(bus, &nak_bit)) return false;
    *acked = !nak_bit; /* ACK is SDA low */
    return true;
}

static bool i2c_read_byte(const greenpak_i2c_bus_t *bus, bool ack, uint8_t *byte_out) {
    uint8_t byte = 0;
    for (int i = 7; i >= 0; i--) {
        bool bit;
        if (!i2c_read_bit(bus, &bit)) return false;
        byte = (uint8_t)((byte << 1) | (bit ? 1 : 0));
    }
    if (!i2c_write_bit(bus, !ack)) return false; /* master drives ACK (0) or NAK (1) */
    *byte_out = byte;
    return true;
}

bool greenpak_i2c_write(const greenpak_i2c_bus_t *bus, uint8_t addr7, const uint8_t *buf, uint32_t len) {
    if (!i2c_start(bus)) return false;
    bool acked;
    if (!i2c_write_byte(bus, (uint8_t)(addr7 << 1), &acked) || !acked) {
        i2c_stop(bus);
        return false;
    }
    for (uint32_t i = 0; i < len; i++) {
        if (!i2c_write_byte(bus, buf[i], &acked) || !acked) {
            i2c_stop(bus);
            return false;
        }
    }
    return i2c_stop(bus);
}

bool greenpak_i2c_read(const greenpak_i2c_bus_t *bus, uint8_t addr7, uint8_t *buf, uint32_t len) {
    if (!i2c_start(bus)) return false;
    bool acked;
    if (!i2c_write_byte(bus, (uint8_t)((addr7 << 1) | 1), &acked) || !acked) {
        i2c_stop(bus);
        return false;
    }
    for (uint32_t i = 0; i < len; i++) {
        if (!i2c_read_byte(bus, i + 1 < len, &buf[i])) {
            i2c_stop(bus);
            return false;
        }
    }
    return i2c_stop(bus);
}

bool greenpak_i2c_write_reg(const greenpak_i2c_bus_t *bus, uint8_t addr7, uint8_t reg, uint8_t value) {
    uint8_t payload[2] = { reg, value };
    return greenpak_i2c_write(bus, addr7, payload, sizeof(payload));
}

bool greenpak_i2c_write_reg_tolerate_nak(const greenpak_i2c_bus_t *bus, uint8_t addr7, uint8_t reg, uint8_t value) {
    if (!i2c_start(bus)) return false;
    bool acked;
    if (!i2c_write_byte(bus, (uint8_t)(addr7 << 1), &acked) || !acked) {
        i2c_stop(bus);
        return false;
    }
    if (!i2c_write_byte(bus, reg, &acked) || !acked) {
        i2c_stop(bus);
        return false;
    }
    /* NAK expected/tolerated here -- see header comment -- but a bus
     * timeout (not just a NAK) is still a real failure. */
    if (!i2c_write_byte(bus, value, &acked)) {
        i2c_stop(bus);
        return false;
    }
    return i2c_stop(bus);
}

bool greenpak_i2c_read_reg(const greenpak_i2c_bus_t *bus, uint8_t addr7, uint8_t reg, uint8_t *buf, uint32_t len) {
    if (!i2c_start(bus)) return false;
    bool acked;
    if (!i2c_write_byte(bus, (uint8_t)(addr7 << 1), &acked) || !acked) {
        i2c_stop(bus);
        return false;
    }
    if (!i2c_write_byte(bus, reg, &acked) || !acked) {
        i2c_stop(bus);
        return false;
    }
    if (!i2c_start(bus)) return false; /* repeated start, no intervening stop */
    if (!i2c_write_byte(bus, (uint8_t)((addr7 << 1) | 1), &acked) || !acked) {
        i2c_stop(bus);
        return false;
    }
    for (uint32_t i = 0; i < len; i++) {
        if (!i2c_read_byte(bus, i + 1 < len, &buf[i])) {
            i2c_stop(bus);
            return false;
        }
    }
    return i2c_stop(bus);
}
