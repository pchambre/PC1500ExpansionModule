/* Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
 * Version 2.0 -- see LICENSE.
 */
/* bt_store.h -- BTstack's key store, kept in RAM and saved by core1
 * (2026-10-04, for the external keyboard's bonds).
 *
 * BTstack keeps classic link keys and BLE bonds in a btstack_tlv_t store.
 * The SDK's (pico_btstack_flash_bank) writes flash itself, from BTstack's
 * core -- core0 here, which can't (flash_safe_execute() only pauses core0:
 * main.c). This one keeps the records in RAM, where BTstack reads and
 * writes them, and core1 saves them to their own flash sector
 * (flash_layout.h FLASH_BTBONDS_*, mcu_store.c slot 3), the way link_store.c
 * saves the Link's pairings.
 *
 * Threading: bt_store_install() and BTstack's use of the store are core0's
 * (BTstack's async context); bt_store_commit() is core1's. */
#pragma once

#include <stdbool.h>

/* core0, after BTstack is initialised and before it's powered on: loads
 * the saved records and points BTstack's TLV, classic link-key database and
 * BLE device database at this store. */
void bt_store_install(void);

/* core1: saves the records to flash if they've changed since the last
 * save. False if a flash write failed. */
bool bt_store_commit(void);
