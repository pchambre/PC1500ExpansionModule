/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* main.c -- BGM220S provisioner for the internal card (2026-11-16). A
 * separate RP2354B image: flash it with the card out of the PC-1500, read
 * its report on the USB console, then flash the card firmware back.
 *
 * Over SWD (GPIO38 SWCLK -> PA01, GPIO39 SWDIO -> PA02; RESETn on GPIO36):
 *   1. reset the module and halt its core in the window right after reset
 *      (below), then read DEVINFO;
 *   2. compare its flash with the embedded image (module_image.S);
 *   3. only if they differ, and only in a build with BGM220_DRY_RUN=0:
 *      erase the pages the image covers, program it, read it back;
 *   4. release the core and reset the module, so it runs.
 *
 * The flash controller sequence is emlib's (em_msc.c MSC_ErasePage,
 * MSC_WriteWord/writeBurst, mscStatusWait) for Series 2, with the register
 * offsets and bits from the SDK's bgm22_msc.h / bgm22_cmu.h. NVM3 and
 * anything else past the image are left alone. */
#include <stdio.h>
#include <string.h>

#include "board_pins_card.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"
#include "swd.h"

#ifndef BGM220_DRY_RUN
#define BGM220_DRY_RUN 1
#endif

extern const uint8_t module_image[], module_image_end[];

/* Cortex-M33 debug */
#define DHCSR 0xE000EDF0u
#define DHCSR_KEY 0xA05F0000u
#define DHCSR_C_DEBUGEN (1u << 0)
#define DHCSR_C_HALT (1u << 1)
#define DHCSR_S_HALT (1u << 17)

/* EFR32xG22 / BGM22 (bgm220sc22wga.h, bgm22_devinfo.h) */
#define DEVINFO_PART 0x0FE08004u
#define DEVINFO_MSIZE 0x0FE0800Cu
#define FLASH_PAGE 0x2000u
/* CMU and MSC each have a secure (0x4...) and a non-secure (0x5...) alias;
 * the module's firmware can make either the live one (SMU) early in its
 * start-up, and the other then reads 0 and drops writes. g_*_base is the
 * alias that answered (find_aliases()). */
static uint32_t g_cmu_base = 0x40008000u, g_msc_base = 0x40030000u;
#define CMU_STATUS (g_cmu_base + 0x008u)
#define CMU_STATUS_LOCKED (1u << 31)
#define CMU_LOCK (g_cmu_base + 0x010u)
#define CMU_LOCK_UNLOCK 0x93F7u
#define CMU_CLKEN1 (g_cmu_base + 0x068u)
#define CMU_CLKEN1_MSC (1u << 17)
#define MSC_WRITECTRL (g_msc_base + 0x00Cu)
#define MSC_WRITECMD (g_msc_base + 0x010u)
#define MSC_ADDRB (g_msc_base + 0x014u)
#define MSC_WDATA (g_msc_base + 0x018u)
#define MSC_STATUS (g_msc_base + 0x01Cu)
#define MSC_LOCK (g_msc_base + 0x03Cu)
#define MSC_LOCK_UNLOCK 0x1B71u
#define WRITECTRL_WREN (1u << 0)
#define WRITECMD_ERASEPAGE (1u << 1)
#define WRITECMD_WRITEEND (1u << 2)
#define STATUS_BUSY (1u << 0)
#define STATUS_LOCKED (1u << 1)
#define STATUS_INVADDR (1u << 2)
#define STATUS_WDATAREADY (1u << 3)
#define STATUS_PENDING (1u << 5)
#define STATUS_TIMEOUT (1u << 6)
#define STATUS_REGLOCK (1u << 16)

static const char *g_result = "not run";

static void wait_for_usb_terminal(void) {
    for (int waited_ms = 0; !stdio_usb_connected() && waited_ms < 10000; waited_ms += 100) sleep_ms(100);
    sleep_ms(200);
}

static void reset_pulse(void) {
    gpio_init(PIN_BLE_RST);
    gpio_put(PIN_BLE_RST, 0);
    gpio_set_dir(PIN_BLE_RST, GPIO_OUT);
    sleep_ms(5);
    gpio_set_dir(PIN_BLE_RST, GPIO_IN); /* released: the module's pull-up */
    gpio_disable_pulls(PIN_BLE_RST);
}

static uint32_t image_word(uint32_t i) {
    uint32_t w;
    memcpy(&w, module_image + 4u * i, 4);
    return w;
}

static uint32_t crc32(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

/* Words of the image that differ from the module's flash, or -1 on an SWD
 * error. Stops counting at `stop_after` (0: count them all). */
static long compare(uint32_t words, long stop_after) {
    long diff = 0;
    for (uint32_t i = 0; i < words; i++) {
        uint32_t v;
        if (!swd_read32(4u * i, &v)) {
            printf("  read at 0x%05lX failed (ack %d)\n", (unsigned long)(4u * i), swd_last_ack());
            return -1;
        }
        if (v != image_word(i)) {
            if (diff < 4)
                printf("  0x%05lX: flash %08lX, image %08lX\n", (unsigned long)(4u * i), (unsigned long)v,
                       (unsigned long)image_word(i));
            if (++diff == stop_after) break;
        }
    }
    return diff;
}

/* emlib mscStatusWait(): 0 ok, -1 timeout, -2 INVADDR, -3 locked, -4 SWD */
static int msc_wait(uint32_t mask, uint32_t value) {
    for (int i = 0; i < 100000; i++) {
        uint32_t st;
        if (!swd_read32(MSC_STATUS, &st)) return -4;
        if (st & STATUS_INVADDR) return -2;
        if ((st & mask) == value) return (st & (STATUS_LOCKED | STATUS_REGLOCK)) ? -3 : 0;
    }
    return -1;
}

static bool msc_done(void) {
    /* "We need to check twice to be sure" (emlib writeBurst) */
    return msc_wait(STATUS_BUSY | STATUS_PENDING, 0) == 0 && msc_wait(STATUS_BUSY | STATUS_PENDING, 0) == 0;
}

/* The CMU alias whose CLKEN1 takes the MSC clock-enable bit, and the MSC
 * alias whose STATUS then answers (MSC STATUS has PWRON, bit 24, set). */
static bool find_aliases(void) {
    static const uint32_t cmu[2] = {0x40008000u, 0x50008000u}, msc[2] = {0x40030000u, 0x50030000u};
    for (int a = 0; a < 2; a++) {
        g_cmu_base = cmu[a];
        uint32_t v = 0;
        swd_write32(CMU_LOCK, CMU_LOCK_UNLOCK);
        if (!swd_read32(CMU_CLKEN1, &v)) continue;
        swd_write32(CMU_CLKEN1, v | CMU_CLKEN1_MSC);
        if (!swd_read32(CMU_CLKEN1, &v) || !(v & CMU_CLKEN1_MSC)) {
            printf("  CMU at %08lX: CLKEN1 %08lX, enable didn't take\n", (unsigned long)g_cmu_base, (unsigned long)v);
            continue;
        }
        for (int b = 0; b < 2; b++) {
            uint32_t st = 0;
            g_msc_base = msc[b];
            if (swd_read32(MSC_STATUS, &st) && st != 0) {
                printf("  CMU at %08lX (CLKEN1 %08lX), MSC at %08lX (STATUS %08lX)\n", (unsigned long)g_cmu_base,
                       (unsigned long)v, (unsigned long)g_msc_base, (unsigned long)st);
                return true;
            }
        }
    }
    printf("  neither CMU/MSC alias answers (ack %d)\n", swd_last_ack());
    return false;
}

static bool program(uint32_t words) {
    if (!find_aliases()) return false;
    /* The MSC's bus clock. Firmware already on the module may have locked
     * the CMU (writes then ACK but don't take) and turned the clock off
     * (emlib MSC_Deinit), so unlock the CMU first and check it took. */
    uint32_t cmu_st = 0, v = 0, st = 0, d = 0;
    swd_read32(CMU_STATUS, &cmu_st);
    swd_write32(CMU_LOCK, CMU_LOCK_UNLOCK);
    bool clk = false;
    int tries = 0;
    for (; tries < 50 && !clk; tries++) {
        if (tries) sleep_ms(1);
        if (swd_read32(CMU_CLKEN1, &v) && (v & CMU_CLKEN1_MSC)) { clk = true; break; }
        /* the plain register, then its SET alias (+0x1000) */
        swd_write32(tries & 1 ? CMU_CLKEN1 + 0x1000u : CMU_CLKEN1, tries & 1 ? CMU_CLKEN1_MSC : v | CMU_CLKEN1_MSC);
    }
    swd_read32(DHCSR, &d);
    if (!clk) {
        printf("  MSC clock: couldn't enable after %d tries (CMU STATUS %08lX, CLKEN1 %08lX, DHCSR %08lX, ack %d)\n",
               tries, (unsigned long)cmu_st, (unsigned long)v, (unsigned long)d, swd_last_ack());
        return false;
    }
    printf("  MSC clock on after %d tries (CLKEN1 %08lX, DHCSR %08lX)\n", tries, (unsigned long)v, (unsigned long)d);
    if (!swd_write32(MSC_LOCK, MSC_LOCK_UNLOCK) || !swd_write32(MSC_WRITECTRL, WRITECTRL_WREN) ||
        !swd_read32(MSC_STATUS, &st)) {
        printf("  MSC unlock/WREN failed (ack %d)\n", swd_last_ack());
        return false;
    }
    printf("  MSC ready (CMU was %s, STATUS %08lX)\n", (cmu_st & CMU_STATUS_LOCKED) ? "locked" : "unlocked",
           (unsigned long)st);

    uint32_t bytes = 4u * words;
    uint32_t pages = (bytes + FLASH_PAGE - 1u) / FLASH_PAGE;
    printf("  erasing %lu pages...\n", (unsigned long)pages);
    for (uint32_t p = 0; p < pages; p++) {
        if (!swd_write32(MSC_ADDRB, p * FLASH_PAGE) || !swd_write32(MSC_WRITECMD, WRITECMD_ERASEPAGE)) return false;
        if (!msc_done()) {
            printf("  erase of page %lu failed\n", (unsigned long)p);
            return false;
        }
    }
    printf("  programming %lu bytes...\n", (unsigned long)bytes);
    for (uint32_t addr = 0; addr < bytes;) {
        uint32_t end = (addr + FLASH_PAGE) & ~(FLASH_PAGE - 1u); /* a burst stops at the page boundary */
        if (end > bytes) end = bytes;
        if (!swd_write32(MSC_ADDRB, addr)) return false;
        uint32_t st;
        if (!swd_read32(MSC_STATUS, &st) || (st & STATUS_INVADDR)) {
            printf("  invalid address 0x%05lX\n", (unsigned long)addr);
            return false;
        }
        if (!swd_write32(MSC_WDATA, image_word(addr / 4u))) return false;
        for (addr += 4u; addr < end; addr += 4u) {
            if (msc_wait(STATUS_WDATAREADY, STATUS_WDATAREADY) != 0) {
                swd_write32(MSC_WRITECMD, WRITECMD_WRITEEND);
                printf("  write stalled at 0x%05lX\n", (unsigned long)addr);
                return false;
            }
            if (!swd_write32(MSC_WDATA, image_word(addr / 4u))) return false;
        }
        if (!swd_write32(MSC_WRITECMD, WRITECMD_WRITEEND) || !msc_done()) {
            printf("  write end failed before 0x%05lX\n", (unsigned long)end);
            return false;
        }
    }
    swd_write32(MSC_WRITECTRL, 0);
    return true;
}

#ifdef BGM220_CHECK_ONLY
/* Read-only check of the RUNNING module (2026-11-16): attach without reset
 * or halt and read the crystal calibration (DEVINFO MODULEINFO/MODXOCAL,
 * the factory CTUNE for modules) against what the HFXO is actually using
 * (HFXO0 XTALCTRL), plus the module's name. Offsets from bgm22_devinfo.h /
 * bgm22_hfxo.h. Writes nothing to the module. */
/* A diagnostic module build's log ring (diag.c): magic "MDG1", its own
 * address, cap | entry size << 16, seq, boots; 16-byte entries from +32. */
static void dump_ring(void) {
    uint32_t at = 0;
    for (uint32_t a = 0x20000000u; a < 0x20008000u && !at; a += 4u) {
        uint32_t v = 0, self = 0;
        if (!swd_read32(a, &v)) {
            printf("ring scan: read at %08lX failed (ack %d)\n", (unsigned long)a, swd_last_ack());
            return;
        }
        if (v == 0x3147444Du && swd_read32(a + 4u, &self) && self == a) at = a;
    }
    if (!at) {
        printf("ring: none\n");
        return;
    }
    uint32_t h[5];
    for (int i = 0; i < 5; i++) swd_read32(at + 4u * i, &h[i]);
    uint32_t cap = h[2] & 0xFFFF, seq = h[3];
    printf("ring at %08lX: cap %lu, entry %lu, seq %lu, boots %lu\n", (unsigned long)at, (unsigned long)cap,
           (unsigned long)(h[2] >> 16), (unsigned long)seq, (unsigned long)h[4]);
    uint32_t from = seq > cap ? seq - cap : 0;
    if (seq - from > 40) from = seq - 40;
    for (uint32_t s = from; s < seq; s++) {
        uint32_t w[4];
        for (int i = 0; i < 4; i++) swd_read32(at + 32u + 16u * (s % cap) + 4u * i, &w[i]);
        printf("  #%lu t=%lu ms type %lu a %lu b %lu c %08lX d %08lX\n", (unsigned long)s, (unsigned long)w[0],
               (unsigned long)(w[1] & 0xFF), (unsigned long)(w[1] >> 8 & 0xFF), (unsigned long)(w[1] >> 16),
               (unsigned long)w[2], (unsigned long)w[3]);
    }
}

#ifndef BGM220_CHECK_SETTLE_MS
#define BGM220_CHECK_SETTLE_MS 500
#endif
static const char *check_only(void) {
    uint32_t idcode = 0, idr = 0, rb = 0;
    /* RESETn: the RP2354B's default pull-down (BOOTSEL, and here until
     * now) fights the module's pull-up and holds it in reset -- the module
     * was "unreachable while running" because it wasn't running
     * (2026-11-17). Release it and give the firmware time to start. */
    gpio_init(PIN_BLE_RST);
    gpio_set_dir(PIN_BLE_RST, GPIO_IN);
    gpio_disable_pulls(PIN_BLE_RST);
    sleep_ms(BGM220_CHECK_SETTLE_MS);
    swd_init(PIN_BLE_SWCLK, PIN_BLE_SWDIO);
    bool ok = false, halted = false;
    uint32_t t0 = time_us_32();
    while (!ok && time_us_32() - t0 < 1000000u) {
        ok = swd_connect(&idcode) && swd_power_up(&idr) && swd_set_csw(0x23000002u, &rb) && (rb & 0x40u);
        if (!ok) sleep_ms(2);
    }
    if (ok) {
        printf("Attached to the running module (IDCODE %08lX, CSW %08lX)\n", (unsigned long)idcode,
               (unsigned long)rb);
    } else {
        /* Asleep and out of reach: reset, let our firmware set its clocks
         * up, then halt it to read them (released again below). */
        printf("Not reachable while running (ack %d, CSW %08lX): reset, 300 ms, halt\n", swd_last_ack(),
               (unsigned long)rb);
        reset_pulse();
        sleep_ms(300);
        t0 = time_us_32();
        while (!halted && time_us_32() - t0 < 3000000u) {
            uint32_t d = 0;
            if (swd_connect(&idcode) && swd_power_up(&idr) &&
                swd_write32(DHCSR, DHCSR_KEY | DHCSR_C_DEBUGEN | DHCSR_C_HALT) && swd_read32(DHCSR, &d) &&
                (d & DHCSR_S_HALT))
                halted = true;
        }
        if (!halted) return "module not reachable over SWD, even after reset";
        printf("Halted %lu us after the 300 ms\n", (unsigned long)(time_us_32() - t0));
    }
    char name[29] = {0};
    for (int i = 0; i < 7; i++) {
        uint32_t w = 0;
        swd_read32(0x0FE08130u + 4u * i, &w);
        memcpy(name + 4 * i, &w, 4);
    }
    for (int i = 0; i < 28; i++)
        if (name[i] && (name[i] < 32 || name[i] > 126)) name[i] = '.';
    uint32_t info = 0, xocal = 0, xtal = 0, st = 0;
    ok = swd_read32(0x0FE0814Cu, &info) && swd_read32(0x0FE08150u, &xocal) &&
         swd_read32(0x4000C018u, &xtal) && swd_read32(0x4000C058u, &st);
    dump_ring();
    if (halted) swd_write32(DHCSR, DHCSR_KEY); /* let it run on */
    printf("MODULENAME \"%s\"\n", name);
    printf("MODULEINFO %08lX (HFXOCALVAL %lu: %s), MODXOCAL %08lX (factory CTUNE %lu)\n", (unsigned long)info,
           (unsigned long)((info >> 19) & 1u), ((info >> 19) & 1u) ? "NOT valid" : "valid", (unsigned long)xocal,
           (unsigned long)(xocal & 0xFFu));
    printf("HFXO XTALCTRL %08lX: CTUNE XI %lu, XO %lu; HFXO STATUS %08lX\n", (unsigned long)xtal,
           (unsigned long)((xtal >> 8) & 0xFFu), (unsigned long)((xtal >> 16) & 0xFFu), (unsigned long)st);
    return ok ? "check done (nothing written)" : "check: some reads failed (see above)";
}
#endif

static const char *run(void) {
#ifdef BGM220_CHECK_ONLY
    return check_only();
#endif
    uint32_t len = (uint32_t)(module_image_end - module_image);
    uint32_t words = len / 4u;
    printf("Image: %lu bytes, CRC32 %08lX\n", (unsigned long)len, (unsigned long)crc32(module_image, len));

    swd_init(PIN_BLE_SWCLK, PIN_BLE_SWDIO);

    /* Connect under reset (2026-11-16): the debug port answers, but the
     * MEM-AP reports DeviceEn=0 and every memory access FAULTs -- the
     * module's core isn't reachable, as when firmware already on it keeps
     * it in a low-power mode. So hold RESETn, connect, release, and retry
     * the halt until the core takes it, right after reset. */
    gpio_init(PIN_BLE_RST);
    gpio_put(PIN_BLE_RST, 0);
    gpio_set_dir(PIN_BLE_RST, GPIO_OUT);
    sleep_ms(5);
    uint32_t idcode = 0, idr = 0;
    bool ok = swd_connect(&idcode) && swd_power_up(&idr);
    printf("Under reset: %s (IDCODE %08lX, AP0 IDR %08lX, ack %d)\n", ok ? "connected" : "no connection",
           (unsigned long)idcode, (unsigned long)idr, swd_last_ack());
    /* DHCSR.C_HALT and DEMCR.VC_CORERESET while still in reset, if the AP
     * takes them -- then the core stops at its first instruction. */
    if (ok) {
        bool h = swd_write32(DHCSR, DHCSR_KEY | DHCSR_C_DEBUGEN | DHCSR_C_HALT);
        bool v = swd_write32(0xE000EDFCu, 1u); /* DEMCR.VC_CORERESET */
        printf("Under reset: halt request %s, vector catch %s\n", h ? "taken" : "refused", v ? "taken" : "refused");
    }

    gpio_set_dir(PIN_BLE_RST, GPIO_IN); /* release */
    gpio_disable_pulls(PIN_BLE_RST);
    uint32_t t0 = time_us_32(), dhcsr = 0;
    int tries = 0;
    bool halted = false;
    while (!halted && time_us_32() - t0 < 2000000u) {
        tries++;
        if (!ok) ok = swd_connect(&idcode) && swd_power_up(&idr);
        if (!ok) continue;
        if (swd_write32(DHCSR, DHCSR_KEY | DHCSR_C_DEBUGEN | DHCSR_C_HALT) && swd_read32(DHCSR, &dhcsr) &&
            (dhcsr & DHCSR_S_HALT)) {
            halted = true;
            break;
        }
        if (swd_last_ack() != SWD_ACK_FAULT) ok = false; /* lost the DP: reconnect */
    }
    uint32_t rb = 0;
    swd_set_csw(0x23000002u, &rb);
    printf("After release: %s after %lu us, %d tries (DHCSR %08lX, CSW %08lX, ack %d)\n",
           halted ? "HALTED" : "not halted", (unsigned long)(time_us_32() - t0), tries, (unsigned long)dhcsr,
           (unsigned long)rb, swd_last_ack());
    if (!halted) return "couldn't halt the module's core (DeviceEn never set?)";
    printf("DP IDCODE %08lX, AP0 IDR %08lX\n", (unsigned long)idcode, (unsigned long)idr);
    uint32_t part = 0, msize = 0;
    swd_read32(DEVINFO_PART, &part);
    swd_read32(DEVINFO_MSIZE, &msize);
    printf("DEVINFO PART %08lX, MSIZE %08lX (flash %lu KB)\n", (unsigned long)part, (unsigned long)msize,
           (unsigned long)(msize & 0xFFFFu));

    printf("Comparing flash with the image...\n");
    long diff = compare(words, 0);
    const char *result;
    if (diff < 0) {
        result = "compare failed (SWD read error)";
    } else if (diff == 0) {
        printf("Up to date.\n");
        result = "module already up to date";
    } else {
        printf("%ld words differ.\n", diff);
        if (BGM220_DRY_RUN) {
            result = "[DRY RUN] module differs from the image, not written";
        } else if (!program(words)) {
            result = "programming FAILED";
        } else {
            printf("Verifying...\n");
            diff = compare(words, 0);
            result = diff == 0 ? "programmed and verified OK" : "programmed, but VERIFY FAILED";
        }
    }

    swd_write32(DHCSR, DHCSR_KEY); /* release the core */
    reset_pulse();                 /* and run the (new) firmware */
    return result;
}

int main(void) {
    stdio_init_all();
    setvbuf(stdout, NULL, _IONBF, 0);
    wait_for_usb_terminal();
    printf("BGM220S provisioner -- dry_run=%d\n", BGM220_DRY_RUN);
    g_result = run();
    printf("Done.\n");
    while (true) { /* re-announce only; never re-runs the programming */
        printf("\n[idle] BGM220S provisioner -- dry_run=%d -- result: %s\n", BGM220_DRY_RUN, g_result);
        sleep_ms(5000);
    }
}
