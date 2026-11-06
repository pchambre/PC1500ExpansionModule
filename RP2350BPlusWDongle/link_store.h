/* link_store.h -- this device's Link identity and its pairings (2026-10-03,
 * BLE_PROTOCOL.md sec.7), kept in flash (mcu_store.c slot 2) with a copy
 * in RAM.
 *
 * Threading: the RAM copy is read by core0 (a HELLO, an AUTH) and changed
 * only on core0's context too (ble_link.c runs link_store_add/forget's RAM
 * part there); the flash writes are core1's (link_store_commit), since a
 * flash write pauses core0. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "link_secure.h"

#define LINK_PAIRS_MAX 8
#define LINK_NAME_MAX 16

typedef struct {
    bool used;
    uint8_t id[LS_ID_LEN];
    char name[LINK_NAME_MAX + 1];
    uint8_t ltk[LS_KEY_LEN];
} link_pair_t;

/* Reads flash into RAM; makes and saves an identity the first time. core1,
 * before the radio's first use. */
void link_store_init(void);

const uint8_t *link_store_id(void);

/* The pairing with this peer, or NULL. */
const link_pair_t *link_store_find(const uint8_t id[LS_ID_LEN]);

/* Adds (or replaces) a pairing in RAM; link_store_commit() saves it. */
void link_store_add(const uint8_t id[LS_ID_LEN], const char *name, const uint8_t ltk[LS_KEY_LEN]);

/* Forgets the pairings whose name matches (any case), or all with NULL, in
 * RAM; returns how many. */
int link_store_forget(const char *name);

/* Saves the RAM copy to flash if it changed. core1. */
bool link_store_commit(void);
