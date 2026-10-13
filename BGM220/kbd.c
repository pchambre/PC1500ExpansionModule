/* kbd.c -- see kbd.h. The keyboard's states are the RP2354B's EXP_KBD_*
 * (pc_exp.h), sent as KBD_STATE events.
 *
 * Pairing: active scan for an advertiser with the HID service (0x1812) or
 * a keyboard's appearance (0x03C1); connect; ask for security -- this side
 * says it can display but not input, with MITM preferred, so a keyboard
 * pairs by Passkey Entry: KBD_PASSKEY to the PC-1500 to show, typed on the
 * keyboard. Then the HID service: its Report Map (read whole), each Report
 * characteristic's Report Reference (ID, type), and notifications on the
 * input reports. A reconnection does the same without pairing: the bond
 * encrypts the link, and discovery runs again (a few hundred ms; the
 * keyboard's first key after it slept may be lost, as on the dongle). */
#include "kbd.h"

#include <string.h>

#include "app.h"
#include "events.h"
#include "hidmap.h"
#include "hostlink.h"
#include "module_config.h"
#include "nvm3_default.h"
#include "sl_sleeptimer.h"

enum { /* pc_exp.h EXP_KBD_* */
    KS_NONE = 0,
    KS_SEARCHING = 1,
    KS_CONNECTING = 2,
    KS_CODE = 3,
    KS_CONNECTED = 4,
    KS_NOT_FOUND = 5,
    KS_FAILED = 6,
    KS_PAIRED = 7,
};

#define NVM_KEY 0x0F001 /* the keyboard's record, in the user range */
#define REC_MAGIC 0x4B
#define KBD_NAME_MAX 16
#define PAIR_TIMEOUT_MS 60000u /* connect, pair (the user types the code), discover */
#define NO_CONN 0xFF

static struct {
    uint8_t magic;
    uint8_t bonding;
    uint8_t addr_type;
    uint8_t addr[6]; /* the stack's order */
    uint8_t name_len;
    char name[KBD_NAME_MAX];
} rec;
static bool have_rec;

static uint8_t state = KS_NONE;
static uint8_t conn = NO_CONN;
static bool enabled, pairing;
static bd_addr pair_addr;
static uint8_t pair_type;
static char pair_name[KBD_NAME_MAX + 1];

static enum { G_NONE, G_ENCRYPT, G_SERVICE, G_CHARS, G_MAP, G_DESC, G_REF, G_NOTIFY, G_DONE } g = G_NONE;
static uint32_t hid_service;
static uint16_t map_handle;
#define MAX_REPORTS 8
static struct {
    uint16_t handle, ref;
    uint8_t id, type; /* type: 1 input (0 = no Report Reference) */
    bool notify;
} rep[MAX_REPORTS];
static uint8_t nrep, cur;
static uint8_t map[512];
static uint16_t map_len;
static hidmap_keyboard_t kb;
static bool kb_ok;

static sl_sleeptimer_timer_handle_t search_timer, pair_timer;
static volatile bool search_expired, pair_expired;

static void search_cb(sl_sleeptimer_timer_handle_t *h, void *d) {
    (void)h, (void)d;
    search_expired = true;
}

static void pair_cb(sl_sleeptimer_timer_handle_t *h, void *d) {
    (void)h, (void)d;
    pair_expired = true;
}

static void set_state(uint8_t s) {
    state = s;
    events_push(EV_KBD_STATE, &s, 1);
}

static void rec_save(void) {
    rec.magic = REC_MAGIC;
    nvm3_writeData(nvm3_defaultHandle, NVM_KEY, &rec, sizeof rec);
    have_rec = true;
}

static void rec_forget(void) {
    if (have_rec && rec.bonding != 0xFF) {
        sl_bt_sm_delete_bonding(rec.bonding);
    }
    nvm3_deleteObject(nvm3_defaultHandle, NVM_KEY);
    have_rec = false;
    memset(&rec, 0, sizeof rec);
}

static void close_conn(void) {
    if (conn != NO_CONN) {
        sl_bt_connection_close(conn); /* the closed event tidies up */
    }
}

void kbd_init(void) {
    /* Display Only, MITM preferred: a keyboard pairs by Passkey Entry. The
     * Link never pairs (BLE_PROTOCOL.md sec.2), so this is the keyboard's. */
    sl_bt_sm_configure(SL_BT_SM_CONFIGURATION_PREFER_MITM, sl_bt_sm_io_capability_displayonly);
    sl_bt_sm_set_bondable_mode(1);
    have_rec = nvm3_readData(nvm3_defaultHandle, NVM_KEY, &rec, sizeof rec) == SL_STATUS_OK && rec.magic == REC_MAGIC;
    state = have_rec ? KS_PAIRED : KS_NONE;
}

/* ---- finding a keyboard ---- */

static bool adv_is_keyboard(const uint8_t *d, size_t n, char *name) {
    bool hid = false;
    size_t i = 0;
    while (i + 1 < n) {
        uint8_t len = d[i], type = d[i + 1];
        if (len == 0 || i + 1 + len > n) {
            break;
        }
        const uint8_t *v = d + i + 2;
        size_t vlen = len - 1u;
        if (type == 0x02 || type == 0x03) { /* incomplete/complete 16-bit UUIDs */
            for (size_t k = 0; k + 2 <= vlen; k += 2) {
                if (v[k] == 0x12 && v[k + 1] == 0x18) {
                    hid = true;
                }
            }
        } else if (type == 0x19 && vlen == 2 && v[0] == 0xC1 && v[1] == 0x03) { /* appearance: keyboard */
            hid = true;
        } else if ((type == 0x08 || type == 0x09) && vlen) {
            if (vlen > KBD_NAME_MAX) {
                vlen = KBD_NAME_MAX;
            }
            memcpy(name, v, vlen);
            name[vlen] = 0;
        }
        i += 1u + len;
    }
    return hid;
}

static bool open_to(bd_addr addr, uint8_t type) {
    uint8_t c;
    if (app_link_connecting()) {
        return false; /* one opening at a time: try on the next advert */
    }
    if (sl_bt_connection_open(addr, type, sl_bt_gap_phy_1m, &c) != SL_STATUS_OK) {
        return false;
    }
    conn = c;
    g = G_NONE;
    return true;
}

void kbd_on_adv(const sl_bt_evt_scanner_legacy_advertisement_report_t *e) {
    if (scanner_owner != SCAN_KBD || conn != NO_CONN) {
        return;
    }
    if (!(e->event_flags & SL_BT_SCANNER_EVENT_FLAG_CONNECTABLE) &&
        !(e->event_flags & SL_BT_SCANNER_EVENT_FLAG_SCAN_RESPONSE)) {
        return;
    }
    if (pairing && state == KS_SEARCHING) {
        char name[KBD_NAME_MAX + 1] = "KEYBOARD";
        if (!adv_is_keyboard(e->data.data, e->data.len, name)) {
            return;
        }
        if (!(e->event_flags & SL_BT_SCANNER_EVENT_FLAG_CONNECTABLE)) {
            return; /* its scan response: connect on its advertisement */
        }
        pair_addr = e->address;
        pair_type = e->address_type;
        strcpy(pair_name, name);
        if (open_to(e->address, e->address_type)) {
            sl_sleeptimer_stop_timer(&search_timer);
            scanner_release(SCAN_KBD);
            set_state(KS_CONNECTING);
        }
    } else if (!pairing && have_rec && enabled && (e->event_flags & SL_BT_SCANNER_EVENT_FLAG_CONNECTABLE) &&
               ((e->bonding != 0xFF && e->bonding == rec.bonding) ||
                (e->address_type == rec.addr_type && memcmp(e->address.addr, rec.addr, 6) == 0))) {
        if (open_to(e->address, e->address_type)) {
            scanner_release(SCAN_KBD); /* the bonded keyboard: reconnect */
        }
    }
}

/* ---- the HID service ---- */

static void fail(uint16_t why) {
    events_log_error("KBD failed 0x%04X step %d", (unsigned)why, (int)g);
    if (pairing) {
        set_state(KS_FAILED);
        pairing = false;
        sl_sleeptimer_stop_timer(&pair_timer);
    }
    close_conn();
}

static void start_discovery(void) {
    static const uint8_t hid_uuid[2] = {0x12, 0x18};
    hid_service = 0;
    map_handle = 0;
    nrep = 0;
    map_len = 0;
    kb_ok = false;
    g = G_SERVICE;
    if (sl_bt_gatt_discover_primary_services_by_uuid(conn, 2, hid_uuid) != SL_STATUS_OK) {
        fail(0xF101);
    }
}

static void next_desc(void) {
    while (cur < nrep && !rep[cur].notify) {
        cur++;
    }
    if (cur < nrep) {
        g = G_DESC;
        if (sl_bt_gatt_discover_descriptors(conn, rep[cur].handle) != SL_STATUS_OK) {
            fail(0xF104);
        }
        return;
    }
    cur = 0;
    g = G_NOTIFY;
    /* falls to next_notify() from the caller */
}

static void done(void) {
    g = G_DONE;
    if (!kb_ok) {
        events_log_error("KBD no keyboard report in its map (%u bytes)", (unsigned)map_len);
    }
    if (pairing) {
        pairing = false;
        sl_sleeptimer_stop_timer(&pair_timer);
        size_t n = strlen(pair_name);
        memcpy(rec.addr, pair_addr.addr, 6);
        rec.addr_type = pair_type;
        rec.name_len = (uint8_t)n;
        memcpy(rec.name, pair_name, n);
        rec_save();
    }
    set_state(KS_CONNECTED);
}

static void next_notify(void) {
    while (cur < nrep && !(rep[cur].notify && (rep[cur].type == 1 || rep[cur].type == 0))) {
        cur++;
    }
    if (cur >= nrep) {
        done();
        return;
    }
    if (sl_bt_gatt_set_characteristic_notification(conn, rep[cur].handle, sl_bt_gatt_notification) != SL_STATUS_OK) {
        fail(0xF105);
    }
}

static void step(uint16_t result) {
    if (result != 0 && g != G_REF && g != G_DESC) {
        fail(result);
        return;
    }
    switch (g) {
        case G_SERVICE:
            if (!hid_service) {
                fail(0xF102);
                return;
            }
            g = G_CHARS;
            if (sl_bt_gatt_discover_characteristics(conn, hid_service) != SL_STATUS_OK) {
                fail(0xF103);
            }
            break;
        case G_CHARS:
            if (map_handle) {
                g = G_MAP;
                if (sl_bt_gatt_read_characteristic_value(conn, map_handle) == SL_STATUS_OK) {
                    break;
                }
            }
            __attribute__((fallthrough)); /* no map to read */
        case G_MAP:
            kb_ok = map_len && hidmap_find_keyboard(map, map_len, &kb);
            cur = 0;
            next_desc();
            if (g == G_NOTIFY) {
                next_notify();
            }
            break;
        case G_DESC:
            if (rep[cur].ref) {
                g = G_REF;
                if (sl_bt_gatt_read_descriptor_value(conn, rep[cur].ref) == SL_STATUS_OK) {
                    break;
                }
            }
            cur++;
            next_desc();
            if (g == G_NOTIFY) {
                next_notify();
            }
            break;
        case G_REF:
            cur++;
            next_desc();
            if (g == G_NOTIFY) {
                next_notify();
            }
            break;
        case G_NOTIFY:
            cur++;
            next_notify();
            break;
        default:
            break;
    }
}

static void report_in(uint16_t handle, const uint8_t *v, size_t len) {
    uint8_t boot[8];
    uint8_t id = 0;
    for (uint8_t i = 0; i < nrep; i++) {
        if (rep[i].handle == handle) {
            id = rep[i].id;
        }
    }
    if (kb_ok) {
        if (id != kb.report_id) {
            return; /* a touchpad's or a media key's report */
        }
        hidmap_to_boot(&kb, v, len, boot);
    } else if (len == 8) { /* no usable map: a boot-shaped report */
        memcpy(boot, v, 8);
    } else {
        return;
    }
    events_push(EV_KBD_REPORT, boot, 8);
}

bool kbd_on_event(sl_bt_msg_t *evt) {
    switch (SL_BT_MSG_ID(evt->header)) {
        case sl_bt_evt_connection_opened_id:
            if (evt->data.evt_connection_opened.connection != conn || conn == NO_CONN) {
                return false;
            }
            g = G_ENCRYPT;
            if (sl_bt_sm_increase_security(conn) != SL_STATUS_OK) {
                fail(0xF100);
            }
            return true;
        case sl_bt_evt_sm_passkey_display_id: {
            sl_bt_evt_sm_passkey_display_t *e = &evt->data.evt_sm_passkey_display;
            if (e->connection != conn) {
                return false;
            }
            uint8_t b[4] = {(uint8_t)e->passkey, (uint8_t)(e->passkey >> 8), (uint8_t)(e->passkey >> 16),
                            (uint8_t)(e->passkey >> 24)};
            events_push(EV_KBD_PASSKEY, b, 4);
            set_state(KS_CODE);
            return true;
        }
        case sl_bt_evt_sm_bonded_id:
            if (evt->data.evt_sm_bonded.connection != conn) {
                return false;
            }
            if (pairing) {
                rec.bonding = evt->data.evt_sm_bonded.bonding;
            }
            if (g == G_ENCRYPT) {
                start_discovery();
            }
            return true;
        case sl_bt_evt_sm_bonding_failed_id:
            if (evt->data.evt_sm_bonding_failed.connection != conn) {
                return false;
            }
            fail(evt->data.evt_sm_bonding_failed.reason);
            return true;
        case sl_bt_evt_connection_parameters_id:
            if (evt->data.evt_connection_parameters.connection != conn) {
                return false;
            }
            /* a reconnection: encrypted with the bond -- on to the HID service */
            if (!pairing && g == G_ENCRYPT && evt->data.evt_connection_parameters.security_mode != sl_bt_connection_mode1_level1) {
                start_discovery();
            }
            return true;
        case sl_bt_evt_gatt_mtu_exchanged_id:
            return evt->data.evt_gatt_mtu_exchanged.connection == conn;
        case sl_bt_evt_gatt_service_id: {
            sl_bt_evt_gatt_service_t *e = &evt->data.evt_gatt_service;
            if (e->connection != conn) {
                return false;
            }
            if (e->uuid.len == 2 && e->uuid.data[0] == 0x12 && e->uuid.data[1] == 0x18) {
                hid_service = e->service;
            }
            return true;
        }
        case sl_bt_evt_gatt_characteristic_id: {
            sl_bt_evt_gatt_characteristic_t *e = &evt->data.evt_gatt_characteristic;
            if (e->connection != conn) {
                return false;
            }
            if (e->uuid.len == 2 && e->uuid.data[1] == 0x2A) {
                if (e->uuid.data[0] == 0x4B) { /* Report Map */
                    map_handle = e->characteristic;
                } else if (e->uuid.data[0] == 0x4D && nrep < MAX_REPORTS) { /* Report */
                    rep[nrep].handle = e->characteristic;
                    rep[nrep].ref = 0;
                    rep[nrep].id = 0;
                    rep[nrep].type = 0;
                    rep[nrep].notify = (e->properties & 0x10) != 0;
                    nrep++;
                }
            }
            return true;
        }
        case sl_bt_evt_gatt_descriptor_id: {
            sl_bt_evt_gatt_descriptor_t *e = &evt->data.evt_gatt_descriptor;
            if (e->connection != conn) {
                return false;
            }
            if (g == G_DESC && e->uuid.len == 2 && e->uuid.data[0] == 0x08 && e->uuid.data[1] == 0x29) {
                rep[cur].ref = e->descriptor; /* Report Reference */
            }
            return true;
        }
        case sl_bt_evt_gatt_descriptor_value_id: {
            sl_bt_evt_gatt_descriptor_value_t *e = &evt->data.evt_gatt_descriptor_value;
            if (e->connection != conn) {
                return false;
            }
            if (g == G_REF && e->descriptor == rep[cur].ref && e->value.len >= 2) {
                rep[cur].id = e->value.data[0];
                rep[cur].type = e->value.data[1];
            }
            return true;
        }
        case sl_bt_evt_gatt_characteristic_value_id: {
            sl_bt_evt_gatt_characteristic_value_t *e = &evt->data.evt_gatt_characteristic_value;
            if (e->connection != conn) {
                return false;
            }
            if (e->att_opcode == sl_bt_gatt_handle_value_notification) {
                if (g == G_DONE) {
                    report_in(e->characteristic, e->value.data, e->value.len);
                }
            } else if (g == G_MAP && e->characteristic == map_handle && e->offset + e->value.len <= sizeof map) {
                memcpy(map + e->offset, e->value.data, e->value.len);
                if (e->offset + e->value.len > map_len) {
                    map_len = (uint16_t)(e->offset + e->value.len);
                }
            }
            return true;
        }
        case sl_bt_evt_gatt_procedure_completed_id:
            if (evt->data.evt_gatt_procedure_completed.connection != conn) {
                return false;
            }
            step(evt->data.evt_gatt_procedure_completed.result);
            return true;
        case sl_bt_evt_connection_closed_id:
            if (evt->data.evt_connection_closed.connection != conn) {
                return false;
            }
            conn = NO_CONN;
            g = G_NONE;
            if (pairing) { /* closed before the HID service was up */
                pairing = false;
                sl_sleeptimer_stop_timer(&pair_timer);
                set_state(KS_FAILED);
            } else if (state != KS_FAILED && state != KS_NOT_FOUND) {
                set_state(have_rec ? KS_PAIRED : KS_NONE);
            }
            return true;
        default:
            return false;
    }
}

/* ---- requests ---- */

uint8_t kbd_request(uint8_t type, const uint8_t *p, size_t len, uint8_t *a, size_t *n) {
    *n = 0;
    switch (type) {
        case RQ_KBD_PAIR: {
            uint32_t s = len >= 1 && p[0] ? p[0] : 15;
            if (scanner_owner == SCAN_LINK) {
                events_log_error("KBD_PAIR: the Link is scanning");
                return ST_STATE;
            }
            close_conn(); /* one keyboard at a time: a new one replaces it */
            rec_forget();
            pairing = true;
            if (!scanner_take(SCAN_KBD, true, 16, 16)) {
                pairing = false;
                return ST_STATE;
            }
            set_state(KS_SEARCHING);
            search_expired = pair_expired = false;
            sl_sleeptimer_start_timer_ms(&search_timer, s * 1000u, search_cb, NULL, 0, 0);
            sl_sleeptimer_start_timer_ms(&pair_timer, PAIR_TIMEOUT_MS, pair_cb, NULL, 0, 0);
            return ST_OK;
        }
        case RQ_KBD_STOP:
            if (pairing) {
                pairing = false;
                sl_sleeptimer_stop_timer(&search_timer);
                sl_sleeptimer_stop_timer(&pair_timer);
                scanner_release(SCAN_KBD);
                close_conn();
                set_state(have_rec ? KS_PAIRED : KS_NONE);
            }
            return ST_OK;
        case RQ_KBD_FORGET:
            pairing = false;
            scanner_release(SCAN_KBD);
            close_conn();
            rec_forget();
            set_state(KS_NONE);
            return ST_OK;
        case RQ_KBD_ENABLE:
            if (len < 1) {
                return ST_BAD_ARGS;
            }
            enabled = p[0] != 0;
            if (!enabled) {
                if (scanner_owner == SCAN_KBD && !pairing) {
                    scanner_release(SCAN_KBD);
                }
                if (!pairing) {
                    close_conn();
                }
            }
            return ST_OK;
        case RQ_KBD_STATUS: {
            size_t i = 0;
            a[i++] = state;
            a[i++] = have_rec ? rec.addr_type : 0;
            for (int k = 5; k >= 0; k--) {
                a[i++] = have_rec ? rec.addr[k] : 0; /* most significant first */
            }
            uint8_t nl = have_rec ? rec.name_len : (uint8_t)strlen(pair_name);
            a[i++] = nl;
            memcpy(a + i, have_rec ? rec.name : pair_name, nl);
            i += nl;
            *n = i;
            return ST_OK;
        }
        default:
            return ST_UNKNOWN_TYPE;
    }
}

bool kbd_pairing(void) { return pairing; }

/* ---- the main loop ---- */

void kbd_tick(void) {
    if (search_expired) {
        search_expired = false;
        if (pairing && state == KS_SEARCHING) {
            pairing = false;
            sl_sleeptimer_stop_timer(&pair_timer);
            scanner_release(SCAN_KBD);
            set_state(KS_NOT_FOUND);
        }
    }
    if (pair_expired) {
        pair_expired = false;
        if (pairing) {
            fail(0xF1FF); /* the code never typed, or discovery stuck */
        }
    }
    /* Background reconnection: a low-duty passive scan (30 ms every
     * 1.28 s, as the dongle's) for the bonded keyboard's advertising, while
     * the Link isn't scanning or connecting. */
    bool want = enabled && have_rec && !pairing && conn == NO_CONN && !app_link_connecting();
    if (want && scanner_owner == SCAN_NONE) {
        scanner_take(SCAN_KBD, false, 2048, 48);
    } else if (!want && scanner_owner == SCAN_KBD && !pairing) {
        scanner_release(SCAN_KBD);
    }
}
