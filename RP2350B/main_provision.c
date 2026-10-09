/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* main_provision.c -- GreenPAK NVM provisioning tool. Separate firmware
 * image from this board's normal main.c/monitor.c -- flash this
 * deliberately to program GP1/GP2's NVM, then flash the normal
 * firmware back for actual expansion-board operation.
 *
 * Workflow (see ../DESIGN_DECISIONS.md for full background). Every run
 * checks the TARGET addresses (0x18 GP1, 0x10 GP2) first, not the
 * factory-default address -- this is what lets a re-run safely
 * reprogram already-provisioned chips instead of just idling, and lets
 * "nothing done yet" be told apart from "one done, one pending"
 * regardless of jumper state:
 *   1. Both GP1 (0x18) and GP2 (0x10) already respond -- reprograms
 *      both in place (each chip stays at its own address throughout).
 *   2. Neither responds -- if a chip answers at the factory default
 *      (0x08), programs it as GP1. (With GP2's isolation jumper
 *      disconnected, only GP1 is on the bus, so this is naturally
 *      GP1's first run.)
 *   3. GP1 responds but GP2 doesn't -- if a second chip answers at the
 *      default address, programs it as GP2. (Reconnect GP2's isolation
 *      jumper before this run.)
 *   4. GP2 responds but GP1 doesn't -- symmetric fallback, programs a
 *      default-address chip as GP1.
 *   5. If nothing answers at any known address, drops into recovery
 *      mode: scans the full 7-bit address range and reports findings.
 *      Recovery mode never reprograms anything unless this firmware
 *      was built with -DGREENPAK_NVM_ALLOW_RECOVERY_WRITE=1 (a
 *      deliberately different build/flash, not a runtime prompt).
 *
 * All real writes are gated by GREENPAK_NVM_DRY_RUN (default 1 --
 * override via -DGREENPAK_NVM_DRY_RUN=0 to actually program a chip).
 * In dry-run mode, program_image() only reads back and reports
 * existing NVM content -- zero write risk.
 */
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/stdio_usb.h"

#include "board_pins.h"
#include "greenpak_i2c.h"
#include "greenpak_nvm.h"
#include "gp1_nvm_image.h"
#include "gp2_nvm_image.h"

#ifndef GREENPAK_NVM_DRY_RUN
#define GREENPAK_NVM_DRY_RUN 1
#endif

#ifndef GREENPAK_NVM_ALLOW_RECOVERY_WRITE
#define GREENPAK_NVM_ALLOW_RECOVERY_WRITE 0
#endif

#define GP1_CONTROL_CODE 0x3 /* GP1-SRAM.gp6's compiled control code -- confirmed against real hardware in GreenPakTester */
#define GP2_CONTROL_CODE 0x2 /* GP2-MCU.gp6's compiled control code -- confirmed against real hardware in GreenPakTester (10/10 self-test vectors passing) */

static greenpak_i2c_bus_t g_bus;

static void wait_for_usb_terminal(void) {
    for (int waited_ms = 0; !stdio_usb_connected() && waited_ms < 10000; waited_ms += 100) {
        sleep_ms(100);
    }
    sleep_ms(200);
}

static void report_page(const char *label, uint8_t control_code, bool ok) {
    printf("  %s: control_code=0x%X reg_addr=0x%02X nvm_addr=0x%02X %s\n",
           label, control_code,
           greenpak_nvm_reg_addr(control_code), greenpak_nvm_nvm_addr(control_code),
           ok ? "OK" : "FAIL");
}

/* `current_cc` is the control code the chip is ACTUALLY answering at
 * right now -- erase/write/read-back/reset_and_reload all target this
 * address, since that's the only address the chip currently listens
 * on. `target_cc` is what it should become after this image's reload
 * (equal to current_cc for an in-place reprogram of an already-
 * provisioned chip; the factory default for a fresh chip's first
 * programming, since the desired final control code is baked into the
 * image itself, not set separately). When they differ, this also
 * re-probes `target_cc`'s address afterward as a real end-to-end check
 * that the chip actually moved, not just that the I2C writes ACKed. */
static bool program_or_report(const char *label, uint8_t current_cc, uint8_t target_cc,
                               const uint8_t *image, uint32_t image_len) {
    if (image_len != GREENPAK_NVM_IMAGE_SIZE) {
        printf("  %s: image size mismatch (%u, expected %u) -- refusing\n",
               label, (unsigned)image_len, (unsigned)GREENPAK_NVM_IMAGE_SIZE);
        return false;
    }
    printf("  %s: %s image (chip currently at 0x%02X, target 0x%02X)...\n", label,
           GREENPAK_NVM_DRY_RUN ? "[DRY RUN] reading back (not writing)" : "programming",
           greenpak_nvm_reg_addr(current_cc), greenpak_nvm_reg_addr(target_cc));
    bool ok = greenpak_nvm_program_image(&g_bus, current_cc, image, GREENPAK_NVM_DRY_RUN);
    report_page(label, current_cc, ok);

    if (ok && !GREENPAK_NVM_DRY_RUN && current_cc != target_cc) {
        sleep_ms(50); /* allow real POR startup time after reset_and_reload -- not datasheet-confirmed exact, a conservative margin above the documented <=20ms self-timed cycles */
        bool moved = greenpak_nvm_probe(&g_bus, greenpak_nvm_reg_addr(target_cc));
        printf("  %s: post-program check at target 0x%02X: %s\n",
               label, greenpak_nvm_reg_addr(target_cc), moved ? "ACK (moved OK)" : "no ACK (FAILED to move!)");
        ok = ok && moved;
    }
    return ok;
}

static void recovery_scan(void) {
    printf("Recovery mode: scanning I2C addresses 0x08-0x77...\n");
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        if (greenpak_nvm_probe(&g_bus, addr)) {
            uint8_t control_code = addr >> 3;
            uint8_t block = addr & 0x7;
            printf("  0x%02X ACKs (control_code=0x%X, block=0b%d%d%d)\n",
                   addr, control_code,
                   (block >> 2) & 1, (block >> 1) & 1, block & 1);
        }
    }
    printf("Recovery scan complete.\n");

#if GREENPAK_NVM_ALLOW_RECOVERY_WRITE
    printf("This build allows recovery writes, but no automatic action is\n"
           "taken -- recovery reprogramming isn't implemented as an\n"
           "unattended step; re-run normal provisioning once you've\n"
           "identified which chip/address needs attention.\n");
#else
    printf("This build does not allow recovery writes (default). Rebuild\n"
           "with -DGREENPAK_NVM_ALLOW_RECOVERY_WRITE=1 if reprogramming a\n"
           "chip found here is really intended.\n");
#endif
}

int main(void) {
    stdio_init_all();
    wait_for_usb_terminal();

    g_bus.sda_gpio = PIN_GREENPAK1_SDA;
    g_bus.scl_gpio = PIN_GREENPAK1_SCL;
    greenpak_i2c_init(&g_bus);

    printf("GreenPAK NVM Provisioner -- dry_run=%d\n", GREENPAK_NVM_DRY_RUN);

    uint8_t default_addr = greenpak_nvm_reg_addr(GREENPAK_DEFAULT_CONTROL_CODE);
    uint8_t gp1_addr = greenpak_nvm_reg_addr(GP1_CONTROL_CODE);
    uint8_t gp2_addr = greenpak_nvm_reg_addr(GP2_CONTROL_CODE);
    bool default_present = greenpak_nvm_probe(&g_bus, default_addr);
    bool gp1_present = greenpak_nvm_probe(&g_bus, gp1_addr);
    bool gp2_present = greenpak_nvm_probe(&g_bus, gp2_addr);

    printf("Bus probe: default(0x%02X)=%s  GP1(0x%02X)=%s  GP2(0x%02X)=%s\n",
           default_addr, default_present ? "ACK" : "no",
           gp1_addr, gp1_present ? "ACK" : "no",
           gp2_addr, gp2_present ? "ACK" : "no");

    /* Check for already-provisioned chips FIRST (by their target
     * addresses), not the default address -- this is what lets a
     * re-run safely reprogram chips that are already done, and lets us
     * tell "nothing done yet" apart from "one done, one pending"
     * without relying on which jumper state happens to be in effect. */
    if (gp1_present && gp2_present) {
        printf("Both GP1 and GP2 already provisioned -- reprogramming both in place.\n");
        program_or_report("GP1", GP1_CONTROL_CODE, GP1_CONTROL_CODE, GP1_NVM_IMAGE, GP1_NVM_IMAGE_LEN);
#if GP2_NVM_IMAGE_AVAILABLE
        program_or_report("GP2", GP2_CONTROL_CODE, GP2_CONTROL_CODE, GP2_NVM_IMAGE, GP2_NVM_IMAGE_LEN);
#else
        printf("  GP2's NVM image isn't available yet -- skipping GP2 reprogram.\n");
#endif
    } else if (!gp1_present && !gp2_present) {
        if (default_present) {
            printf("Neither GP1 nor GP2 provisioned yet -- programming fresh chip as GP1.\n");
            program_or_report("GP1", GREENPAK_DEFAULT_CONTROL_CODE, GP1_CONTROL_CODE, GP1_NVM_IMAGE, GP1_NVM_IMAGE_LEN);
        } else {
            recovery_scan();
        }
    } else if (gp1_present && !gp2_present) {
        if (default_present) {
            printf("GP1 already provisioned; a second fresh chip is present -- programming it as GP2.\n");
#if GP2_NVM_IMAGE_AVAILABLE
            program_or_report("GP2", GREENPAK_DEFAULT_CONTROL_CODE, GP2_CONTROL_CODE, GP2_NVM_IMAGE, GP2_NVM_IMAGE_LEN);
#else
            printf("  GP2's NVM image isn't available yet -- can't program it.\n");
#endif
        } else {
            printf("GP1 already provisioned; no fresh chip found for GP2 -- reconnect its\n"
                   "isolation jumper and re-run.\n");
        }
    } else { /* !gp1_present && gp2_present -- not the documented workflow order, but handled symmetrically */
        if (default_present) {
            printf("GP2 already provisioned; a second fresh chip is present -- programming it as GP1.\n");
            program_or_report("GP1", GREENPAK_DEFAULT_CONTROL_CODE, GP1_CONTROL_CODE, GP1_NVM_IMAGE, GP1_NVM_IMAGE_LEN);
        } else {
            printf("GP2 already provisioned; no fresh chip found for GP1.\n");
        }
    }

    printf("Done.\n");
    while (true) {
        sleep_ms(1000);
    }
}
