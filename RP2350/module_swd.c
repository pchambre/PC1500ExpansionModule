/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* module_swd.c -- see module_swd.h. Bit timing as CMSIS-DAP's
 * SW_WRITE_BIT/SW_READ_BIT: SWCLK idles high; the host changes SWDIO with
 * SWCLK low and the target samples on the rising edge; the target drives
 * after the rising edge and the host samples with SWCLK low. About 1 MHz. */
#include "module_swd.h"

#include "board_pins_card.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

#define WAIT_RETRIES 200

#define ACK_OK 1
#define ACK_WAIT 2
#define ACK_FAULT 4
#define ACK_PARITY 8 /* ours: a read's parity bit was wrong */

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

#define CSW_WORD 0x23000002u     /* 32-bit, secure, no auto-increment */
#define CSW_WORD_INC 0x23000012u /* ... single auto-increment */
#define CSW_DEVICEEN 0x40u

static uint32_t g_half; /* cycles per half bit */
static int g_ack;
static uint32_t g_select = 0xFFFFFFFFu;

static inline void delay(void) { busy_wait_at_least_cycles(g_half); }

static inline void dio_out(void) { gpio_set_dir(PIN_BLE_SWDIO, GPIO_OUT); }
static inline void dio_in(void) { gpio_set_dir(PIN_BLE_SWDIO, GPIO_IN); }

static inline void write_bit(bool b) {
    gpio_put(PIN_BLE_SWDIO, b);
    gpio_put(PIN_BLE_SWCLK, 0);
    delay();
    gpio_put(PIN_BLE_SWCLK, 1);
    delay();
}

static inline bool read_bit(void) {
    gpio_put(PIN_BLE_SWCLK, 0);
    delay();
    bool b = gpio_get(PIN_BLE_SWDIO);
    gpio_put(PIN_BLE_SWCLK, 1);
    delay();
    return b;
}

static inline void turnaround(void) {
    gpio_put(PIN_BLE_SWCLK, 0);
    delay();
    gpio_put(PIN_BLE_SWCLK, 1);
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

static void ones(int n) {
    dio_out();
    for (int i = 0; i < n; i++) write_bit(1);
}

static void line_reset(void) {
    ones(56);
    for (int i = 0; i < 8; i++) write_bit(0);
}

static void dormant_to_swd(void) {
    static const uint32_t alert[4] = {0x6209F392u, 0x86852D95u, 0xE3DDAFE9u, 0x19BC0EA2u};
    dio_out();
    for (int i = 0; i < 8; i++) write_bit(1);
    for (int i = 0; i < 4; i++) write_bits(alert[i], 32);
    write_bits(0x0, 4);
    write_bits(0x1A, 8);
}

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
    if (ack == ACK_OK) {
        if (rnw) {
            uint32_t v = read_bits(32);
            bool p = read_bit();
            turnaround();
            dio_out();
            if (p != (__builtin_popcount(v) & 1)) ack = ACK_PARITY;
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
        if (g_ack != ACK_WAIT) break;
    }
    if (g_ack == ACK_FAULT) {
        uint32_t clr = 0x1E; /* STKCMPCLR | STKERRCLR | WDERRCLR | ORUNERRCLR */
        transfer_once(false, false, DP_ABORT, &clr);
    }
    return g_ack == ACK_OK;
}

static bool dp_read(uint8_t a, uint32_t *v) { return transfer(false, true, a, v); }
static bool dp_write(uint8_t a, uint32_t v) { return transfer(false, false, a, &v); }

static bool ap_select(uint8_t a) {
    uint32_t sel = (uint32_t)(a & 0xF0u); /* AP 0, bank */
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

static bool connect_once(void) {
    uint32_t idcode, v = 0, idr, csw = 0;
    g_select = 0xFFFFFFFFu;
    ones(56);
    write_bits(0xE79E, 16);
    line_reset();
    if (!dp_read(DP_IDCODE, &idcode)) {
        dormant_to_swd();
        line_reset();
        if (!dp_read(DP_IDCODE, &idcode)) return false;
    }
    if (!dp_write(DP_ABORT, 0x1E) || !dp_write(DP_SELECT, 0)) return false;
    g_select = 0;
    if (!dp_write(DP_CTRL_STAT, 0x50000000u)) return false; /* CSYSPWRUPREQ | CDBGPWRUPREQ */
    for (int i = 0; i < 100; i++) {
        if (!dp_read(DP_CTRL_STAT, &v)) return false;
        if ((v & 0xA0000000u) == 0xA0000000u) break;
        sleep_us(100);
    }
    if ((v & 0xA0000000u) != 0xA0000000u) return false;
    if (!ap_read(AP_IDR, &idr)) return false;
    return ap_write(AP_CSW, CSW_WORD) && ap_read(AP_CSW, &csw) && (csw & CSW_DEVICEEN);
}

bool module_swd_attach(uint32_t ms) {
    g_half = clock_get_hz(clk_sys) / 2000000u; /* ~0.5 us */
    gpio_init(PIN_BLE_SWCLK);
    gpio_put(PIN_BLE_SWCLK, 1);
    gpio_set_dir(PIN_BLE_SWCLK, GPIO_OUT);
    gpio_init(PIN_BLE_SWDIO);
    gpio_pull_up(PIN_BLE_SWDIO);
    gpio_put(PIN_BLE_SWDIO, 1);
    dio_out();
    absolute_time_t until = make_timeout_time_ms(ms);
    do {
        if (connect_once()) return true;
        sleep_ms(2);
    } while (!time_reached(until));
    return false;
}

#define DHCSR 0xE000EDF0u
#define DHCSR_KEY 0xA05F0000u
#define DHCSR_C_DEBUGEN (1u << 0)
#define DHCSR_C_HALT (1u << 1)
#define DHCSR_S_HALT (1u << 17)

bool module_swd_reset_halt(uint32_t ms) {
    gpio_init(PIN_BLE_RST);
    gpio_put(PIN_BLE_RST, 0);
    gpio_set_dir(PIN_BLE_RST, GPIO_OUT);
    sleep_ms(5);
    gpio_set_dir(PIN_BLE_RST, GPIO_IN); /* released: the module's pull-up */
    gpio_disable_pulls(PIN_BLE_RST);
    absolute_time_t until = make_timeout_time_ms(ms);
    do {
        uint32_t d = 0;
        if (connect_once() && ap_write(AP_TAR, DHCSR) && ap_write(AP_DRW, DHCSR_KEY | DHCSR_C_DEBUGEN | DHCSR_C_HALT) &&
            module_swd_read32(DHCSR, &d) && (d & DHCSR_S_HALT))
            return true;
    } while (!time_reached(until));
    return false;
}

void module_swd_resume(void) { ap_write(AP_TAR, DHCSR) && ap_write(AP_DRW, DHCSR_KEY); }

bool module_swd_read32(uint32_t addr, uint32_t *value) { return ap_write(AP_TAR, addr) && ap_read(AP_DRW, value); }

bool module_swd_read_block(uint32_t addr, uint32_t *out, uint32_t n) {
    bool ok = ap_write(AP_CSW, CSW_WORD_INC);
    while (ok && n) {
        /* TAR only increments within a 1 KB block */
        uint32_t chunk = (0x400u - (addr & 0x3FFu)) / 4u, v;
        if (chunk > n) chunk = n;
        ok = ap_write(AP_TAR, addr) && ap_select(AP_DRW) && transfer(true, true, AP_DRW, &v); /* posted */
        for (uint32_t i = 1; ok && i < chunk; i++) {
            ok = transfer(true, true, AP_DRW, &v);
            out[i - 1] = v;
        }
        if (ok) ok = dp_read(DP_RDBUFF, &out[chunk - 1]);
        addr += 4u * chunk;
        out += chunk;
        n -= chunk;
    }
    return ap_write(AP_CSW, CSW_WORD) && ok;
}

void module_swd_detach(void) {
    dp_write(DP_CTRL_STAT, 0);
    gpio_set_dir(PIN_BLE_SWCLK, GPIO_IN);
    gpio_set_dir(PIN_BLE_SWDIO, GPIO_IN);
    gpio_pull_down(PIN_BLE_SWCLK);
    gpio_pull_up(PIN_BLE_SWDIO);
}
