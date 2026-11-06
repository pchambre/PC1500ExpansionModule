/* ssh_client.c -- see ssh_client.h. Message numbers and layouts: RFC 4253
 * (transport), 4252 (userauth), 4254 (connection); the cipher and strict
 * KEX: OpenSSH's PROTOCOL.chacha20poly1305 and PROTOCOL. */
#include "ssh_client.h"

#include <string.h>

#include "third_party/monocypher/monocypher-ed25519.h"
#include "third_party/monocypher/monocypher.h"

enum {
    MSG_DISCONNECT = 1,
    MSG_IGNORE = 2,
    MSG_UNIMPLEMENTED = 3,
    MSG_DEBUG = 4,
    MSG_SERVICE_REQUEST = 5,
    MSG_SERVICE_ACCEPT = 6,
    MSG_EXT_INFO = 7,
    MSG_KEXINIT = 20,
    MSG_NEWKEYS = 21,
    MSG_KEX_ECDH_INIT = 30,
    MSG_KEX_ECDH_REPLY = 31,
    MSG_USERAUTH_REQUEST = 50,
    MSG_USERAUTH_FAILURE = 51,
    MSG_USERAUTH_SUCCESS = 52,
    MSG_USERAUTH_BANNER = 53,
    MSG_USERAUTH_60 = 60, /* PK_OK or PASSWD_CHANGEREQ: neither is expected */
    MSG_GLOBAL_REQUEST = 80,
    MSG_REQUEST_SUCCESS = 81,
    MSG_REQUEST_FAILURE = 82,
    MSG_CHANNEL_OPEN = 90,
    MSG_CHANNEL_OPEN_CONFIRMATION = 91,
    MSG_CHANNEL_OPEN_FAILURE = 92,
    MSG_CHANNEL_WINDOW_ADJUST = 93,
    MSG_CHANNEL_DATA = 94,
    MSG_CHANNEL_EXTENDED_DATA = 95,
    MSG_CHANNEL_EOF = 96,
    MSG_CHANNEL_CLOSE = 97,
    MSG_CHANNEL_REQUEST = 98,
    MSG_CHANNEL_SUCCESS = 99,
    MSG_CHANNEL_FAILURE = 100,
};

/* DISCONNECT reasons (RFC 4253 sec.11.1) */
enum { DC_PROTOCOL_ERROR = 2, DC_KEY_EXCHANGE_FAILED = 3, DC_MAC_ERROR = 5, DC_HOST_KEY_NOT_VERIFIABLE = 9,
       DC_BY_APPLICATION = 11, DC_NO_MORE_AUTH_METHODS = 14 };

#define VERSION "SSH-2.0-PC1500_1.0"
#define KEX_ALGS "curve25519-sha256,curve25519-sha256@libssh.org,kex-strict-c-v00@openssh.com"
#define HOSTKEY_ALG "ssh-ed25519"
#define CIPHER "chacha20-poly1305@openssh.com"
#define LOCAL_WINDOW 16384u
#define LOCAL_MAX_PACKET 4096u /* our channel's: well inside SSH_PACKET_MAX */
#define WRITE_CHUNK 512u
#define MAC_LEN 16
#define PASSWORD_TRIES 3

/* ---- reading a payload ---- */

typedef struct {
    const uint8_t *p;
    size_t n;
    bool bad;
} reader_t;

static uint8_t get_u8(reader_t *r) {
    if (r->n < 1) {
        r->bad = true;
        return 0;
    }
    r->n--;
    return *r->p++;
}

static uint32_t get_u32(reader_t *r) {
    uint32_t v;
    if (r->n < 4) {
        r->bad = true;
        r->n = 0;
        return 0;
    }
    v = (uint32_t)r->p[0] << 24 | (uint32_t)r->p[1] << 16 | (uint32_t)r->p[2] << 8 | r->p[3];
    r->p += 4;
    r->n -= 4;
    return v;
}

/* A string: its bytes stay in the payload. */
static const uint8_t *get_string(reader_t *r, uint32_t *len) {
    const uint8_t *s;
    *len = get_u32(r);
    if (r->bad || *len > r->n) {
        r->bad = true;
        r->n = 0;
        *len = 0;
        return (const uint8_t *)"";
    }
    s = r->p;
    r->p += *len;
    r->n -= *len;
    return s;
}

static bool string_is(const uint8_t *s, uint32_t len, const char *want) {
    return len == strlen(want) && memcmp(s, want, len) == 0;
}

/* Is `name` in the comma-separated name-list? */
static bool list_has(const uint8_t *list, uint32_t len, const char *name) {
    size_t n = strlen(name);
    uint32_t start = 0;
    for (uint32_t i = 0; i <= len; i++) {
        if (i == len || list[i] == ',') {
            if (i - start == n && memcmp(list + start, name, n) == 0) return true;
            start = i + 1;
        }
    }
    return false;
}

/* ---- writing a payload, into tx_buf after the packet's 5-byte header ---- */

typedef struct {
    uint8_t *p;
    size_t n, cap;
    bool bad;
} writer_t;

static writer_t begin(ssh_t *s, uint8_t type) {
    writer_t w = {s->tx_buf + 5, 0, sizeof s->tx_buf - 5 - 255 - MAC_LEN, false};
    w.p[w.n++] = type;
    return w;
}

static void put_bytes(writer_t *w, const void *data, size_t len) {
    if (w->n + len > w->cap) {
        w->bad = true;
        return;
    }
    memcpy(w->p + w->n, data, len);
    w->n += len;
}

static void put_u8(writer_t *w, uint8_t v) { put_bytes(w, &v, 1); }

static void put_u32(writer_t *w, uint32_t v) {
    uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    put_bytes(w, b, 4);
}

static void put_string(writer_t *w, const void *data, size_t len) {
    put_u32(w, (uint32_t)len);
    put_bytes(w, data, len);
}

static void put_cstring(writer_t *w, const char *s) { put_string(w, s, strlen(s)); }

static void store_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* ---- the cipher (PROTOCOL.chacha20poly1305) ---- */

static void nonce_for(uint32_t seq, uint8_t nonce[8]) {
    memset(nonce, 0, 4);
    store_u32(nonce + 4, seq);
}

static void poly_key(const ssh_dir_t *d, const uint8_t nonce[8], uint8_t out[32]) {
    crypto_chacha20_djb(out, NULL, 32, d->key, nonce, 0);
}

/* ---- failing ---- */

static void wipe_secrets(ssh_t *s) {
    crypto_wipe(s->password, sizeof s->password);
    crypto_wipe(s->key, sizeof s->key);
    crypto_wipe(s->x_secret, sizeof s->x_secret);
    crypto_wipe(s->next_tx, sizeof s->next_tx);
    crypto_wipe(s->next_rx, sizeof s->next_rx);
    crypto_wipe(&s->tx, sizeof s->tx);
    crypto_wipe(&s->rx, sizeof s->rx);
}

static bool send_packet(ssh_t *s, const writer_t *w);

static void send_disconnect(ssh_t *s, uint32_t reason) {
    writer_t w = begin(s, MSG_DISCONNECT);
    put_u32(&w, reason);
    put_cstring(&w, "");
    put_cstring(&w, "");
    send_packet(s, &w);
}

/* Ends the session with `err`; tells the server why unless the
 * connection itself is what went. */
static void fail(ssh_t *s, ssh_error_t err, uint32_t reason) {
    if (s->state == SSH_ST_CLOSED) return;
    s->error = err;
    if (err != SSH_ERR_IO && err != SSH_ERR_DISCONNECTED && s->state != SSH_ST_VERSION) send_disconnect(s, reason);
    s->state = SSH_ST_CLOSED;
    wipe_secrets(s);
}

/* ---- sending a packet ---- */

/* Pads, seals (once keys are on) and sends the payload `w` built. */
static bool send_packet(ssh_t *s, const writer_t *w) {
    uint8_t *p = s->tx_buf, nonce[8], pk[32];
    size_t body = 1 + w->n, total;
    /* the length field isn't counted once it's sealed on its own */
    size_t align = s->tx.on ? body : 4 + body;
    uint8_t pad = (uint8_t)(8 - align % 8);
    if (pad < 4) pad += 8;
    if (w->bad) {
        s->error = SSH_ERR_PROTOCOL;
        s->state = SSH_ST_CLOSED;
        return false;
    }
    store_u32(p, (uint32_t)(body + pad));
    p[4] = pad;
    s->io.random(s->io.ctx, p + 5 + w->n, pad);
    total = 4 + body + pad;
    if (s->tx.on) {
        nonce_for(s->tx.seq, nonce);
        crypto_chacha20_djb(p, p, 4, s->tx.key + 32, nonce, 0);
        poly_key(&s->tx, nonce, pk);
        crypto_chacha20_djb(p + 4, p + 4, total - 4, s->tx.key, nonce, 1);
        crypto_poly1305(p + total, p, total, pk);
        crypto_wipe(pk, sizeof pk);
        total += MAC_LEN;
    }
    s->tx.seq++;
    if (!s->io.send(s->io.ctx, p, total)) {
        s->error = SSH_ERR_IO;
        s->state = SSH_ST_CLOSED;
        wipe_secrets(s);
        return false;
    }
    return true;
}

/* ---- key exchange ---- */

static void hash_string(ssh_t *s, const void *data, size_t len) {
    uint8_t b[4];
    store_u32(b, (uint32_t)len);
    sha256_update(&s->hash, b, 4);
    sha256_update(&s->hash, data, len);
}

/* The shared secret as an mpint (RFC 8731 sec.3.1: its 32 bytes read as a
 * big-endian number); returns the encoding's length. */
static size_t mpint(const uint8_t k[32], uint8_t out[4 + 33]) {
    size_t i = 0, n;
    while (i < 32 && k[i] == 0) i++;
    n = 32 - i;
    if (n && (k[i] & 0x80)) {
        store_u32(out, (uint32_t)n + 1);
        out[4] = 0;
        memcpy(out + 5, k + i, n);
        return 5 + n;
    }
    store_u32(out, (uint32_t)n);
    memcpy(out + 4, k + i, n);
    return 4 + n;
}

/* A 64-byte key for `letter` (RFC 4253 sec.7.2), from K (as an mpint) and H. */
static void derive(const ssh_t *s, const uint8_t *k, size_t k_len, const uint8_t h[32], char letter,
                   uint8_t out[64]) {
    sha256_t d;
    sha256_init(&d);
    sha256_update(&d, k, k_len);
    sha256_update(&d, h, 32);
    sha256_update(&d, &letter, 1);
    sha256_update(&d, s->session_id, 32);
    sha256_final(&d, out);
    sha256_init(&d);
    sha256_update(&d, k, k_len);
    sha256_update(&d, h, 32);
    sha256_update(&d, out, 32);
    sha256_final(&d, out + 32);
}

static bool send_kexinit(ssh_t *s) {
    writer_t w = begin(s, MSG_KEXINIT);
    uint8_t cookie[16];
    s->io.random(s->io.ctx, cookie, sizeof cookie);
    put_bytes(&w, cookie, sizeof cookie);
    put_cstring(&w, KEX_ALGS);
    put_cstring(&w, HOSTKEY_ALG);
    put_cstring(&w, CIPHER);
    put_cstring(&w, CIPHER);
    put_cstring(&w, "hmac-sha2-256"); /* unused: the cipher has its own MAC */
    put_cstring(&w, "hmac-sha2-256");
    put_cstring(&w, "none");
    put_cstring(&w, "none");
    put_cstring(&w, "");
    put_cstring(&w, "");
    put_u8(&w, 0); /* first_kex_packet_follows */
    put_u32(&w, 0);
    if (w.n > sizeof s->i_c) {
        fail(s, SSH_ERR_PROTOCOL, DC_PROTOCOL_ERROR);
        return false;
    }
    memcpy(s->i_c, w.p, w.n);
    s->i_c_len = (uint16_t)w.n;
    s->kex_sent = true;
    return send_packet(s, &w);
}

static void on_kexinit(ssh_t *s, const uint8_t *payload, size_t len) {
    reader_t r = {payload + 1, len - 1, false};
    const uint8_t *lists[10];
    uint32_t lens[10];
    uint8_t q_c[32];
    writer_t w;
    if (s->kex_got) { fail(s, SSH_ERR_PROTOCOL, DC_PROTOCOL_ERROR); return; }
    if (r.n < 16) { fail(s, SSH_ERR_PROTOCOL, DC_PROTOCOL_ERROR); return; }
    r.p += 16, r.n -= 16; /* cookie */
    for (int i = 0; i < 10; i++) lists[i] = get_string(&r, &lens[i]);
    if (r.bad) { fail(s, SSH_ERR_PROTOCOL, DC_PROTOCOL_ERROR); return; }
    if (!(list_has(lists[0], lens[0], "curve25519-sha256") || list_has(lists[0], lens[0], "curve25519-sha256@libssh.org")) ||
        !list_has(lists[1], lens[1], HOSTKEY_ALG) || !list_has(lists[2], lens[2], CIPHER) ||
        !list_has(lists[3], lens[3], CIPHER) || !list_has(lists[6], lens[6], "none") ||
        !list_has(lists[7], lens[7], "none"))
        { fail(s, SSH_ERR_ALGORITHMS, DC_KEY_EXCHANGE_FAILED); return; }
    if (!s->kex_done_once) s->strict = list_has(lists[0], lens[0], "kex-strict-s-v00@openssh.com");
    if (!s->kex_sent && !send_kexinit(s)) return; /* their rekey */
    s->kex_got = true;
    s->in_kex = true;
    sha256_init(&s->hash);
    hash_string(s, VERSION, strlen(VERSION));
    hash_string(s, s->v_s, strlen(s->v_s));
    hash_string(s, s->i_c, s->i_c_len);
    hash_string(s, payload, len);
    s->io.random(s->io.ctx, s->x_secret, sizeof s->x_secret);
    crypto_x25519_public_key(q_c, s->x_secret);
    w = begin(s, MSG_KEX_ECDH_INIT);
    put_string(&w, q_c, sizeof q_c);
    send_packet(s, &w);
}

static void send_newkeys(ssh_t *s);

static void on_ecdh_reply(ssh_t *s, const uint8_t *payload, size_t len) {
    reader_t r = {payload + 1, len - 1, false}, blob, sig;
    const uint8_t *k_s, *q_s, *sig_blob, *alg, *pk, *sg;
    uint32_t k_s_len, q_s_len, sig_len, alg_len, pk_len, sg_len;
    uint8_t q_c[32], shared[32], k[4 + 33], h[32];
    size_t k_len;
    if (!s->kex_got) { fail(s, SSH_ERR_PROTOCOL, DC_PROTOCOL_ERROR); return; }
    k_s = get_string(&r, &k_s_len);
    q_s = get_string(&r, &q_s_len);
    sig_blob = get_string(&r, &sig_len);
    if (r.bad || q_s_len != 32 || k_s_len != SSH_HOSTKEY_BLOB_LEN) { fail(s, SSH_ERR_PROTOCOL, DC_KEY_EXCHANGE_FAILED); return; }
    blob = (reader_t){k_s, k_s_len, false};
    alg = get_string(&blob, &alg_len);
    pk = get_string(&blob, &pk_len);
    if (blob.bad || !string_is(alg, alg_len, HOSTKEY_ALG) || pk_len != 32) { fail(s, SSH_ERR_ALGORITHMS, DC_KEY_EXCHANGE_FAILED); return; }
    sig = (reader_t){sig_blob, sig_len, false};
    alg = get_string(&sig, &alg_len);
    sg = get_string(&sig, &sg_len);
    if (sig.bad || !string_is(alg, alg_len, HOSTKEY_ALG) || sg_len != 64) { fail(s, SSH_ERR_HOSTKEY, DC_KEY_EXCHANGE_FAILED); return; }

    crypto_x25519_public_key(q_c, s->x_secret);
    crypto_x25519(shared, s->x_secret, q_s);
    crypto_wipe(s->x_secret, sizeof s->x_secret);
    {
        uint8_t zero = 0;
        for (int i = 0; i < 32; i++) zero |= shared[i];
        if (!zero) { fail(s, SSH_ERR_PROTOCOL, DC_KEY_EXCHANGE_FAILED); return; }
    }
    k_len = mpint(shared, k);
    crypto_wipe(shared, sizeof shared);
    hash_string(s, k_s, k_s_len);
    hash_string(s, q_c, sizeof q_c);
    hash_string(s, q_s, q_s_len);
    sha256_update(&s->hash, k, k_len);
    sha256_final(&s->hash, h);
    if (crypto_ed25519_check(sg, pk, h, sizeof h) != 0) {
        crypto_wipe(k, sizeof k);
        { fail(s, SSH_ERR_HOSTKEY, DC_HOST_KEY_NOT_VERIFIABLE); return; }
    }
    if (!s->kex_done_once) memcpy(s->session_id, h, sizeof h);
    derive(s, k, k_len, h, 'C', s->next_tx);
    derive(s, k, k_len, h, 'D', s->next_rx);
    crypto_wipe(k, sizeof k);
    memcpy(s->hostkey, k_s, SSH_HOSTKEY_BLOB_LEN);
    if (s->kex_done_once) { /* a rekey: the same host, or nothing */
        if (memcmp(s->hostkey, s->first_hostkey, SSH_HOSTKEY_BLOB_LEN) != 0)
            { fail(s, SSH_ERR_HOSTKEY, DC_HOST_KEY_NOT_VERIFIABLE); return; }
        send_newkeys(s);
        return;
    }
    s->state = SSH_ST_HOSTKEY;
}

static void kex_finished(ssh_t *s);

static void send_newkeys(ssh_t *s) {
    writer_t w = begin(s, MSG_NEWKEYS);
    if (!send_packet(s, &w)) return;
    memcpy(s->tx.key, s->next_tx, sizeof s->tx.key);
    crypto_wipe(s->next_tx, sizeof s->next_tx);
    s->tx.on = true;
    if (s->strict) s->tx.seq = 0;
    s->newkeys_sent = true;
    kex_finished(s);
}

static void on_newkeys(ssh_t *s) {
    if (!s->kex_got || s->newkeys_got) { fail(s, SSH_ERR_PROTOCOL, DC_PROTOCOL_ERROR); return; }
    memcpy(s->rx.key, s->next_rx, sizeof s->rx.key);
    crypto_wipe(s->next_rx, sizeof s->next_rx);
    s->rx.on = true;
    if (s->strict) s->rx.seq = 0;
    s->newkeys_got = true;
    kex_finished(s);
}

/* Both NEWKEYS done: the round is over. After the first, log in. */
static void kex_finished(ssh_t *s) {
    writer_t w;
    if (!s->newkeys_sent || !s->newkeys_got) return;
    s->kex_sent = s->kex_got = s->newkeys_sent = s->newkeys_got = false;
    s->in_kex = false;
    if (s->kex_done_once) return;
    s->kex_done_once = true;
    w = begin(s, MSG_SERVICE_REQUEST);
    put_cstring(&w, "ssh-userauth");
    send_packet(s, &w);
}

/* ---- logging in ---- */

static void send_auth_header(writer_t *w, const ssh_t *s, const char *method) {
    put_cstring(w, s->user);
    put_cstring(w, "ssh-connection");
    put_cstring(w, method);
}

static void public_blob(const uint8_t public_key[32], uint8_t out[SSH_HOSTKEY_BLOB_LEN]) {
    store_u32(out, 11);
    memcpy(out + 4, HOSTKEY_ALG, 11);
    store_u32(out + 15, 32);
    memcpy(out + 19, public_key, 32);
}

/* publickey with the signature at once -- no query first (RFC 4252 sec.7). */
static void send_publickey(ssh_t *s) {
    uint8_t blob[SSH_HOSTKEY_BLOB_LEN], sig[64], sig_blob[4 + 11 + 4 + 64];
    uint8_t signed_data[4 + 32 + 1 + 4 + SSH_USER_MAX + 4 + 14 + 4 + 9 + 1 + 4 + 11 + 4 + SSH_HOSTKEY_BLOB_LEN];
    writer_t w = begin(s, MSG_USERAUTH_REQUEST), d = {signed_data, 0, sizeof signed_data, false};
    public_blob(s->key + 32, blob);
    put_string(&d, s->session_id, 32);
    put_u8(&d, MSG_USERAUTH_REQUEST);
    send_auth_header(&d, s, "publickey");
    put_u8(&d, 1);
    put_cstring(&d, HOSTKEY_ALG);
    put_string(&d, blob, sizeof blob);
    crypto_ed25519_sign(sig, s->key, signed_data, d.n);
    store_u32(sig_blob, 11);
    memcpy(sig_blob + 4, HOSTKEY_ALG, 11);
    store_u32(sig_blob + 15, 64);
    memcpy(sig_blob + 19, sig, 64);
    send_auth_header(&w, s, "publickey");
    put_u8(&w, 1);
    put_cstring(&w, HOSTKEY_ALG);
    put_string(&w, blob, sizeof blob);
    put_string(&w, sig_blob, sizeof sig_blob);
    send_packet(s, &w);
}

static void send_password(ssh_t *s) {
    writer_t w = begin(s, MSG_USERAUTH_REQUEST);
    send_auth_header(&w, s, "password");
    put_u8(&w, 0);
    put_cstring(&w, s->password);
    send_packet(s, &w);
    crypto_wipe(w.p, w.n); /* the password, in the clear in tx_buf until sealed */
}

/* The next way in, given what the server last said it takes (`methods`,
 * NULL before it has said: try anything). */
static void auth_next(ssh_t *s, const uint8_t *methods, uint32_t len) {
    bool key_ok = !methods || list_has(methods, len, "publickey");
    bool pw_ok = !methods || list_has(methods, len, "password");
    if (s->have_key && !s->tried_key && key_ok) {
        s->tried_key = true;
        send_publickey(s);
    } else if (s->password[0] && !s->tried_password && pw_ok) {
        s->tried_password = true;
        send_password(s);
    } else if (!methods) { /* nothing to try: ask what it takes */
        writer_t w = begin(s, MSG_USERAUTH_REQUEST);
        send_auth_header(&w, s, "none");
        send_packet(s, &w);
    } else if (pw_ok && s->pw_tries < PASSWORD_TRIES) {
        crypto_wipe(s->password, sizeof s->password);
        s->state = SSH_ST_PASSWORD;
    } else {
        fail(s, SSH_ERR_AUTH, DC_NO_MORE_AUTH_METHODS);
    }
}

/* ---- the session channel ---- */

static void send_channel_open(ssh_t *s) {
    writer_t w = begin(s, MSG_CHANNEL_OPEN);
    put_cstring(&w, "session");
    put_u32(&w, 0);
    put_u32(&w, LOCAL_WINDOW);
    put_u32(&w, LOCAL_MAX_PACKET);
    s->local_window = LOCAL_WINDOW;
    s->channel_step = 0;
    send_packet(s, &w);
}

static void send_pty_req(ssh_t *s) {
    static const uint8_t modes[] = {3, 0, 0, 0, 127, 0}; /* VERASE = DEL (what backspace sends), TTY_OP_END */
    writer_t w = begin(s, MSG_CHANNEL_REQUEST);
    put_u32(&w, s->remote_id);
    put_cstring(&w, "pty-req");
    put_u8(&w, 1);
    put_cstring(&w, "dumb"); /* ssh_term.c drops escape sequences anyway */
    put_u32(&w, SSH_COLS);
    put_u32(&w, SSH_ROWS);
    put_u32(&w, 0);
    put_u32(&w, 0);
    put_string(&w, modes, sizeof modes);
    send_packet(s, &w);
}

static void send_shell(ssh_t *s) {
    writer_t w = begin(s, MSG_CHANNEL_REQUEST);
    put_u32(&w, s->remote_id);
    put_cstring(&w, "shell");
    put_u8(&w, 1);
    send_packet(s, &w);
}

static void channel_data(ssh_t *s, const uint8_t *data, uint32_t len) {
    if (len > s->local_window) { fail(s, SSH_ERR_PROTOCOL, DC_PROTOCOL_ERROR); return; }
    s->local_window -= len;
    if (len) s->io.data(s->io.ctx, data, len);
    if (s->state == SSH_ST_OPEN && s->local_window < LOCAL_WINDOW / 2 && !s->in_kex) {
        writer_t w = begin(s, MSG_CHANNEL_WINDOW_ADJUST);
        put_u32(&w, s->remote_id);
        put_u32(&w, LOCAL_WINDOW - s->local_window);
        s->local_window = LOCAL_WINDOW;
        send_packet(s, &w);
    }
}

static void on_channel_close(ssh_t *s) {
    if (!s->close_sent) {
        writer_t w = begin(s, MSG_CHANNEL_CLOSE);
        put_u32(&w, s->remote_id);
        s->close_sent = true;
        if (!send_packet(s, &w)) return;
    }
    send_disconnect(s, DC_BY_APPLICATION);
    s->state = SSH_ST_CLOSED;
    s->error = SSH_ERR_NONE;
    wipe_secrets(s);
}

/* ---- dispatch ---- */

static bool kex_message(uint8_t type) {
    return type == MSG_KEXINIT || type == MSG_NEWKEYS || type == MSG_KEX_ECDH_INIT || type == MSG_KEX_ECDH_REPLY;
}

static void dispatch(ssh_t *s, const uint8_t *payload, size_t len) {
    reader_t r = {payload + 1, len - 1, false};
    uint8_t type = payload[0];
    uint32_t n;
    const uint8_t *str;
    if (type == MSG_DISCONNECT) {
        s->disconnect_reason = get_u32(&r);
        s->error = SSH_ERR_DISCONNECTED;
        s->state = SSH_ST_CLOSED;
        wipe_secrets(s);
        return;
    }
    /* Strict KEX: nothing but key exchange until the first one is done. */
    if (!s->kex_done_once && !kex_message(type)) {
        if (s->strict || !(type == MSG_IGNORE || type == MSG_DEBUG || type == MSG_UNIMPLEMENTED))
            fail(s, SSH_ERR_PROTOCOL, DC_PROTOCOL_ERROR);
        return;
    }
    switch (type) {
        case MSG_IGNORE:
        case MSG_DEBUG:
        case MSG_UNIMPLEMENTED:
        case MSG_EXT_INFO:
        case MSG_REQUEST_SUCCESS:
        case MSG_REQUEST_FAILURE:
            return;
        case MSG_KEXINIT: { on_kexinit(s, payload, len); return; }
        case MSG_KEX_ECDH_REPLY: { on_ecdh_reply(s, payload, len); return; }
        case MSG_NEWKEYS: { on_newkeys(s); return; }
        case MSG_SERVICE_ACCEPT:
            if (s->state != SSH_ST_KEX) { fail(s, SSH_ERR_PROTOCOL, DC_PROTOCOL_ERROR); return; }
            s->state = SSH_ST_AUTH;
            { auth_next(s, NULL, 0); return; }
        case MSG_USERAUTH_BANNER:
            str = get_string(&r, &n);
            if (!r.bad && n) s->io.data(s->io.ctx, str, n);
            return;
        case MSG_USERAUTH_FAILURE:
            if (s->state != SSH_ST_AUTH) { fail(s, SSH_ERR_PROTOCOL, DC_PROTOCOL_ERROR); return; }
            str = get_string(&r, &n);
            if (r.bad) { fail(s, SSH_ERR_PROTOCOL, DC_PROTOCOL_ERROR); return; }
            { auth_next(s, str, n); return; }
        case MSG_USERAUTH_60: { fail(s, SSH_ERR_AUTH, DC_NO_MORE_AUTH_METHODS); return; }
        case MSG_USERAUTH_SUCCESS:
            if (s->state != SSH_ST_AUTH) { fail(s, SSH_ERR_PROTOCOL, DC_PROTOCOL_ERROR); return; }
            crypto_wipe(s->password, sizeof s->password);
            crypto_wipe(s->key, sizeof s->key);
            s->state = SSH_ST_CHANNEL;
            { send_channel_open(s); return; }
        case MSG_GLOBAL_REQUEST:
            get_string(&r, &n);
            if (get_u8(&r) && !r.bad) {
                writer_t w = begin(s, MSG_REQUEST_FAILURE);
                send_packet(s, &w);
            }
            return;
        case MSG_CHANNEL_OPEN: { /* the server opening one to us (forwarding): no */
            writer_t w = begin(s, MSG_CHANNEL_OPEN_FAILURE);
            get_string(&r, &n);
            put_u32(&w, get_u32(&r));
            put_u32(&w, 1); /* ADMINISTRATIVELY_PROHIBITED */
            put_cstring(&w, "");
            put_cstring(&w, "");
            send_packet(s, &w);
            return;
        }
        default: break;
    }
    /* the channel's: ours is 0, and only once it's being opened */
    if (type >= MSG_CHANNEL_OPEN_CONFIRMATION && type <= MSG_CHANNEL_FAILURE) {
        if ((s->state != SSH_ST_CHANNEL && s->state != SSH_ST_OPEN) || get_u32(&r) != 0)
            { fail(s, SSH_ERR_PROTOCOL, DC_PROTOCOL_ERROR); return; }
        switch (type) {
            case MSG_CHANNEL_OPEN_CONFIRMATION:
                if (s->channel_step != 0) { fail(s, SSH_ERR_PROTOCOL, DC_PROTOCOL_ERROR); return; }
                s->remote_id = get_u32(&r);
                s->remote_window = get_u32(&r);
                s->remote_max = get_u32(&r);
                if (r.bad) { fail(s, SSH_ERR_PROTOCOL, DC_PROTOCOL_ERROR); return; }
                s->channel_step = 1;
                { send_pty_req(s); return; }
            case MSG_CHANNEL_OPEN_FAILURE:
            case MSG_CHANNEL_FAILURE:
                { fail(s, SSH_ERR_CHANNEL, DC_BY_APPLICATION); return; }
            case MSG_CHANNEL_SUCCESS:
                if (s->channel_step == 1) {
                    s->channel_step = 2;
                    { send_shell(s); return; }
                }
                if (s->channel_step == 2) {
                    s->channel_step = 3;
                    s->state = SSH_ST_OPEN;
                }
                return;
            case MSG_CHANNEL_WINDOW_ADJUST:
                n = get_u32(&r);
                if (!r.bad) s->remote_window = (s->remote_window + n < s->remote_window) ? UINT32_MAX : s->remote_window + n;
                return;
            case MSG_CHANNEL_DATA:
                str = get_string(&r, &n);
                if (r.bad) { fail(s, SSH_ERR_PROTOCOL, DC_PROTOCOL_ERROR); return; }
                { channel_data(s, str, n); return; }
            case MSG_CHANNEL_EXTENDED_DATA:
                get_u32(&r); /* 1 = stderr: shown the same */
                str = get_string(&r, &n);
                if (r.bad) { fail(s, SSH_ERR_PROTOCOL, DC_PROTOCOL_ERROR); return; }
                { channel_data(s, str, n); return; }
            case MSG_CHANNEL_EOF: return;
            case MSG_CHANNEL_CLOSE: { on_channel_close(s); return; }
            case MSG_CHANNEL_REQUEST: /* exit-status, keepalive@openssh.com...: none wanted */
                get_string(&r, &n);
                if (get_u8(&r) && !r.bad) {
                    writer_t w = begin(s, MSG_CHANNEL_FAILURE);
                    put_u32(&w, s->remote_id);
                    send_packet(s, &w);
                }
                return;
            default: return;
        }
    }
    {
        writer_t w = begin(s, MSG_UNIMPLEMENTED);
        put_u32(&w, s->rx.seq - 1);
        send_packet(s, &w);
    }
}

/* ---- receiving ---- */

static bool paused(const ssh_t *s) {
    return s->state == SSH_ST_HOSTKEY || s->state == SSH_ST_PASSWORD || s->state == SSH_ST_CLOSED;
}

static void consume(ssh_t *s, size_t n) {
    memmove(s->rx_buf, s->rx_buf + n, s->rx_len - n);
    s->rx_len = (uint16_t)(s->rx_len - n);
}

/* The server's version line (RFC 4253 sec.4.2); lines before it are skipped. */
static bool version_line(ssh_t *s) {
    uint8_t *nl = memchr(s->rx_buf, '\n', s->rx_len);
    size_t n;
    if (!nl) {
        if (s->rx_len >= sizeof s->v_s) fail(s, SSH_ERR_PROTOCOL, DC_PROTOCOL_ERROR);
        return false;
    }
    n = (size_t)(nl - s->rx_buf);
    if (n >= 4 && memcmp(s->rx_buf, "SSH-", 4) == 0) {
        size_t m = n && s->rx_buf[n - 1] == '\r' ? n - 1 : n;
        if (m >= sizeof s->v_s || !(m >= 8 && (memcmp(s->rx_buf, "SSH-2.0-", 8) == 0 || memcmp(s->rx_buf, "SSH-1.99-", 9) == 0))) {
            fail(s, SSH_ERR_PROTOCOL, DC_PROTOCOL_ERROR);
            return false;
        }
        memcpy(s->v_s, s->rx_buf, m);
        s->v_s[m] = 0;
        consume(s, n + 1);
        s->state = SSH_ST_KEX;
        return send_kexinit(s);
    }
    consume(s, n + 1);
    return true;
}

/* One whole packet, if it's there: checked, opened, dispatched. */
static bool next_packet(ssh_t *s) {
    uint8_t *p = s->rx_buf, nonce[8], pk[32], mac[MAC_LEN];
    uint32_t plen;
    if (s->rx_len < 4) return false;
    if (s->rx.on) {
        uint8_t len_bytes[4];
        nonce_for(s->rx.seq, nonce);
        crypto_chacha20_djb(len_bytes, p, 4, s->rx.key + 32, nonce, 0);
        plen = (uint32_t)len_bytes[0] << 24 | (uint32_t)len_bytes[1] << 16 | (uint32_t)len_bytes[2] << 8 | len_bytes[3];
        if (plen < 8 || plen > SSH_PACKET_MAX || plen % 8) {
            fail(s, SSH_ERR_MAC, DC_MAC_ERROR);
            return false;
        }
        if (s->rx_len < 4 + plen + MAC_LEN) return false;
        poly_key(&s->rx, nonce, pk);
        crypto_poly1305(mac, p, 4 + plen, pk);
        crypto_wipe(pk, sizeof pk);
        if (crypto_verify16(mac, p + 4 + plen) != 0) {
            fail(s, SSH_ERR_MAC, DC_MAC_ERROR);
            return false;
        }
        crypto_chacha20_djb(p + 4, p + 4, plen, s->rx.key, nonce, 1);
    } else {
        plen = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
        if (plen < 8 || plen > SSH_PACKET_MAX) {
            fail(s, SSH_ERR_PROTOCOL, DC_PROTOCOL_ERROR);
            return false;
        }
        if (s->rx_len < 4 + plen) return false;
    }
    if (p[4] + 1u >= plen) {
        fail(s, SSH_ERR_PROTOCOL, DC_PROTOCOL_ERROR);
        return false;
    }
    {
        size_t total = 4 + plen + (s->rx.on ? MAC_LEN : 0); /* before NEWKEYS turns the keys on */
        s->rx.seq++;
        dispatch(s, p + 5, plen - 1 - p[4]);
        consume(s, total);
    }
    return true;
}

static void process(ssh_t *s) {
    while (!paused(s)) {
        if (s->state == SSH_ST_VERSION ? !version_line(s) : !next_packet(s)) break;
    }
}

/* ---- the interface ---- */

void ssh_start(ssh_t *s, const ssh_io_t *io, const char *user, const uint8_t *key, const char *password) {
    memset(s, 0, sizeof *s);
    s->io = *io;
    strncpy(s->user, user, SSH_USER_MAX);
    if (key) {
        memcpy(s->key, key, sizeof s->key);
        s->have_key = true;
    }
    if (password) strncpy(s->password, password, SSH_PASSWORD_MAX);
    s->state = SSH_ST_VERSION;
    if (!s->io.send(s->io.ctx, (const uint8_t *)VERSION "\r\n", strlen(VERSION) + 2)) {
        s->error = SSH_ERR_IO;
        s->state = SSH_ST_CLOSED;
        wipe_secrets(s);
    }
}

size_t ssh_feed(ssh_t *s, const uint8_t *data, size_t len) {
    size_t taken = 0;
    while (taken < len && s->state != SSH_ST_CLOSED) {
        size_t n = sizeof s->rx_buf - s->rx_len;
        if (n == 0) break; /* paused, and full: the rest waits with the caller */
        if (n > len - taken) n = len - taken;
        memcpy(s->rx_buf + s->rx_len, data + taken, n);
        s->rx_len = (uint16_t)(s->rx_len + n);
        taken += n;
        process(s);
    }
    return s->state == SSH_ST_CLOSED ? len : taken;
}

void ssh_hostkey_answer(ssh_t *s, bool accept) {
    if (s->state != SSH_ST_HOSTKEY) return;
    if (!accept) { fail(s, SSH_ERR_REJECTED, DC_HOST_KEY_NOT_VERIFIABLE); return; }
    memcpy(s->first_hostkey, s->hostkey, SSH_HOSTKEY_BLOB_LEN);
    s->state = SSH_ST_KEX;
    send_newkeys(s);
    process(s);
}

const uint8_t *ssh_hostkey_blob(const ssh_t *s) { return s->hostkey; }

void ssh_password(ssh_t *s, const char *password) {
    if (s->state != SSH_ST_PASSWORD) return;
    strncpy(s->password, password, SSH_PASSWORD_MAX);
    s->password[SSH_PASSWORD_MAX] = 0;
    s->state = SSH_ST_AUTH;
    s->tried_password = true;
    s->pw_tries++;
    send_password(s);
    process(s);
}

size_t ssh_write(ssh_t *s, const uint8_t *data, size_t len) {
    writer_t w;
    if (s->state != SSH_ST_OPEN || s->in_kex) return 0;
    if (len > s->remote_window) len = s->remote_window;
    if (len > s->remote_max) len = s->remote_max;
    if (len > WRITE_CHUNK) len = WRITE_CHUNK;
    if (len == 0) return 0;
    w = begin(s, MSG_CHANNEL_DATA);
    put_u32(&w, s->remote_id);
    put_string(&w, data, len);
    if (!send_packet(s, &w)) return 0;
    s->remote_window -= (uint32_t)len;
    return len;
}

void ssh_close(ssh_t *s, bool io_lost) {
    if (s->state == SSH_ST_CLOSED) return;
    if (io_lost) {
        s->error = SSH_ERR_IO;
        s->state = SSH_ST_CLOSED;
        wipe_secrets(s);
        return;
    }
    if (s->state == SSH_ST_OPEN && !s->close_sent && !s->in_kex) {
        writer_t w = begin(s, MSG_CHANNEL_CLOSE);
        put_u32(&w, s->remote_id);
        s->close_sent = true;
        send_packet(s, &w);
    }
    fail(s, SSH_ERR_NONE, DC_BY_APPLICATION);
}

size_t ssh_base64(const uint8_t *data, size_t len, char *out, bool pad) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)data[i] << 16;
        size_t n = len - i < 3 ? len - i : 3;
        if (n > 1) v |= (uint32_t)data[i + 1] << 8;
        if (n > 2) v |= data[i + 2];
        out[o++] = T[v >> 18 & 63];
        out[o++] = T[v >> 12 & 63];
        if (n > 1) out[o++] = T[v >> 6 & 63];
        else if (pad) out[o++] = '=';
        if (n > 2) out[o++] = T[v & 63];
        else if (pad) out[o++] = '=';
    }
    out[o] = 0;
    return o;
}

void ssh_fingerprint(const uint8_t blob[SSH_HOSTKEY_BLOB_LEN], char out[SSH_FINGERPRINT_LEN + 1]) {
    uint8_t h[SHA256_LEN];
    sha256(blob, SSH_HOSTKEY_BLOB_LEN, h);
    memcpy(out, "SHA256:", 7);
    ssh_base64(h, sizeof h, out + 7, false);
}

size_t ssh_public_key_line(const uint8_t public_key[32], char *out) {
    uint8_t blob[SSH_HOSTKEY_BLOB_LEN];
    public_blob(public_key, blob);
    memcpy(out, HOSTKEY_ALG " ", 12);
    return 12 + ssh_base64(blob, sizeof blob, out + 12, true);
}
