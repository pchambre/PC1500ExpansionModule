/* app.c -- the PC-1500 internal card's BGM220S firmware (2026-10-12).
 *
 * The module does the radio work and passes Link frames through unchanged;
 * the Link protocol itself stays on the RP2354B (../RP2350/MODULE_PROTOCOL.md
 * sec.1). This first version is the advertiser side: the Link service,
 * advertising under the name, one peer connecting, frames both ways.
 * Scanning and connecting (the connector side) and the keyboard come next.
 *
 * Everything runs in the main loop: hostlink_poll() for the UART, and the
 * Bluetooth stack's events through sl_bt_on_event(). */
#include <stdbool.h>
#include <string.h>

#include "app_assert.h"
#include "em_gpio.h"
#include "events.h"
#include "gatt_db.h"
#include "gpiointerrupt.h"
#include "hostlink.h"
#include "module_config.h"
#include "sl_bt_api.h"
#include "sl_iostream_init_usart_instances.h"
#include "sl_iostream_uart.h"
#include "sl_main_init.h"

/* The Link service UUID, little-endian as it goes in advertising data. */
static const uint8_t kLinkUuid[16] = {0xd0, 0xae, 0x37, 0x0a, 0xdb, 0x7c, 0x3d, 0xb6,
                                      0xab, 0x40, 0xa3, 0x92, 0x01, 0x00, 0x1f, 0xc3};

#define NO_CONN 0xFF

static uint8_t adv_set = 0xFF;
static bool adv_wanted;
static char name[NAME_MAX + 1] = "PC-1500";
static uint8_t link_conn = NO_CONN; /* the Link's connection, if any */
static uint16_t link_mtu = 23;
static bool link_up;                /* LINK_UP sent */
static volatile bool wake_changed;

/* ---- MODULE_WAKE: a session keeps the UART receiving (sec.4) ---- */

static void wake_isr(uint8_t pin) {
    (void)pin;
    wake_changed = true;
}

static void session_update(void) {
    bool on = GPIO_PinInGet(MODULE_WAKE_PORT, MODULE_WAKE_PIN);
    /* Outside a session the module may sleep in EM2, where the UART can't
     * receive; the MCU only sends inside one. */
    sl_iostream_uart_set_rx_energy_mode_restriction(sl_iostream_uart_vcom_handle, on);
}

/* ---- advertising ---- */

static void adv_apply(void) {
    sl_status_t sc;
    if (adv_set == 0xFF) {
        return; /* not booted yet: system_boot does it */
    }
    sl_bt_advertiser_stop(adv_set);
    if (!adv_wanted || link_conn != NO_CONN) {
        return;
    }
    uint8_t ad[3 + 18], sr[2 + NAME_MAX];
    size_t n = strlen(name);
    ad[0] = 2, ad[1] = 0x01, ad[2] = 0x06;                   /* flags: LE general, no BR/EDR */
    ad[3] = 17, ad[4] = 0x07, memcpy(ad + 5, kLinkUuid, 16); /* complete 128-bit UUIDs */
    sr[0] = (uint8_t)(n + 1), sr[1] = 0x09, memcpy(sr + 2, name, n); /* complete local name */
    sc = sl_bt_legacy_advertiser_set_data(adv_set, sl_bt_advertiser_advertising_data_packet, sizeof ad, ad);
    app_assert_status(sc);
    sc = sl_bt_legacy_advertiser_set_data(adv_set, sl_bt_advertiser_scan_response_packet, n + 2, sr);
    app_assert_status(sc);
    sc = sl_bt_advertiser_set_timing(adv_set, 160, 160, 0, 0); /* 100 ms */
    app_assert_status(sc);
    sc = sl_bt_legacy_advertiser_start(adv_set, sl_bt_legacy_advertiser_connectable);
    app_assert_status(sc);
}

/* ---- requests (MODULE_PROTOCOL.md sec.6) ---- */

static uint8_t do_hello(uint8_t *a, size_t *n) {
    bd_addr addr;
    uint8_t type;
    size_t i = 0;
    a[i++] = MODULE_PROTOCOL_VERSION;
    a[i++] = (uint8_t)strlen(MODULE_FW_VERSION);
    memcpy(a + i, MODULE_FW_VERSION, strlen(MODULE_FW_VERSION));
    i += strlen(MODULE_FW_VERSION);
    if (sl_bt_system_get_identity_address(&addr, &type) != SL_STATUS_OK) {
        memset(addr.addr, 0, 6);
    }
    for (int k = 5; k >= 0; k--) {
        a[i++] = addr.addr[k]; /* most significant byte first, as it's printed */
    }
    a[i++] = (uint8_t)FRAME_MAX_PAYLOAD;
    a[i++] = (uint8_t)(FRAME_MAX_PAYLOAD >> 8);
    *n = i;
    return ST_OK;
}

static uint8_t do_set_name(const uint8_t *p, size_t len) {
    if (len < 1 || p[0] > NAME_MAX || p[0] == 0 || len != (size_t)p[0] + 1) {
        return ST_BAD_ARGS;
    }
    memcpy(name, p + 1, p[0]);
    name[p[0]] = 0;
    sl_bt_gatt_server_write_attribute_value(gattdb_device_name, 0, p[0], p + 1);
    adv_apply(); /* the scan response carries it */
    return ST_OK;
}

static uint8_t do_link_send(const uint8_t *p, size_t len) {
    if (link_conn == NO_CONN || !link_up) {
        return ST_NOT_CONNECTED;
    }
    if (len == 0 || len > (size_t)link_mtu - 3) {
        return ST_BAD_ARGS;
    }
    sl_status_t sc = sl_bt_gatt_server_send_notification(link_conn, gattdb_link_tx, len, p);
    return sc == SL_STATUS_OK ? ST_OK : sc == SL_STATUS_NO_MORE_RESOURCE ? ST_BUSY : ST_FAILED;
}

uint8_t hostlink_handle(uint8_t type, const uint8_t *p, size_t len, uint8_t *a, size_t *n) {
    *n = 0;
    switch (type) {
        case RQ_HELLO:
            return do_hello(a, n);
        case RQ_POLL:
            *n = events_pop(a, FRAME_MAX_PAYLOAD - 1);
            return ST_OK;
        case RQ_SET_NAME:
            return do_set_name(p, len);
        case RQ_ADV_START:
            adv_wanted = true;
            adv_apply();
            return ST_OK;
        case RQ_ADV_STOP:
            adv_wanted = false;
            adv_apply();
            return ST_OK;
        case RQ_DISCONNECT:
            if (link_conn == NO_CONN) {
                return ST_NOT_CONNECTED;
            }
            sl_bt_connection_close(link_conn);
            return ST_OK; /* LINK_DOWN follows */
        case RQ_LINK_SEND:
            return do_link_send(p, len);
        default:
            return ST_UNKNOWN_TYPE;
    }
}

/* ---- the Bluetooth stack ---- */

static void link_event(uint8_t type, uint16_t reason) {
    uint8_t b[2] = {(uint8_t)reason, (uint8_t)(reason >> 8)};
    events_push(type, b, 2);
}

void sl_bt_on_event(sl_bt_msg_t *evt) {
    switch (SL_BT_MSG_ID(evt->header)) {
        case sl_bt_evt_system_boot_id: {
            sl_status_t sc = sl_bt_advertiser_create_set(&adv_set);
            app_assert_status(sc);
            sl_bt_gatt_server_write_attribute_value(gattdb_device_name, 0, strlen(name), (const uint8_t *)name);
            sl_bt_gatt_server_set_max_mtu(LINK_FRAME_MAX + 3, &link_mtu);
            link_mtu = 23;
            adv_apply();
            events_log("MODULE BOOT %s", MODULE_FW_VERSION);
            break;
        }
        case sl_bt_evt_connection_opened_id: {
            sl_bt_evt_connection_opened_t *e = &evt->data.evt_connection_opened;
            if (link_conn == NO_CONN && e->role == sl_bt_connection_role_peripheral) {
                link_conn = e->connection; /* a peer connected to our advertising */
                link_mtu = 23;
                link_up = false;
            }
            break;
        }
        case sl_bt_evt_gatt_mtu_exchanged_id:
            if (evt->data.evt_gatt_mtu_exchanged.connection == link_conn) {
                link_mtu = evt->data.evt_gatt_mtu_exchanged.mtu;
            }
            break;
        case sl_bt_evt_gatt_server_characteristic_status_id: {
            sl_bt_evt_gatt_server_characteristic_status_t *e = &evt->data.evt_gatt_server_characteristic_status;
            if (e->connection == link_conn && e->characteristic == gattdb_link_tx &&
                e->status_flags == sl_bt_gatt_server_client_config) {
                bool on = (e->client_config_flags & sl_bt_gatt_notification) != 0;
                if (on && !link_up) {
                    /* The peer subscribed: the link is up (sec.7). */
                    bd_addr addr;
                    uint8_t at = 0, b[1 + 1 + 6 + 2];
                    memset(&addr, 0, sizeof addr);
                    sl_bt_connection_get_remote_address(link_conn, &addr, &at);
                    b[0] = 1; /* they connected */
                    b[1] = at;
                    for (int k = 0; k < 6; k++) {
                        b[2 + k] = addr.addr[5 - k];
                    }
                    b[8] = (uint8_t)link_mtu;
                    b[9] = (uint8_t)(link_mtu >> 8);
                    link_up = true;
                    events_push(EV_LINK_UP, b, sizeof b);
                }
            }
            break;
        }
        case sl_bt_evt_gatt_server_attribute_value_id: {
            sl_bt_evt_gatt_server_attribute_value_t *e = &evt->data.evt_gatt_server_attribute_value;
            if (e->connection == link_conn && e->attribute == gattdb_link_rx && e->value.len) {
                events_push(EV_LINK_RX, e->value.data, e->value.len);
            }
            break;
        }
        case sl_bt_evt_connection_closed_id: {
            sl_bt_evt_connection_closed_t *e = &evt->data.evt_connection_closed;
            if (e->connection == link_conn) {
                link_conn = NO_CONN;
                link_up = false;
                link_event(EV_LINK_DOWN, e->reason);
                adv_apply(); /* advertise again if the MCU still wants it */
            }
            break;
        }
        default:
            break;
    }
}

/* ---- start-up and the main loop ---- */

void app_init(void) {
    events_init();
    hostlink_init();
    GPIO_PinModeSet(MODULE_WAKE_PORT, MODULE_WAKE_PIN, gpioModeInputPull, 1);
    GPIOINT_Init();
    GPIOINT_CallbackRegister(MODULE_WAKE_PIN, wake_isr);
    GPIO_ExtIntConfig(MODULE_WAKE_PORT, MODULE_WAKE_PIN, MODULE_WAKE_PIN, true, true, true);
    /* No RTOS: sl_iostream_read() doesn't block (its set_read_block()
     * exists only with a kernel -- don't call it). */
    session_update();
}

void app_process_action(void) {
    if (wake_changed) {
        wake_changed = false;
        session_update();
    }
    hostlink_poll();
}
