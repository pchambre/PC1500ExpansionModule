#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico/multicore.h"
#include "pico/flash.h"

#include "ff.h"

#include "board_pins.h"
#include "monitor.h"
#include "mcu_log.h"
#include "mcu_config.h"

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
    /* TEMPORARY boot-timing instrumentation (2026-09-28) -- remove once
     * the user has captured real hardware numbers. GP28 (PIN_SD_BRIDGE_INT)
     * is provably idle for this whole window: sc18is602b_init_int_pin()
     * only gpio_init()s it lazily on the first real SPI transaction, which
     * per f_mount()'s own comment below never happens before deferred
     * disk_initialize() on core1, long after boot. It's fully reconfigured
     * as an input there regardless of this temporary output use. Scope
     * channel 1 on the 3.3V rail/VSYS, channel 2 here: rising edge marks
     * main() entry, falling edge (right after monitor_setup_pio(), below)
     * marks the read_serve PIO actually being live. */
    gpio_init(PIN_SD_BRIDGE_INT);
    gpio_set_dir(PIN_SD_BRIDGE_INT, GPIO_OUT);
    gpio_put(PIN_SD_BRIDGE_INT, 1);

    /* Registers core0 (this core) as a flash_safe_execute() lockout
     * victim -- must happen before core1 (launched below) ever calls
     * mcu_log_*() and triggers a flash write there, or flash_safe_execute()
     * has no way to safely pause this core during it (see mcu_log.c's own
     * top comment). Cheap, one-time, no reason not to do it unconditionally
     * even on boots that never end up logging anything. */
    flash_safe_execute_core_init();

    /* Bus-serving PIO path comes up FIRST (2026-09-28), ahead of
     * everything below -- moved out of monitor_run() (see
     * monitor_setup_pio()'s own comment in monitor.c) because real
     * hardware showed the LH5801's own one-time expansion-ROM boot scan,
     * on a genuine PC-1500 cold power-on, doesn't leave enough margin for
     * read_serve to still be gated behind stdio/GreenPAK-I2C/config/log/
     * SD-mount/cyw43 bring-up first. monitor_init_buffer() must run
     * before monitor_setup_pio() -- read_serve's DMA chain reads directly
     * from that buffer with no further synchronization. */
    monitor_init_buffer();
    monitor_setup_pio();

    /* TEMPORARY boot-timing instrumentation, see the comment at the top
     * of this function. */
    gpio_put(PIN_SD_BRIDGE_INT, 0);

    stdio_init_all();
    monitor_init_greenpak();
    mcu_config_init(); /* before the log: its size and flags are settings */
    mcu_log_init();
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
        /* LED starts OFF, no flash here (2026-09-22, REVERTED after a
         * real regression) -- a sleep_ms(150) boot-flash used to live
         * right here, and it broke real-hardware reads: it delayed
         * SetupReadServePio() (called inside monitor_run(), only after
         * this whole function returns) by 150ms+, past whatever window
         * the PC-1500's own one-time expansion-ROM boot scan uses --
         * found live, confirmed by a plain RP2350 reboot NOT fixing it
         * (the PC-1500 itself needed a full power cycle to re-scan and
         * detect the module again; the read path was never actually
         * broken, just not ready in time). This board's own established
         * "do no harm" boot priority (see f_mount()'s own comment above)
         * applies here just as much as it does to SD bring-up -- nothing
         * may delay monitor_run()'s own PIO setup. See monitor_run()'s
         * own comment for where the boot-flash moved to instead.
         *
         * CYW43_WL_GPIO_SMPS_PIN is unrelated (PWM-mode forcing, not the
         * LED) and stays untouched -- see this block's own comment above
         * for why it's load-bearing. */
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 0);
        cyw43_arch_gpio_put(CYW43_WL_GPIO_SMPS_PIN, 1);
        g_cyw43_up = true;
    }

    /* Clear both drive-activity LED flags before monitor_run()'s loop
     * ever looks at them (2026-09-22) -- monitor_init_greenpak() above
     * already made several real greenpak_i2c_write()/_read() calls
     * (forcing GreenPAK1/2 back to ROM_FROM_MCU), which set
     * g_i2c_activity_pending, but at a point the LED can't have shown it
     * anyway (cyw43_arch_init() hadn't even run yet). Left uncleared,
     * monitor_run()'s very first iteration would see that stale pending
     * flag and turn the LED straight back on with no real command ever
     * having run, and no corresponding g_command_done_pending to turn it
     * off again. */
    g_i2c_activity_pending = false;
    g_command_done_pending = false;

    multicore_launch_core1(monitor_command_worker);
    monitor_run(); /* never returns -- does its own brief boot-flash
                       once its PIO setup is already live, see its own
                       comment */
}
