/* no_radio.c -- the internal card's stand-ins for the radio modules
 * (2026-10-09, PC1500_TARGET=card only).
 *
 * The card has no Wi-Fi, and its BLE module (a BGM220S) has no stack yet --
 * which one (BTstack over HCI with a BlueKitchen licence, Silicon Labs'
 * NCP, or NimBLE) is still open. Until then nothing radio-related is linked
 * at all, BTstack included, and these keep the rest of the firmware's calls
 * unchanged: every command answers ERROR with code 0 ("no link"/"no
 * radio") in the window, nothing is ever wanted, so nothing keeps the MCU
 * awake. keywords.c turns the Wi-Fi and SSH keywords away before they get
 * here (PC1500_NO_WIFI); BL*, BLKBD and the CE-150 stand-in raise the same
 * errors they do on the dongle with no link. */
#include <stdbool.h>
#include <stdint.h>

#include "pc_exp.h"
#include "wifi_link.h"
#include "ssh_session.h"
#include "net_ping.h"
#include "net_time.h"
#include "ble_link.h"
#include "kbd_host.h"
#include "bt_store.h"

static uint8_t no_radio(uint8_t *window) {
    window[0] = 0; /* EXP_WIFI_ERR_FAILED, EXP_SSH_ERR_NONE, BLE's "no link" */
    return EXP_STATUS_ERROR;
}

/* ---- Wi-Fi, SSH, WFPING, SNTP ---- */
uint8_t wifi_link_command(uint8_t command, uint8_t *window) { (void)command; return no_radio(window); }
bool wifi_link_connected(void) { return false; }
bool wifi_link_wanted(void) { return false; }
void wifi_link_poll(bool radio_up) { (void)radio_up; }

uint8_t ssh_session_command(uint8_t command, uint8_t *window) { (void)command; return no_radio(window); }
bool ssh_session_terminal(void) { return false; }
void ssh_session_poll(uint8_t *window) { (void)window; }

uint8_t net_ping_command(uint8_t command, uint8_t *window) { (void)command; return no_radio(window); }

bool net_time_get(int64_t *utc_ms) { (void)utc_ms; return false; }

/* ---- BLE: the Link, the keyboard, the bonds ---- */
uint8_t ble_link_command(uint8_t command, uint8_t *window) { (void)command; return no_radio(window); }
bool ble_link_transfer_open(void) { return false; }
bool ble_link_wanted(void) { return false; }
void ble_link_poll(bool radio_up) { (void)radio_up; }
bool ble_link_stack_acquire(void) { return false; }
void ble_link_stack_release(void) {}
bool ble_link_host_time(int64_t *utc_ms, int32_t *offset_s) { (void)utc_ms; (void)offset_s; return false; }
bool ble_link_le_busy(void) { return false; }

bool kbd_host_wanted(void) { return false; }
void kbd_host_stack_init(void) {}
void kbd_host_publish(uint8_t *window) { (void)window; }
uint8_t kbd_host_command(uint8_t command, uint8_t *window) { (void)command; return no_radio(window); }
void kbd_host_le_yield(void) {}
void kbd_host_trace_to_log(void) {}

void bt_store_install(void) {}
bool bt_store_commit(void) { return true; }
