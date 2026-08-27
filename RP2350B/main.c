/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
#include "pico/stdlib.h"
#include "pico/multicore.h"

#include "ff.h"

#include "board_pins.h"
#include "monitor.h"
#include "ble_rn4871.h"
#include "greenpak_i2c.h"

/* Core0: one-time init (stdio-over-USB, SPI SD transport, FatFs mount,
 * BLE UART, GreenPAK I2C buses), then launches the bus-servicing/
 * DoCommand loop on core1 and sits idle -- reserved for future BLE
 * traffic servicing, not part of this port yet. See RP2350B/README.md
 * and ../RP2350/README.md (the earlier Pico 2 W port this was derived
 * from) for the full writeup. */

static FATFS g_fatfs;

static const greenpak_i2c_bus_t g_greenpak1_bus = {
    .sda_gpio = PIN_GREENPAK1_SDA,
    .scl_gpio = PIN_GREENPAK1_SCL,
};
static const greenpak_i2c_bus_t g_greenpak2_bus = {
    .sda_gpio = PIN_GREENPAK2_SDA,
    .scl_gpio = PIN_GREENPAK2_SCL,
};

int main(void) {
    /* USB stdio, not UART stdio: every GPIO on this board (including
     * the Pico SDK's usual UART0 default pins, GPIO12/13) is already
     * committed to the PC-1500 bus -- see board_pins.h. USB uses the
     * chip's dedicated D+/D- pins, no GPIO budget cost. Enabled via
     * CMakeLists.txt's pico_enable_stdio_usb/pico_enable_stdio_uart,
     * not here. */
    stdio_init_all();

    /* Hardware SPI1 SD transport (see hw_config.c) -- opt=1: mount
     * immediately (not lazily on first file access) so a missing/dead
     * SD card is discovered at boot rather than silently on the first
     * SDLS. The return value isn't checked here beyond that:
     * DoCommand()'s own SD cases already handle and report a not-ready
     * card via ordinary f_* failures. */
    f_mount(&g_fatfs, "", 1);

    ble_rn4871_init();
    greenpak_i2c_init(&g_greenpak1_bus);
    greenpak_i2c_init(&g_greenpak2_bus);

    multicore_launch_core1(monitor_run);

    while (true) {
        tight_loop_contents();
    }
}
