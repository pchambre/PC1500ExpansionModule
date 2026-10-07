/* ssh_store.h -- SSH's flash store (2026-10-07, ssh_session.h): the
 * dongle's own ed25519 key, made once from the TRNG, and the known hosts
 * (trust on first use: a host's key is kept the first time the user
 * accepts it, and a different one later is refused).
 *
 * RAM copy, read from flash on first use; ssh_store_commit() writes it back
 * (mcu_store.c slot EXP_STORE_SLOT_SSH). core1 only: every caller is a
 * command. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SSH_STORE_HOSTS 16
#define SSH_STORE_HOST_MAX 40

typedef enum {
    SSH_HOST_UNKNOWN, /* never seen: ask */
    SSH_HOST_KNOWN,   /* the key kept for it */
    SSH_HOST_CHANGED, /* a different key: refuse (SSH FORGET first) */
} ssh_host_check_t;

/* The device key: the 64-byte secret (seed, then public) and the public
 * half. Made and saved the first time. False if it couldn't be saved. */
bool ssh_store_device_key(uint8_t secret[64], uint8_t public_key[32]);

ssh_host_check_t ssh_store_check_host(const char *host, uint16_t port, const uint8_t key[32]);
void ssh_store_add_host(const char *host, uint16_t port, const uint8_t key[32]);
/* Forgets a host (any port), or every one with NULL; returns how many. */
int ssh_store_forget(const char *host);

bool ssh_store_commit(void);
