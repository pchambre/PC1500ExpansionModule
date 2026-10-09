/* sd_bus_bridge.c -- sd_bus.h on the dongle: the SC18IS602B I2C-to-SPI
 * bridge (U5), sharing the GreenPAKs' I2C bus (2026-10-09; the calls
 * diskio_sd_bridge.c made directly before the card build existed). */
#include "sd_bus.h"

#include "board_pins.h"
#include "sc18is602b.h"

_Static_assert(SD_BUS_MAX_CHUNK == SC18IS602B_MAX_CHUNK, "sd_bus.h's chunk is the bridge's ceiling");

static greenpak_i2c_bus_t g_sd_bus;

bool sd_bus_open_slow(void) {
    g_sd_bus.sda_gpio = PIN_GREENPAK1_SDA;
    g_sd_bus.scl_gpio = PIN_GREENPAK1_SCL;
    greenpak_i2c_init(&g_sd_bus); /* idempotent -- shared bus, GreenPAKs init it too */
    return sc18is602b_configure(&g_sd_bus, SC18IS602B_MODE_CPOL0_CPHA0, SC18IS602B_CLK_58KHZ);
}

bool sd_bus_set_fast(void) {
    return sc18is602b_configure(&g_sd_bus, SC18IS602B_MODE_CPOL0_CPHA0, SC18IS602B_CLK_1843KHZ);
}

bool sd_bus_clock_only(uint32_t len) {
    return sc18is602b_clock_only(&g_sd_bus, len);
}

bool sd_bus_transfer(const uint8_t *tx, uint8_t *rx, uint32_t len) {
    return sc18is602b_transfer(&g_sd_bus, tx, rx, len);
}

void sd_bus_reset_stats(void) {
    uint32_t a, b, c;
    sc18is602b_get_and_reset_retry_stats(&a, &b);
    sc18is602b_get_and_reset_int_stats(&a, &b, &c);
}

void sd_bus_int_stats(uint32_t *outImmediate, uint32_t *outWaited, uint32_t *outTimeout) {
    sc18is602b_get_and_reset_int_stats(outImmediate, outWaited, outTimeout);
}
