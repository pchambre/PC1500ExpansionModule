/* sha256.h -- SHA-256 (FIPS 180-4), for SSH (2026-10-07, ssh_client.h):
 * the exchange hash, key derivation and host key fingerprints. Portable C:
 * pc1500emu's expansion mock compiles it too. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define SHA256_LEN 32

typedef struct {
    uint32_t h[8];
    uint64_t total; /* bytes hashed so far */
    uint8_t block[64];
    uint8_t used; /* bytes waiting in block */
} sha256_t;

void sha256_init(sha256_t *s);
void sha256_update(sha256_t *s, const void *data, size_t len);
void sha256_final(sha256_t *s, uint8_t out[SHA256_LEN]);
void sha256(const void *data, size_t len, uint8_t out[SHA256_LEN]);
