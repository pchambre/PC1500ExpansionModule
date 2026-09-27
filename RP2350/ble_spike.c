/* ble_spike.c -- see ble_spike.h.
 *
 * Threading: BTstack runs in the CYW43's async context, whose work happens
 * in a low-priority IRQ on core0 (cyw43_arch_threadsafe_background). The
 * packet handler below runs there; ble_spike_poll() runs in core0's loop
 * and takes the context's lock around every BTstack call. Core1 only reads
 * the counters (plain volatile words -- a torn summary line is harmless in
 * a spike). No printf: stdio is USB CDC and the dongle usually runs without
 * a host attached. */
#include "ble_spike.h"

#include <stdio.h>
#include <string.h>

#include "btstack.h"
#include "flash_layout.h"
#include "mcu_config.h"
#include "pico/btstack_flash_bank.h"
#include "pico/cyw43_arch.h"

static_assert(PICO_FLASH_BANK_TOTAL_SIZE == FLASH_BTSTACK_SIZE, "BTstack store size");

/* The SDK's BTstack flash store goes where flash_layout.h says, not at its
 * default (CMakeLists.txt points pico_flash_bank_get_storage_offset_func
 * here). */
uint32_t ble_flash_bank_offset(void) { return FLASH_BTSTACK_OFFSET; }

#define MAX_SEEN 32
#define NAME_MAX 12

static btstack_packet_callback_registration_t g_hci_cb;
static bool g_powered; /* hci_power_control(ON) sent, since the radio came up */

static bd_addr_t g_seen[MAX_SEEN];
static uint8_t g_seen_count;
static volatile uint32_t g_reports, g_devices;
static volatile bool g_changed;
static char g_last_name[NAME_MAX + 1];

bool ble_spike_wanted(void) { return mcu_config_get(MCU_CONFIG_BLE) != 0; }

/* The advertised (complete or shortened) local name, if any, into `out`. */
static void adv_name(const uint8_t *data, uint8_t len, char *out) {
    ad_context_t ad;
    out[0] = 0;
    for (ad_iterator_init(&ad, len, data); ad_iterator_has_more(&ad); ad_iterator_next(&ad)) {
        uint8_t type = ad_iterator_get_data_type(&ad);
        if (type != BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME && type != BLUETOOTH_DATA_TYPE_SHORTENED_LOCAL_NAME)
            continue;
        uint8_t n = ad_iterator_get_data_len(&ad);
        if (n > NAME_MAX) n = NAME_MAX;
        memcpy(out, ad_iterator_get_data(&ad), n);
        out[n] = 0;
        return;
    }
}

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;
    switch (hci_event_packet_get_type(packet)) {
        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
                gap_set_scan_parameters(0 /* passive */, 0x0030, 0x0030); /* 30ms every 30ms */
                gap_start_scan();
            }
            break;
        case GAP_EVENT_ADVERTISING_REPORT: {
            bd_addr_t addr;
            gap_event_advertising_report_get_address(packet, addr);
            g_reports++;
            for (uint8_t i = 0; i < g_seen_count; i++)
                if (bd_addr_cmp(g_seen[i], addr) == 0) return;
            if (g_seen_count < MAX_SEEN) bd_addr_copy(g_seen[g_seen_count++], addr);
            g_devices++;
            char name[NAME_MAX + 1];
            adv_name(gap_event_advertising_report_get_data(packet),
                     gap_event_advertising_report_get_data_length(packet), name);
            if (name[0]) memcpy(g_last_name, name, sizeof name);
            g_changed = true;
            break;
        }
        default:
            break;
    }
}

void ble_spike_poll(bool radio_up) {
    if (!radio_up) {
        g_powered = false; /* cyw43_arch_deinit() took BTstack down with it */
        return;
    }
    bool want = ble_spike_wanted();
    if (want == g_powered) return;
    async_context_t *ctx = cyw43_arch_async_context();
    async_context_acquire_lock_blocking(ctx);
    if (want) {
        g_hci_cb.callback = &packet_handler;
        hci_add_event_handler(&g_hci_cb); /* no-op if already registered */
        g_seen_count = 0;
        g_reports = g_devices = 0;
        g_last_name[0] = 0;
        hci_power_control(HCI_POWER_ON);
    } else {
        gap_stop_scan();
        hci_power_control(HCI_POWER_OFF);
    }
    async_context_release_lock(ctx);
    g_powered = want;
}

bool ble_spike_take_summary(char *msg, size_t size) {
    if (!g_changed) return false;
    g_changed = false;
    snprintf(msg, size, "BLE %lud %luA %s", (unsigned long)g_devices, (unsigned long)g_reports, g_last_name);
    return true;
}
