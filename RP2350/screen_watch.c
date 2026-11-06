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

#if PIN_SCREEN_TRIG < 32
    /* One program on pio2 watches the trigger and samples GPIO0-23. */
    PIO pio = pio2;
    uint offset = pio_add_program(pio, &screen_watch_program);
    uint sm = pio_claim_unused_sm(pio, true);
    pio_sm_config c = screen_watch_program_get_default_config(offset);
#else
    /* The trigger is past GPIO31 (the RP2350B-Plus-W dongle's GPIO42):
     * the sampler on pio0, whose GPIO base 0 reaches the bus, and the
     * trigger through screen_trig_relay on a PIO whose base reaches it --
     * pio2 (base 16), since `irq next` from pio2 sets the flag in pio0
     * (screen_watch.pio). The radio shares pio2 at the same base. */
    PIO pio = pio0;
    uint offset = pio_add_program(pio, &screen_watch_irq_program);
    uint sm = pio_claim_unused_sm(pio, true);
    pio_sm_config c = screen_watch_irq_program_get_default_config(offset);
#endif

    /* Low with nothing driving it. On the dongle the SD bridge's driver
     * pulls it up instead when it takes the pin as INT (BRIDGEINT=1). */
    gpio_init(PIN_SCREEN_TRIG);
    gpio_set_dir(PIN_SCREEN_TRIG, GPIO_IN);
    gpio_set_pulls(PIN_SCREEN_TRIG, false, true);

    sm_config_set_in_pins(&c, 0);                /* GPIO0: A0 */
#if PIN_SCREEN_TRIG < 32
    sm_config_set_jmp_pin(&c, PIN_SCREEN_TRIG);
#endif
    sm_config_set_in_shift(&c, false, true, 24); /* shift left: the sample right-justified */
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    sm_config_set_clkdiv(&c, 1.0f);
    pio_sm_init(pio, sm, offset, &c);

#if PIN_SCREEN_TRIG >= 32
    PIO relay_pio;
    uint relay_sm, relay_offset;
    hard_assert(pio_claim_free_sm_and_add_program_for_gpio_range(&screen_trig_relay_program, &relay_pio,
                                                                 &relay_sm, &relay_offset, PIN_SCREEN_TRIG, 1,
                                                                 true));
    hard_assert(relay_pio == pio2); /* `irq next` must land in pio0 */
    pio_sm_config rc = screen_trig_relay_program_get_default_config(relay_offset);
    sm_config_set_jmp_pin(&rc, PIN_SCREEN_TRIG);
    sm_config_set_clkdiv(&rc, 1.0f);
    pio_sm_init(relay_pio, relay_sm, relay_offset, &rc);
#endif

    /* The ring: the write address wraps at RING_N words; ENDLESS count
     * (RP2350 TRANS_COUNT mode 0xF), so it never stops. */
    g_dma = dma_claim_unused_channel(true);
    dma_channel_config d = dma_channel_get_default_config(g_dma);
    channel_config_set_transfer_data_size(&d, DMA_SIZE_32);
    channel_config_set_read_increment(&d, false);
    channel_config_set_write_increment(&d, true);
    channel_config_set_ring(&d, true, RING_LOG2 + 2u);
    channel_config_set_dreq(&d, pio_get_dreq(pio, sm, false));
    dma_channel_configure(g_dma, &d, g_ring, &pio->rxf[sm], (0xFu << 28) | 1u, true);

    pio_sm_set_enabled(pio, sm, true);
#if PIN_SCREEN_TRIG >= 32
    pio_sm_set_enabled(relay_pio, relay_sm, true);
#endif
    ble_link_set_wait_hook(screen_watch_drain); /* during a frame's round trip too, or the ring overflows */
}
