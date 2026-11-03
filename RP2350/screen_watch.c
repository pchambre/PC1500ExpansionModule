/* screen_watch.c -- see screen_watch.h. */
#include "screen_watch.h"

#include "ble_link.h"
#include "board_pins.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "mcu_config.h"
#include "monitor.h"
#include "pico/time.h"
#include "screen.h"
#include "screen_watch.pio.h"

/* 8192 samples (32 KB): covers core1 being busy with a long command for a
 * while even during a LIST. A sample is GPIO0-23: A0-A12 in bits 0-12,
 * D0-D7 from DATA_PIN_BASE. */
#define RING_LOG2 13u
#define RING_N (1u << RING_LOG2)
static uint32_t g_ring[RING_N] __attribute__((aligned(RING_N * sizeof(uint32_t))));

static uint g_dma;
static uint32_t g_rd;

void screen_watch_drain(void) {
    static uint32_t gen;
    uint32_t wr = (((uint32_t)(uintptr_t)dma_channel_hw_addr(g_dma)->write_addr -
                    (uint32_t)(uintptr_t)g_ring) / sizeof(uint32_t)) & (RING_N - 1u);
    if (gen != screen_generation()) { /* BLDISP turned on: nothing from before, the ring's not drained while off */
        gen = screen_generation();
        g_rd = wr;
    }
    while (g_rd != wr) {
        uint32_t s = g_ring[g_rd];
        g_rd = (g_rd + 1u) & (RING_N - 1u);
        /* GP2 only fires for 7000H-7FFFH: A12-A14 are 1 there, A15 0. */
        screen_watch_write((uint16_t)(0x6000u | (s & 0x1FFFu)), (uint8_t)(s >> DATA_PIN_BASE));
    }
}

/* ---- screen.h's backend ---- */
static uint32_t b_session(void *ctx) {
    (void)ctx;
    return ble_link_session();
}
static int b_display(void *ctx, const uint8_t *payload, uint16_t len) {
    (void)ctx;
    return ble_link_display(payload, len);
}
static uint16_t b_display_max(void *ctx) {
    (void)ctx;
    return ble_link_display_max();
}
static bool b_enabled(void *ctx) {
    (void)ctx;
    return mcu_config_get(MCU_CONFIG_BLDISP) != 0;
}
static uint32_t b_now_us(void *ctx) {
    (void)ctx;
    return time_us_32();
}
static int b_rom_byte(void *ctx, uint16_t addr) {
    (void)ctx;
    return monitor_rom_image_byte(addr);
}
static void b_drain(void *ctx) {
    (void)ctx;
    screen_watch_drain();
}

void screen_watch_init(void) {
    static const screen_backend_t backend = {
        .session = b_session,
        .display = b_display,
        .display_max = b_display_max,
        .enabled = b_enabled,
        .now_us = b_now_us,
        .rom_byte = b_rom_byte,
        .drain = b_drain,
    };
    screen_init(&backend);

    uint offset = pio_add_program(pio2, &screen_watch_program);
    uint sm = pio_claim_unused_sm(pio2, true);
    pio_sm_config c = screen_watch_program_get_default_config(offset);

    /* Low with nothing driving it. On the dongle the SD bridge's driver
     * pulls it up instead when it takes the pin as INT (BRIDGEINT=1). */
    gpio_init(PIN_SCREEN_TRIG);
    gpio_set_dir(PIN_SCREEN_TRIG, GPIO_IN);
    gpio_set_pulls(PIN_SCREEN_TRIG, false, true);

    sm_config_set_in_pins(&c, 0);                /* GPIO0: A0 */
    sm_config_set_jmp_pin(&c, PIN_SCREEN_TRIG);
    sm_config_set_in_shift(&c, false, true, 24); /* shift left: the sample right-justified */
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    sm_config_set_clkdiv(&c, 1.0f);
    pio_sm_init(pio2, sm, offset, &c);

    /* The ring: the write address wraps at RING_N words; ENDLESS count
     * (RP2350 TRANS_COUNT mode 0xF), so it never stops. */
    g_dma = dma_claim_unused_channel(true);
    dma_channel_config d = dma_channel_get_default_config(g_dma);
    channel_config_set_transfer_data_size(&d, DMA_SIZE_32);
    channel_config_set_read_increment(&d, false);
    channel_config_set_write_increment(&d, true);
    channel_config_set_ring(&d, true, RING_LOG2 + 2u);
    channel_config_set_dreq(&d, pio_get_dreq(pio2, sm, false));
    dma_channel_configure(g_dma, &d, g_ring, &pio2->rxf[sm], (0xFu << 28) | 1u, true);

    pio_sm_set_enabled(pio2, sm, true);
    ble_link_set_wait_hook(screen_watch_drain); /* during a frame's round trip too, or the ring overflows */
}
