/* ssh_client.h -- an SSH-2 client for one interactive shell (2026-10-07).
 *
 * The protocol only: the caller moves the bytes. It feeds what arrives from
 * the TCP connection to ssh_feed() and sends what the io's send() is given;
 * the shell's output comes out through the io's data(). Portable C, no
 * allocation: the firmware runs it on core1 (ssh_session.c), and
 * tools/ssh_host_test.c runs it against a real sshd from a PC.
 *
 * One algorithm each, OpenSSH's defaults since 6.5/8.x:
 * - key exchange curve25519-sha256 (RFC 8731), with OpenSSH's strict KEX
 *   (kex-strict-c-v00@openssh.com, the Terrapin fix);
 * - host key ssh-ed25519 (RFC 8709);
 * - cipher chacha20-poly1305@openssh.com (OpenSSH's PROTOCOL.chacha20poly1305);
 * - login: publickey with an ed25519 key, then password.
 * No compression. A server that can't do these fails with SSH_ERR_ALGORITHMS.
 *
 * Two pauses need the caller's answer before it goes on:
 * - SSH_ST_HOSTKEY: the server's host key (ssh_hostkey_blob(), and
 *   ssh_fingerprint() for showing it) wants ssh_hostkey_answer(): trust on
 *   first use, a known key's check, are the caller's.
 * - SSH_ST_PASSWORD: the server takes a password and none was given (or the
 *   key wasn't accepted): ssh_password().
 * Input that arrives meanwhile waits in the receive buffer. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sha256.h"

#define SSH_COLS 80 /* the pty's size: lines up to 80 characters (ssh_term.h) */
#define SSH_ROWS 24
#define SSH_PACKET_MAX 8192 /* the biggest packet received; the channel's max packet is less */
#define SSH_USER_MAX 32
#define SSH_PASSWORD_MAX 64
#define SSH_HOSTKEY_BLOB_LEN 51 /* string "ssh-ed25519", string key(32) */
#define SSH_FINGERPRINT_LEN 50  /* "SHA256:" + 43 base64 characters */

typedef enum {
    SSH_ST_VERSION,  /* exchanging version lines */
    SSH_ST_KEX,      /* key exchange */
    SSH_ST_HOSTKEY,  /* paused: ssh_hostkey_answer() */
    SSH_ST_AUTH,     /* logging in */
    SSH_ST_PASSWORD, /* paused: ssh_password() */
    SSH_ST_CHANNEL,  /* opening the shell */
    SSH_ST_OPEN,     /* the shell is running: ssh_write() */
    SSH_ST_CLOSED,   /* the shell ended (or the server disconnected): ssh_error() says which */
} ssh_state_t;

typedef enum {
    SSH_ERR_NONE = 0,     /* closed normally: the shell exited */
    SSH_ERR_IO,           /* send() failed, or the caller's connection went */
    SSH_ERR_PROTOCOL,     /* a malformed or unexpected message */
    SSH_ERR_ALGORITHMS,   /* the server can't do ours */
    SSH_ERR_MAC,          /* a packet failed its check: tampered or garbled */
    SSH_ERR_HOSTKEY,      /* the host key's signature was bad, or it changed on a rekey */
    SSH_ERR_REJECTED,     /* the caller refused the host key */
    SSH_ERR_AUTH,         /* no login method worked */
    SSH_ERR_CHANNEL,      /* the server refused the session, pty or shell */
    SSH_ERR_DISCONNECTED, /* the server sent DISCONNECT */
} ssh_error_t;

typedef struct {
    void *ctx;
    /* Sends all `len` bytes (or queues them); false ends the session. */
    bool (*send)(void *ctx, const uint8_t *data, size_t len);
    void (*random)(void *ctx, uint8_t *out, size_t len);
    /* The shell's output (stdout and stderr) and any login banner. */
    void (*data)(void *ctx, const uint8_t *data, size_t len);
} ssh_io_t;

typedef struct {
    uint8_t key[64]; /* chacha20-poly1305@openssh.com: K_2 (main) then K_1 (length) */
    uint32_t seq;
    bool on;
} ssh_dir_t;

typedef struct {
    ssh_io_t io;
    ssh_state_t state;
    ssh_error_t error;
    uint32_t disconnect_reason; /* SSH_ERR_DISCONNECTED's */

    char user[SSH_USER_MAX + 1];
    char password[SSH_PASSWORD_MAX + 1];
    uint8_t key[64]; /* ed25519 secret key (seed then public), if have_key */
    bool have_key, tried_key, tried_password;
    uint8_t pw_tries; /* passwords asked for (SSH_ST_PASSWORD) */

    /* key exchange */
    char v_s[256];         /* the server's version line, for the exchange hash */
    uint8_t i_c[512];      /* our KEXINIT payload, likewise */
    uint16_t i_c_len;
    uint8_t x_secret[32];  /* our ephemeral X25519 key */
    uint8_t hostkey[SSH_HOSTKEY_BLOB_LEN];
    uint8_t first_hostkey[SSH_HOSTKEY_BLOB_LEN];
    uint8_t session_id[32];
    uint8_t next_tx[64], next_rx[64]; /* the keys NEWKEYS switches to */
    bool kex_sent;      /* our KEXINIT is out, this round */
    bool kex_got;       /* theirs is in */
    bool kex_done_once; /* session_id is set */
    bool strict;        /* the server does strict KEX */
    bool in_kex;        /* a round is under way: nothing else may be sent */
    bool newkeys_sent, newkeys_got;
    sha256_t hash;     /* the exchange hash, begun at their KEXINIT */

    ssh_dir_t tx, rx;

    /* the session channel */
    uint32_t remote_id, remote_window, remote_max;
    uint32_t local_window; /* what the server may still send */
    uint8_t channel_step;  /* 0 open sent, 1 pty-req sent, 2 shell sent, 3 running */
    bool close_sent;

    /* receiving */
    uint8_t rx_buf[4 + SSH_PACKET_MAX + 16];
    uint16_t rx_len;

    uint8_t tx_buf[4 + 1 + 1024 + 255 + 16]; /* our packets: never more than this */
} ssh_t;

/* Starts a session: sends our version line. `key` is the ed25519 secret key
 * (64 bytes, as crypto_ed25519_key_pair() makes it) or NULL; `password` may
 * be NULL or "". */
void ssh_start(ssh_t *s, const ssh_io_t *io, const char *user, const uint8_t *key, const char *password);

/* Bytes received from the server. Returns how many it took: while paused
 * with a full buffer, fewer -- the rest waits with the caller (and the TCP
 * window stays shut). All of them once closed. */
size_t ssh_feed(ssh_t *s, const uint8_t *data, size_t len);

/* The caller's answer in SSH_ST_HOSTKEY. */
void ssh_hostkey_answer(ssh_t *s, bool accept);
const uint8_t *ssh_hostkey_blob(const ssh_t *s);
/* "SHA256:..." as OpenSSH shows it, NUL-terminated, for a host key blob. */
void ssh_fingerprint(const uint8_t blob[SSH_HOSTKEY_BLOB_LEN], char out[SSH_FINGERPRINT_LEN + 1]);

/* The password, in SSH_ST_PASSWORD. */
void ssh_password(ssh_t *s, const char *password);

/* Keys typed: sends up to `len` bytes to the shell, as far as the server's
 * window allows. Returns how many went. */
size_t ssh_write(ssh_t *s, const uint8_t *data, size_t len);

/* Ends the session from our side (BREAK out of it, or the connection went:
 * `io_lost`, nothing more is sent). */
void ssh_close(ssh_t *s, bool io_lost);

static inline ssh_state_t ssh_state(const ssh_t *s) { return s->state; }
static inline ssh_error_t ssh_error(const ssh_t *s) { return s->error; }

/* Base64 (RFC 4648, with padding if `pad`), NUL-terminated; returns its
 * length. For the fingerprint, and the public key's line (SSHKEY). */
size_t ssh_base64(const uint8_t *data, size_t len, char *out, bool pad);

/* "ssh-ed25519 AAAA..." for a public key, as authorized_keys takes it
 * (without a comment); returns its length. `out` needs 81 bytes. */
size_t ssh_public_key_line(const uint8_t public_key[32], char *out);
