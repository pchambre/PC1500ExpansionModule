/* sd_test_stubs.c -- what the SD-bridge test image (main_sd_test.c) needs
 * from monitor.c and mcu_log.c, so it links neither: monitor.c is the whole
 * bus loop, with the keyword executor, BLE and DORMANT sleep behind it, and
 * mcu_log.c keeps its log in flash (2026-10-03). */
#include <stdio.h>

#include "mcu_log.h"
#include "monitor.h"

/* set by greenpak_i2c.c on I2C traffic; the test image has no LED for it */
volatile bool g_i2c_activity_pending = false;

/* called by diskio_sd_bridge.c on a card swap; the test image keeps no open
 * files of monitor.c's to drop */
void monitor_sd_card_changed(void) {}

/* the driver's log lines go to USB serial instead of the flash log */
void mcu_log_error(const char *msg) { printf("[error] %s\n", msg); }
void mcu_log_info(const char *msg) { printf("[info] %s\n", msg); }
