/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* main_sd_test.c -- standalone bring-up/test firmware for the SD card
 * behind U5 (SC18IS602B I2C-to-SPI bridge), completely independent of
 * the PC-1500 bus loop. Separate build/flash from
 * pc1500_expansion_rp2350 -- built specifically so the SD driver can be
 * tested over USB serial with the dongle sitting on the bench, not
 * plugged into the PC-1500 at all, after SDLS (and, separately, ECVER --
 * pure ROM dispatch, no I2C involved at all) both stopped responding on
 * real hardware, suggesting the whole single-core firmware hangs
 * somewhere in the new SD path rather than the SD command failing
 * cleanly.
 *
 * Reports every step (I2C bus init, disk_initialize(), card capacity,
 * a directory listing, a one-block raw read) over USB serial via
 * printf, then loops re-announcing the final result -- same
 * reconnect-friendly pattern as GreenPak_Provision's main_provision.c
 * (missing the boot-time output is easy to do while switching to a
 * terminal after a flash-triggered reboot).
 */
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/stdio_usb.h"

#include "ff.h"
#include "diskio.h"

static void wait_for_usb_terminal(void) {
    for (int waited_ms = 0; !stdio_usb_connected() && waited_ms < 10000; waited_ms += 100) {
        sleep_ms(100);
    }
    sleep_ms(200);
}

static FATFS g_fatfs;

int main(void) {
    stdio_init_all();
    setvbuf(stdout, NULL, _IONBF, 0);
    wait_for_usb_terminal();

    printf("\n=== SD-over-I2C-bridge (SC18IS602B/U5) standalone test ===\n");

    const char *result_summary = "did not reach a conclusion";

    DSTATUS ds = disk_initialize(0);
    if (ds & STA_NOINIT) {
        printf("disk_initialize(): FAILED (status=0x%02X) -- no card detected, or the\n"
               "  bridge/card didn't respond as expected. Check: card seated, U5's\n"
               "  SDA/SCL actually reach the Pico (GP26/GP27), U5's I2C address\n"
               "  strapping (A0/A1/A2 -> GND -> 0x28).\n", ds);
        result_summary = "disk_initialize() FAILED";
    } else {
        printf("disk_initialize(): OK\n");

        DWORD sector_count = 0;
        if (disk_ioctl(0, GET_SECTOR_COUNT, &sector_count) == RES_OK) {
            printf("Card capacity: %lu sectors (%.1f MB)\n", (unsigned long)sector_count,
                   (double)sector_count * 512.0 / (1024.0 * 1024.0));
        } else {
            printf("GET_SECTOR_COUNT failed\n");
        }

        uint8_t block[512];
        DRESULT dr = disk_read(0, block, 0, 1);
        if (dr == RES_OK) {
            printf("disk_read(sector 0): OK -- first 16 bytes:");
            for (int i = 0; i < 16; i++) printf(" %02X", block[i]);
            printf("\n");
        } else {
            printf("disk_read(sector 0): FAILED (result=%d)\n", dr);
        }

        FRESULT fr = f_mount(&g_fatfs, "", 1);
        if (fr != FR_OK) {
            printf("f_mount(): FAILED (FRESULT=%d) -- disk_read() worked above, so the\n"
                   "  bridge/card link is fine; this points at the filesystem itself\n"
                   "  (unformatted, or a filesystem type FatFs doesn't recognize).\n", fr);
            result_summary = "disk_read() OK, f_mount() FAILED";
        } else {
            printf("f_mount(): OK -- directory listing of /:\n");
            DIR dir;
            FILINFO fno;
            int count = 0;
            if (f_opendir(&dir, "/") == FR_OK) {
                while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != 0) {
                    printf("  %s%s  %lu bytes\n", fno.fname, (fno.fattrib & AM_DIR) ? "/" : "",
                           (unsigned long)fno.fsize);
                    count++;
                }
                f_closedir(&dir);
                printf("(%d entries)\n", count);
                result_summary = "f_mount() OK, directory listing OK";

                /* Write/read-back test -- exercises disk_write() (CMD24),
                 * whose multi-transfer CS framing (separate token/payload/
                 * CRC/busy-poll calls, since a 512-byte block can't fit in
                 * the SC18IS602B's 200-byte buffer as one transfer, unlike
                 * CMD9's CSD read) hadn't been confirmed against real
                 * hardware yet -- see diskio_sd_bridge.c's own top comment.
                 * Uses a throwaway filename and cleans up after itself so
                 * this test doesn't leave clutter on the card. */
                static const char testPath[] = "CLAUDEWR.TST";
                uint8_t writeBuf[600]; /* >512 bytes so this spans a full block plus a remainder */
                for (unsigned i = 0; i < sizeof(writeBuf); i++) writeBuf[i] = (uint8_t)(i * 37 + 11);

                FIL wf;
                FRESULT wfr = f_open(&wf, testPath, FA_CREATE_ALWAYS | FA_WRITE);
                if (wfr != FR_OK) {
                    printf("write-test: f_open(FA_CREATE_ALWAYS|FA_WRITE) FAILED (FRESULT=%d)\n", wfr);
                    result_summary = "directory listing OK, write-test f_open FAILED";
                } else {
                    UINT written = 0;
                    wfr = f_write(&wf, writeBuf, sizeof(writeBuf), &written);
                    f_close(&wf);
                    if (wfr != FR_OK || written != sizeof(writeBuf)) {
                        printf("write-test: f_write() FAILED (FRESULT=%d, wrote %u/%u bytes)\n",
                               wfr, (unsigned)written, (unsigned)sizeof(writeBuf));
                        result_summary = "directory listing OK, write-test f_write FAILED";
                    } else {
                        printf("write-test: wrote %u bytes to %s, reading back...\n",
                               (unsigned)written, testPath);
                        FIL rf;
                        FRESULT rfr = f_open(&rf, testPath, FA_READ);
                        if (rfr != FR_OK) {
                            printf("write-test: read-back f_open() FAILED (FRESULT=%d)\n", rfr);
                            result_summary = "write-test wrote OK, read-back f_open FAILED";
                        } else {
                            uint8_t readBuf[sizeof(writeBuf)];
                            UINT got = 0;
                            rfr = f_read(&rf, readBuf, sizeof(readBuf), &got);
                            f_close(&rf);
                            if (rfr != FR_OK || got != sizeof(readBuf)) {
                                printf("write-test: read-back f_read() FAILED (FRESULT=%d, got %u/%u bytes)\n",
                                       rfr, (unsigned)got, (unsigned)sizeof(readBuf));
                                result_summary = "write-test wrote OK, read-back f_read FAILED";
                            } else if (memcmp(writeBuf, readBuf, sizeof(writeBuf)) != 0) {
                                printf("write-test: read-back data MISMATCH -- disk_write()/disk_read()\n"
                                       "  round-trip is NOT reliable on this bridge (see disk_write()'s\n"
                                       "  multi-transfer CS-framing caveat in diskio_sd_bridge.c)\n");
                                result_summary = "write-test round-trip data MISMATCH";
                            } else {
                                printf("write-test: read-back matches written data byte-for-byte -- PASS\n");
                                result_summary = "write/read-back round-trip PASS";
                            }
                        }
                    }
                    f_unlink(testPath); /* clean up regardless of outcome above */
                }
            } else {
                printf("f_opendir(\"/\") FAILED\n");
                result_summary = "f_mount() OK, f_opendir() FAILED";
            }
        }
    }

    printf("\nDone: %s\n", result_summary);
    while (true) {
        printf("\n[idle] SD bridge test -- result: %s (reconnect the serial monitor any time)\n",
               result_summary);
        sleep_ms(5000);
    }
}
