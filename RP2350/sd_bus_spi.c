/* sd_bus_spi.c -- sd_bus.h on the internal card: SPI1 straight to the
 * microSD socket (2026-10-09, board_pins_card.h). Mode 0, MSB first; CS is a
 * plain GPIO so it can stay low across a whole transfer and stay high for
 * the power-up preamble. Polled, no DMA: diskio_sd.c moves at most one
 * 512-byte block per call. */
#include "sd_bus.h"

#include "hardware/gpio.h"
#include "hardware/spi.h"

#include "board_pins.h"

#define SD_SPI_INIT_HZ 400000u   /* the SD spec's ceiling until ACMD41 is done */

/* Data clocks, fastest first: 25 MHz is default-speed mode's ceiling;
 * diskio_sd.c steps down while the card misreads (sd_bus_slower()). */
static const uint32_t kFastHz[] = {25000000u, 12500000u, 6250000u, 3000000u, 1000000u};
static unsigned g_speed;
static uint32_t g_hz = SD_SPI_INIT_HZ;

static void sd_cs(bool asserted) {
    gpio_put(PIN_SD_CS, !asserted); /* active low */
}

/* Clocks `len` bytes of 0xFF, whatever CS is. */
static void sd_clock_ff(uint32_t len) {
    uint8_t sink;
    while (len--) spi_read_blocking(SD_SPI_INSTANCE, 0xFF, &sink, 1);
}

bool sd_bus_open_slow(void) {
    gpio_init(PIN_SD_CS);
    gpio_set_dir(PIN_SD_CS, GPIO_OUT);
    sd_cs(false);
    g_hz = spi_init(SD_SPI_INSTANCE, SD_SPI_INIT_HZ);
    spi_set_format(SD_SPI_INSTANCE, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(PIN_SD_MISO, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SD_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_SD_MOSI, GPIO_FUNC_SPI);
    gpio_pull_up(PIN_SD_MISO); /* the card's DO floats until it's selected */
    return true;
}

bool sd_bus_set_fast(void) {
    g_speed = 0;
    g_hz = spi_set_baudrate(SD_SPI_INSTANCE, kFastHz[0]);
    return true;
}

bool sd_bus_slower(void) {
    if (g_speed + 1 >= sizeof kFastHz / sizeof kFastHz[0]) return false;
    g_hz = spi_set_baudrate(SD_SPI_INSTANCE, kFastHz[++g_speed]);
    return true;
}

uint32_t sd_bus_clock_khz(void) {
    return g_hz / 1000u;
}

bool sd_bus_clock_only(uint32_t len) {
    sd_cs(false);
    sd_clock_ff(len);
    return true;
}

static bool g_hold;

void sd_bus_hold(bool on) {
    g_hold = on;
    if (!on) {
        sd_cs(false);
        sd_clock_ff(1); /* the card releases DO */
    }
}

bool sd_bus_transfer(const uint8_t *tx, uint8_t *rx, uint32_t len) {
    sd_cs(true);
    if (tx && rx) {
        spi_write_read_blocking(SD_SPI_INSTANCE, tx, rx, len);
    } else if (tx) {
        spi_write_blocking(SD_SPI_INSTANCE, tx, len);
    } else if (rx) {
        spi_read_blocking(SD_SPI_INSTANCE, 0xFF, rx, len);
    } else {
        sd_clock_ff(len);
    }
    if (!g_hold) {
        sd_cs(false);
        sd_clock_ff(1); /* one byte with CS high: the card releases DO */
    }
    return true;
}

void sd_bus_reset_stats(void) {}

void sd_bus_int_stats(uint32_t *outImmediate, uint32_t *outWaited, uint32_t *outTimeout) {
    *outImmediate = *outWaited = *outTimeout = 0;
}
