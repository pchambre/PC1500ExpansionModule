/* module_link.h -- the internal card's side of MODULE_PROTOCOL.md: the
 * UART to the BGM220S and its two wake lines (2026-10-13). For now a
 * bring-up probe; the Link transport (ble_link.c's on the card) comes next.
 * Card build only (PC1500_TARGET_CARD). core1 only. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void module_link_init(void);

/* One request, its answer's payload (status byte first) into `ans`;
 * returns the payload length, or -1 with no valid answer after 3 tries
 * (sec.5). Opens a session around it (sec.4). */
int module_link_request(uint8_t type, const uint8_t *payload, size_t len, uint8_t *ans, size_t max);

/* HOST_WAKE: the module has events queued (sec.4). */
bool module_link_host_wake(void);

/* MODULE_WAKE low: no session, the module may sleep. A request opens one
 * again. */
void module_link_session_end(void);

/* A USB console key (bring-up; ble_link_module_console() reads them):
 * 'm' probes the module -- HELLO, then POLL until empty -- and prints what
 * it got. */
void module_link_console_key(int c);
