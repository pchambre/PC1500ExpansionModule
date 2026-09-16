/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* main.c -- GreenPakTester: Pico 2 W bench-test firmware for the
 * GreenPAK SLG46826V. Waits for a USB CDC terminal to actually connect
 * (rather than guessing a fixed delay -- a one-shot printf right after
 * boot is easy to miss, since flashing reboots the device and you're
 * still switching over to open a serial monitor), then re-runs the
 * self-test (currently placeholder vectors -- see selftest.c/README.md)
 * on a loop, so output is visible whenever you attach, not just in a
 * narrow window right after boot.
 */
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/stdio_usb.h"

#include "greenpak_gpio.h"
#include "greenpak_i2c.h"
#include "selftest.h"

int main(void) {
    stdio_init_all();
    setvbuf(stdout, NULL, _IONBF, 0); /* flush printf output immediately over USB CDC,
                                        * instead of only once the libc buffer fills */

    /* Wait for a terminal to actually open the port (DTR asserted) so
     * the first printf isn't lost -- bounded so the firmware still
     * proceeds (self-test still runs, just with no one watching) if no
     * terminal ever connects. */
    for (int waited_ms = 0; !stdio_usb_connected() && waited_ms < 10000; waited_ms += 100) {
        sleep_ms(100);
    }
    sleep_ms(200); /* let the host-side terminal finish attaching before the first byte */

    greenpak_gpio_init_all();
    greenpak_i2c_init(100 * 1000); /* 100kHz standard mode; bump later if margins allow */

    while (true) {
        printf("GreenPakTester -- SLG46826V bench-test firmware\n");

        greenpak_dut_t dut = greenpak_i2c_detect_dut();
        switch (dut) {
            case GREENPAK_DUT_GP1:
                printf("DUT detected: GP1 (I2C addr 0x%02X) -- running GP1 self-test\n", greenpak_dut_addr);
                selftest_run(test_vectors, test_vectors_count);
                break;
            case GREENPAK_DUT_GP2:
                printf("DUT detected: GP2 (I2C addr 0x%02X) -- running GP2 self-test\n", greenpak_dut_addr);
                selftest_run(test_vectors_gp2, test_vectors_gp2_count);
                break;
            case GREENPAK_DUT_NONE:
            default:
                printf("No DUT found at GP1 (0x%02X) or GP2 (0x%02X) -- check wiring/power, or the "
                       "chip's control code doesn't match either expected value.\n",
                       GREENPAK_GP1_I2C_ADDR, GREENPAK_GP2_I2C_ADDR);
                break;
        }

        sleep_ms(5000);
    }
}
