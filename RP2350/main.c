#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico/multicore.h"

#include "ff.h"

#include "board_pins.h"
#include "monitor.h"

/* Dual-core as of 2026-09-17 -- see monitor.c's own "WHY TWO CORES"
 * comment for the real root-caused reason this changed from the original
 * single-core design (a confirmed data-corruption bug, not just a
 * performance tweak). core0 runs monitor_run()'s bus loop, which never
 * blocks; core1 runs monitor_command_worker(), which blocks for as long
 * as each command (including real SD/I2C-bridge work) takes. Launching
 * core1 pulls in pico_multicore, which in turn makes cyw43_arch's
 * async_context enable its cross-core (semaphore/IRQ) support -- harmless
 * extra overhead here since cyw43_arch is still only ever called from
 * core0 (right below), never core1.
 *
 * SD card goes through U5 (an SC18IS602B I2C-to-SPI bridge chip,
 * sharing the GreenPAKs' own I2C bus) -- see board_pins.h's own SD
 * comment, sc18is602b.h, and diskio_sd_bridge.c. f_mount() below uses
 * opt=0 (register the work area, don't mount yet) rather than opt=1 --
 * mounting means disk_initialize()'s full SD command handshake, which
 * can legitimately take over a second (or the full timeout, if no card
 * is inserted); opt=0 defers that entirely to the first real file
 * operation DoCommand() actually performs (now on core1), instead of
 * blocking this board's own "do no harm" boot priority. A previous,
 * unrelated SD approach (QMI CS1, lib/qmi_cs1_sdspi/) hijacked GPIO19 --
 * which is really this board's D6 data-bus line -- on every boot; that's
 * gone now, but the same "don't put SD bring-up ahead of the bus loop"
 * principle still applies here even with a driver that touches the
 * right hardware. */

static FATFS g_fatfs;

int main(void) {
    stdio_init_all();

    monitor_init_buffer();
    f_mount(&g_fatfs, "", 0);

    /* cyw43_arch_init() before monitor_run(), on the same core (core0) --
     * it must run on whatever core owns the CYW43 async context, and
     * since everything now runs on core0 only, that's just "before the
     * loop that uses it starts." (Calling it from core1, briefly tried,
     * kept the LED dark entirely -- the CYW43 driver's background
     * PIO/DMA/IRQ setup expects the SDK's normal boot core.)
     *
     * REVERTED disabling this (2026-09-18) -- tried skipping
     * cyw43_arch_init()/LED/SMPS-PWM-forcing entirely as a power-draw/
     * noise-source experiment, and it broke something real: with this
     * block disabled, the dongle blocked the PC-1500 from powering on at
     * all. Whatever cyw43_arch_init() and/or forcing
     * CYW43_WL_GPIO_SMPS_PIN into PWM mode actually does here matters for
     * real, even though the original justifying comment for the PWM-mode
     * choice overstated an unconfirmed noise/corruption correlation (see
     * plan/git history) -- the two are separate questions: the *reason*
     * given for this code was shakier than claimed, but the code itself
     * is load-bearing. Do not disable this again without figuring out
     * why it's load-bearing first. */
    if (cyw43_arch_init() == 0) {
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
        cyw43_arch_gpio_put(CYW43_WL_GPIO_SMPS_PIN, 1);
    }

    multicore_launch_core1(monitor_command_worker);
    monitor_run(); /* never returns */
}
