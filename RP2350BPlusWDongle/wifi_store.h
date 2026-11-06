/* wifi_store.h -- the remembered Wi-Fi networks (2026-10-06, the WF*
 * keywords): EXP_WIFI_REMEMBERED SSIDs with their passwords, kept in their
 * own flash sector (flash_layout.h, mcu_store.c slot EXP_STORE_SLOT_WIFI).
 * Stored unencrypted, as the Link's keys are.
 *
 * core1 only (the WF* commands, wifi_link.c): a RAM copy, read at start-up,
 * changed by add/forget and saved by commit. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "pc_exp.h"

typedef struct {
    bool used;
    char ssid[EXP_WIFI_SSID_MAX + 1];
    char pw[EXP_WIFI_PW_MAX + 1]; /* "" = an open network */
} wifi_net_t;

void wifi_store_init(void);

/* The remembered network with exactly this SSID (SSIDs are case-sensitive),
 * or NULL. */
const wifi_net_t *wifi_store_find(const char *ssid);

/* Remembers (or updates) a network, the most recent first; when full, the
 * least recently connected one makes way. */
void wifi_store_add(const char *ssid, const char *pw);

/* Forgets the network with this SSID (any case), or every one for NULL.
 * Returns how many were forgotten. */
int wifi_store_forget(const char *ssid);

/* Saves the RAM copy if it changed. False if the flash write failed. */
bool wifi_store_commit(void);
