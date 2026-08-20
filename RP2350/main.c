#include "pico/stdlib.h"
#include "pico/multicore.h"

#include "ff.h"

#include "board_pins.h"
#include "monitor.h"
#include "qmi_cs1_spi.h"

/* Core0: one-time init (stdio, QMI-CS1 SD transport, FatFs mount), then
 * launches the bus-servicing/DoCommand loop on core1 and sits idle --
 * reserved for future WiFi (cyw43_arch) work, not part of this port. See
 * RP2350/README.md and the repo's plan history for the full writeup. */

static FATFS g_fatfs;

int main(void) {
    stdio_init_all();

    qmi_cs1_spi_init(PIN_SD_CS1);

    /* opt=1: mount immediately (not lazily on first file access) --
     * f_mount() calls disk_initialize() now, so a missing/dead SD card
     * is discovered at boot rather than silently on the first SDLS. The
     * return value isn't checked here beyond that: DoCommand()'s own SD
     * cases already handle and report a not-ready card via ordinary
     * f_* failures, same as the original PSoC5 side did through
     * emFile's own equivalent failure paths. */
    f_mount(&g_fatfs, "", 1);

    multicore_launch_core1(monitor_run);

    while (true) {
        tight_loop_contents();
    }
}
