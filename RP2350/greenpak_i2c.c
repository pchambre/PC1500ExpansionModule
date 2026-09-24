/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* greenpak_i2c.c -- see greenpak_i2c.h for the why.
 *
 * Real RP2350 I2C1 hardware peripheral (2026-09-20) -- GP26/GP27 land on
 * a genuine I2C1 SDA/SCL pair (confirmed against the PC1500-Pico2W-Dongle
 * schematic, see board_pins.h), and this board's own bit-banged history
 * turned out to be the real ceiling on this bus's throughput: with real
 * external pull-ups added and the bit-bang delay tuned as aggressively
 * as sleep_us() would allow, measured throughput barely moved beyond
 * ~140kHz-equivalent, nowhere near the 400kHz (Fast Mode) the SC18IS602B
 * SD bridge chip itself supports (its own hard ceiling -- confirmed with
 * the board owner, don't try to push this peripheral faster than
 * 400000). Software bit-banging's own per-bit call/timer overhead was
 * the actual bottleneck, not the deliberate delay value -- a real
 * hardware peripheral clocks bits in dedicated silicon with none of
 * that overhead. RP2350B's own copy of this file is intentionally NOT
 * updated to match (different board, different pins, not verified to
 * land on a hardware I2C peripheral the same way) -- see that file's
 * own comment.
 */
#include "greenpak_i2c.h"

#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "pico/time.h"
#include "pico/timeout_helper.h"

#include "monitor.h" /* g_i2c_activity_pending -- see its own comment */

/* GP26/GP27 -- confirmed I2C1, not I2C0 (see this file's own top
 * comment). Hardcoded rather than derived from bus->sda_gpio/scl_gpio
 * at runtime, since this driver is already specific to this board's
 * fixed pin assignment (bus->sda_gpio/scl_gpio still exist and are used
 * to configure the GPIO function/pull-ups in greenpak_i2c_init(), just
 * not to pick the peripheral instance). */
#define GREENPAK_I2C_INSTANCE i2c1

/* The SC18IS602B's own real ceiling (see this file's own top comment) --
 * do not raise this. */
#define GREENPAK_I2C_BAUDRATE 400000u

/* Per-BYTE timeout (not a fixed total-transaction timeout), since this
 * same driver serves both tiny 1-2 byte GreenPAK register pokes and much
 * larger (~200 byte) SD-bridge chunk transfers (via sc18is602b.c's own
 * greenpak_i2c_write()/read() calls) -- a fixed total timeout sized for
 * the small case would be far too tight for the large one. 1000us/byte
 * is a generous ~40x margin over the ~22.5us/byte a 400kHz transfer
 * actually needs, bounding a genuinely stuck/wedged bus (the same
 * concern the old bit-banged version's I2C_SCL_TIMEOUT_US existed for --
 * see this project's own history, 2026-09-17: a real hang here once
 * froze the whole PC-1500, no keys responding, including OFF) without
 * being so tight it spuriously fails on a real, correctly-functioning
 * transfer. */
#define GREENPAK_I2C_TIMEOUT_PER_BYTE_US 1000u

void greenpak_i2c_init(const greenpak_i2c_bus_t *bus) {
    i2c_init(GREENPAK_I2C_INSTANCE, GREENPAK_I2C_BAUDRATE);
    gpio_set_function(bus->sda_gpio, GPIO_FUNC_I2C);
    gpio_set_function(bus->scl_gpio, GPIO_FUNC_I2C);
    /* No gpio_pull_up() here -- real 1.5K external pull-ups already sit on
     * this bus (added 2026-09-20 specifically for Fast Mode), and the
     * RP2350's own weak internal pulls would only be redundant in
     * parallel with them, not needed (confirmed with the board owner). */
}

void greenpak_i2c_bus_recover(const greenpak_i2c_bus_t *bus) {
    uint32_t scl = bus->scl_gpio;
    uint32_t sda = bus->sda_gpio;

    /* Take the pins back from the I2C peripheral -- plain SIO GPIO, driven
     * open-drain style (direction toggling between IN/Hi-Z and OUT/driven-
     * LOW, never actively driven HIGH -- the real external 1.5K pull-ups
     * do that, exactly like a real I2C bus). Pre-arm the output latch to 0
     * BEFORE ever switching direction to OUT, same principle already used
     * elsewhere in this project (read_serve.pio's own header, DriveData())
     * for avoiding a glitch on the very first direction change. */
    gpio_put(scl, 0);
    gpio_put(sda, 0);
    gpio_set_function(scl, GPIO_FUNC_SIO);
    gpio_set_function(sda, GPIO_FUNC_SIO);
    gpio_set_dir(scl, GPIO_IN);
    gpio_set_dir(sda, GPIO_IN);
    sleep_us(5);

    /* Up to 9 clock pulses (NXP UM10204's own "bus clear" procedure) --
     * enough to walk a slave that's stuck mid-byte all the way through
     * releasing SDA (worst case: 8 data bits plus the ACK bit it's
     * waiting to clock out). Stops early the moment SDA is seen released,
     * rather than always doing the full 9. */
    for (int i = 0; i < 9; i++) {
        if (gpio_get(sda)) break;
        gpio_set_dir(scl, GPIO_OUT); /* drives LOW -- latch already 0 */
        sleep_us(5);
        gpio_set_dir(scl, GPIO_IN);  /* release -- pull-up brings it HIGH */
        sleep_us(5);
    }

    /* Manufacture a STOP condition (SDA low-to-high while SCL is high) so
     * any slave watching the bus sees a clean, unambiguous end of
     * transaction, not just an abandoned clock train. */
    gpio_set_dir(sda, GPIO_OUT); /* SDA low */
    sleep_us(5);
    gpio_set_dir(scl, GPIO_IN);  /* SCL high */
    sleep_us(5);
    gpio_set_dir(sda, GPIO_IN);  /* SDA high -- STOP */
    sleep_us(5);

    /* Hand the pins back to the real I2C peripheral, freshly reconfigured. */
    greenpak_i2c_init(bus);
}

bool greenpak_i2c_write(const greenpak_i2c_bus_t *bus, uint8_t addr7, const uint8_t *buf, uint32_t len) {
    (void)bus;
    g_i2c_activity_pending = true; /* real drive-activity LED -- see monitor.h */
    int rc = i2c_write_timeout_per_char_us(GREENPAK_I2C_INSTANCE, addr7, buf, len, false,
                                            GREENPAK_I2C_TIMEOUT_PER_BYTE_US);
    return rc == (int)len;
}

/* Local, safe replacement for the pico-sdk's own i2c_read_timeout_per_char_us()
 * (2026-09-22) -- a real, confirmed gap found by reading hardware_i2c/i2c.c
 * (SDK 2.3.1) directly: i2c_read_blocking_internal()'s wait loop for TX
 * FIFO room, before pushing the next read-command byte --
 *
 *     while (!i2c_get_write_available(i2c))
 *         tight_loop_contents();
 *
 * -- never calls its own timeout_check callback, unlike every other wait
 * loop in that same function (the FIFO-drain wait right after it does).
 * If the TX FIFO is ever stuck full, that call hangs forever, regardless
 * of the timeout_per_char_us argument passed to it. Root-caused live:
 * RAMTST2/STAGE calls into greenpak_i2c_read_reg() hung indefinitely with
 * the drive-activity LED solid on (one I2C transaction started, silence
 * after -- consistent with being stuck inside this exact loop, not the
 * bounded 5x retry loop in greenpak_virtual_io.c above it, which would
 * show up as repeated blinking as each attempt fails and retries).
 *
 * This is a close copy of the SDK's own i2c_read_blocking_internal(),
 * built only from its public API (i2c_get_hw()/i2c_get_write_available()/
 * i2c_get_read_available() in hardware/i2c.h; timeout_state_t/
 * check_timeout_fn/init_per_iteration_timeout_us() in
 * pico/timeout_helper.h) -- not a vendored-source edit, which would be
 * silently lost on any SDK upgrade -- with exactly one change: a
 * timeout_check() call added to the previously-unprotected loop, matching
 * the pattern every other wait loop in the original already uses.
 * Simplified from the general original for this project's own actual
 * usage: nostop is always false here (every greenpak_i2c_read_reg()/
 * greenpak_i2c_read() call sends a real STOP), so the STOP bit is just
 * `last`, and restart_on_next is always cleared afterward rather than
 * threaded through. */
static int greenpak_i2c_read_safe(i2c_inst_t *i2c, uint8_t addr7, uint8_t *dst, size_t len,
                                   uint timeout_per_char_us) {
    timeout_state_t ts;
    check_timeout_fn timeout_check = init_per_iteration_timeout_us(&ts, timeout_per_char_us);

    i2c_hw_t *hw = i2c_get_hw(i2c);
    hw->enable = 0;
    hw->tar = addr7;
    hw->enable = 1;

    bool abort = false;
    bool timeout = false;
    uint32_t abort_reason = 0;
    int byte_ctr;
    int ilen = (int)len;

    for (byte_ctr = 0; byte_ctr < ilen; ++byte_ctr) {
        bool first = byte_ctr == 0;
        bool last = byte_ctr == ilen - 1;
        timeout_check(&ts, true); /* per-iteration reset, matches the SDK's own convention */

        /* THE FIX -- see this function's own header comment: the SDK's
         * own equivalent loop here never checks timeout_check at all. */
        while (!i2c_get_write_available(i2c)) {
            timeout = timeout_check(&ts, false);
            abort |= timeout;
            if (abort) break;
            tight_loop_contents();
        }
        if (abort) break;

        hw->data_cmd =
                (uint32_t)((first && i2c->restart_on_next) ? 1u : 0u) << I2C_IC_DATA_CMD_RESTART_LSB |
                (uint32_t)(last ? 1u : 0u) << I2C_IC_DATA_CMD_STOP_LSB |
                I2C_IC_DATA_CMD_CMD_BITS; /* 1 -> read */

        do {
            abort_reason = hw->tx_abrt_source;
            if (hw->raw_intr_stat & I2C_IC_RAW_INTR_STAT_TX_ABRT_BITS) {
                abort = true;
                (void)hw->clr_tx_abrt;
            }
            timeout = timeout_check(&ts, false);
            abort |= timeout;
        } while (!abort && !i2c_get_read_available(i2c));

        if (abort) break;

        *dst++ = (uint8_t)hw->data_cmd;
    }

    int rval;
    if (abort) {
        rval = timeout ? PICO_ERROR_TIMEOUT : PICO_ERROR_GENERIC;
    } else {
        rval = byte_ctr;
    }

    i2c->restart_on_next = false;
    return rval;
}

bool greenpak_i2c_read(const greenpak_i2c_bus_t *bus, uint8_t addr7, uint8_t *buf, uint32_t len) {
    (void)bus;
    g_i2c_activity_pending = true; /* real drive-activity LED -- see monitor.h */
    int rc = greenpak_i2c_read_safe(GREENPAK_I2C_INSTANCE, addr7, buf, len,
                                     GREENPAK_I2C_TIMEOUT_PER_BYTE_US);
    return rc == (int)len;
}

bool greenpak_i2c_write_reg(const greenpak_i2c_bus_t *bus, uint8_t addr7, uint8_t reg, uint8_t value) {
    uint8_t payload[2] = { reg, value };
    return greenpak_i2c_write(bus, addr7, payload, sizeof(payload));
}

bool greenpak_i2c_write_reg_tolerate_nak(const greenpak_i2c_bus_t *bus, uint8_t addr7, uint8_t reg, uint8_t value) {
    (void)bus;
    uint8_t payload[2] = { reg, value };
    /* Per hardware_i2c's own internal abort handling: PICO_ERROR_GENERIC
     * means the address itself was NAKed (a real failure); a return
     * equal to the byte index where a NAK occurred means the address and
     * every byte before that index were ACKed. rc==2 is a full, ordinary
     * success. rc==1 means the register-address byte (index 0) was
     * ACKed but the value byte (index 1) was NAKed -- exactly the
     * tolerated erase-register quirk this function exists for (see this
     * function's own header comment). rc==0 (register byte itself
     * NAKed) or negative (address NAKed/timeout) are still real
     * failures. */
    g_i2c_activity_pending = true; /* real drive-activity LED -- see monitor.h */
    int rc = i2c_write_timeout_per_char_us(GREENPAK_I2C_INSTANCE, addr7, payload, sizeof(payload), false,
                                            GREENPAK_I2C_TIMEOUT_PER_BYTE_US);
    return rc == 2 || rc == 1;
}

bool greenpak_i2c_read_reg(const greenpak_i2c_bus_t *bus, uint8_t addr7, uint8_t reg, uint8_t *buf, uint32_t len) {
    (void)bus;
    g_i2c_activity_pending = true; /* real drive-activity LED -- see monitor.h */
    int wrc = i2c_write_timeout_per_char_us(GREENPAK_I2C_INSTANCE, addr7, &reg, 1, true,
                                             GREENPAK_I2C_TIMEOUT_PER_BYTE_US);
    if (wrc != 1) return false;
    int rrc = greenpak_i2c_read_safe(GREENPAK_I2C_INSTANCE, addr7, buf, len,
                                      GREENPAK_I2C_TIMEOUT_PER_BYTE_US);
    return rrc == (int)len;
}
