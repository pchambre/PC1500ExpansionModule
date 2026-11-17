/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* swd.c -- see swd.h. Bit timing as CMSIS-DAP's SW_WRITE_BIT/SW_READ_BIT:
 * SWCLK idles high; the host changes SWDIO with SWCLK low and the target
 * samples on the rising edge; the target drives after the rising edge and
 * the host samples with SWCLK low. About 1 MHz. */
#include "swd.h"

#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

#define HALF_CYCLES 75u /* at 150 MHz: ~0.5 us per half period */
#define WAIT_RETRIES 200

/* DP registers (A[3:2] << 2) */
#define DP_IDCODE 0x0 /* read */
#define DP_ABORT 0x0  /* write */
#define DP_CTRL_STAT 0x4
#define DP_SELECT 0x8
#define DP_RDBUFF 0xC
/* MEM-AP registers */
#define AP_CSW 0x00
#define AP_TAR 0x04
#define AP_DRW 0x0C
#define AP_IDR 0xFC

static unsigned g_clk, g_dio;
static int g_ack;
static uint32_t g_select = 0xFFFFFFFFu;

static inline void delay(void) { busy_wait_at_least_cycles(HALF_CYCLES); }

static inline void dio_out(void) { gpio_set_dir(g_dio, GPIO_OUT); }
static inline void dio_in(void) { gpio_set_dir(g_dio, GPIO_IN); }

static inline void write_bit(bool b) {
    gpio_put(g_dio, b);
    gpio_put(g_clk, 0);
    delay();
    gpio_put(g_clk, 1);
    delay();
}

static inline bool read_bit(void) {
    gpio_put(g_clk, 0);
    delay();
    bool b = gpio_get(g_dio);
    gpio_put(g_clk, 1);
    delay();
    return b;
}

static inline void turnaround(void) {
    gpio_put(g_clk, 0);
    delay();
    gpio_put(g_clk, 1);
    delay();
}

static void write_bits(uint32_t v, int n) {
    for (int i = 0; i < n; i++, v >>= 1) write_bit(v & 1u);
}

static uint32_t read_bits(int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++)
        if (read_bit()) v |= 1u << i;
    return v;
}

void swd_init(unsigned swclk_gpio, unsigned swdio_gpio) {
    g_clk = swclk_gpio;
    g_dio = swdio_gpio;
    gpio_init(g_clk);
    gpio_put(g_clk, 1);
    gpio_set_dir(g_clk, GPIO_OUT);
    gpio_init(g_dio);
    gpio_pull_up(g_dio);
    gpio_put(g_dio, 1);
    dio_out();
}

static void ones(int n) {
    dio_out();
    for (int i = 0; i < n; i++) write_bit(1);
}

/* >= 50 high, then idle: a line reset ready for a transfer. */
static void line_reset(void) {
    ones(56);
    for (int i = 0; i < 8; i++) write_bit(0);
}

/* ADIv5.2 B5.3: selection alert, then the SWD activation code, from any
 * state (as OpenOCD's swd_seq_dormant_to_swd). */
static void dormant_to_swd(void) {
    static const uint32_t alert[4] = {0x6209F392u, 0x86852D95u, 0xE3DDAFE9u, 0x19BC0EA2u};
    dio_out();
    for (int i = 0; i < 8; i++) write_bit(1);
    for (int i = 0; i < 4; i++) write_bits(alert[i], 32);
    write_bits(0x0, 4);
    write_bits(0x1A, 8);
}

/* One transfer. req: APnDP (bit 0), RnW (bit 1), A[3:2] (bits 2-3). */
static int transfer_once(bool ap, bool rnw, uint8_t a, uint32_t *data) {
    uint8_t a23 = (uint8_t)((a >> 2) & 3u);
    uint8_t bits = (uint8_t)((ap ? 1 : 0) | (rnw ? 2 : 0) | (a23 << 2));
    uint8_t parity = (uint8_t)(__builtin_popcount(bits) & 1);
    uint8_t req = (uint8_t)(1u | bits << 1 | parity << 5 | 0u << 6 | 1u << 7);
    dio_out();
    write_bits(req, 8);
    dio_in();
    turnaround();
    int ack = (int)read_bits(3);
    if (ack == SWD_ACK_OK) {
        if (rnw) {
            uint32_t v = read_bits(32);
            bool p = read_bit();
            turnaround();
            dio_out();
            if (p != (__builtin_popcount(v) & 1)) ack = SWD_ACK_PARITY;
            else *data = v;
        } else {
            turnaround();
            dio_out();
            write_bits(*data, 32);
            write_bit(__builtin_popcount(*data) & 1);
        }
    } else {
        turnaround();
        dio_out();
    }
    write_bits(0, 8); /* idle */
    return ack;
}

static bool transfer(bool ap, bool rnw, uint8_t a, uint32_t *data) {
    for (int i = 0; i < WAIT_RETRIES; i++) {
        g_ack = transfer_once(ap, rnw, a, data);
        if (g_ack != SWD_ACK_WAIT) break;
    }
    if (g_ack == SWD_ACK_FAULT) {
        uint32_t clr = 0x1E; /* STKCMPCLR | STKERRCLR | WDERRCLR | ORUNERRCLR */
        transfer_once(false, false, DP_ABORT, &clr);
    }
    return g_ack == SWD_ACK_OK;
}

static bool dp_read(uint8_t a, uint32_t *v) { return transfer(false, true, a, v); }
static bool dp_write(uint8_t a, uint32_t v) { return transfer(false, false, a, &v); }

static bool ap_select(uint8_t a) {
    uint32_t sel = (0u << 24) | (uint32_t)(a & 0xF0u); /* AP 0, bank */
    if (sel == g_select) return true;
    if (!dp_write(DP_SELECT, sel)) return false;
    g_select = sel;
    return true;
}

static bool ap_write(uint8_t a, uint32_t v) {
    return ap_select(a) && transfer(true, false, (uint8_t)(a & 0x0Cu), &v);
}

static bool ap_read(uint8_t a, uint32_t *v) {
    uint32_t posted;
    return ap_select(a) && transfer(true, true, (uint8_t)(a & 0x0Cu), &posted) && dp_read(DP_RDBUFF, v);
}

bool swd_connect(uint32_t *idcode) {
    g_select = 0xFFFFFFFFu;
    /* JTAG-to-SWD: >= 50 high, 0xE79E, >= 50 high -- no idle cycles before
     * the 0xE79E, or the switch isn't recognised -- then idle. */
    ones(56);
    write_bits(0xE79E, 16);
    line_reset();
    if (dp_read(DP_IDCODE, idcode)) return true;
    dormant_to_swd();
    line_reset();
    return dp_read(DP_IDCODE, idcode);
}

bool swd_power_up(uint32_t *ap_idr) {
    uint32_t v = 0;
    if (!dp_write(DP_ABORT, 0x1E)) return false;
    if (!dp_write(DP_SELECT, 0)) return false;
    g_select = 0;
    if (!dp_write(DP_CTRL_STAT, 0x50000000u)) return false; /* CSYSPWRUPREQ | CDBGPWRUPREQ */
    for (int i = 0; i < 100; i++) {
        if (!dp_read(DP_CTRL_STAT, &v)) return false;
        if ((v & 0xA0000000u) == 0xA0000000u) break; /* both ACKs */
        sleep_ms(1);
    }
    if ((v & 0xA0000000u) != 0xA0000000u) return false;
    if (!ap_read(AP_IDR, ap_idr)) return false;
    /* 32-bit, no auto-increment; bit 30 clear = secure transfers (MSC_S). */
    return ap_write(AP_CSW, 0x23000002u);
}

bool swd_read32(uint32_t addr, uint32_t *value) { return ap_write(AP_TAR, addr) && ap_read(AP_DRW, value); }

bool swd_write32(uint32_t addr, uint32_t value) { return ap_write(AP_TAR, addr) && ap_write(AP_DRW, value); }

int swd_last_ack(void) { return g_ack; }

bool swd_set_csw(uint32_t csw, uint32_t *readback) { return ap_write(AP_CSW, csw) && ap_read(AP_CSW, readback); }

