/* btstack_config.h -- BTstack build options for the dongle's BLE link
 * (pico_btstack_ble + pico_btstack_cyw43; the SDK requires the application
 * to supply this file). Based on the SDK's own RP2 port configuration
 * (lib/btstack/port/rp2040-vela-if820/btstack_config.h), cut down to BLE
 * without pairing: the PC-1500 Link doesn't bond (BLE_PROTOCOL.md), and
 * BTstack's flash store can't be written from core0 in this firmware anyway
 * (flash_safe_execute() only pauses core0 -- see main.c). */
#ifndef BTSTACK_CONFIG_H
#define BTSTACK_CONFIG_H

/* Features. Both roles: a PC-1500 scans for and connects to the app or
 * another PC-1500 (central), and advertises for another PC-1500 (P2P,
 * peripheral). */
#define ENABLE_LOG_ERROR
#define ENABLE_PRINTF_HEXDUMP /* the SDK always compiles hci_dump_embedded_stdout.c */
#define ENABLE_LE_CENTRAL
#define ENABLE_LE_PERIPHERAL
#define ENABLE_LE_DATA_LENGTH_EXTENSION

/* Buffers and sizes. The CYW43 transport needs a 4-byte pre-buffer and
 * 4-byte ACL chunk alignment (btstack_hci_transport_cyw43.c checks both). */
#define HCI_OUTGOING_PRE_BUFFER_SIZE 4
#define HCI_ACL_CHUNK_SIZE_ALIGNMENT 4
#define HCI_ACL_PAYLOAD_SIZE (255 + 4)
#define MAX_NR_HCI_CONNECTIONS 1
#define MAX_NR_GATT_CLIENTS 1
#define MAX_NR_L2CAP_CHANNELS 1
#define MAX_NR_L2CAP_SERVICES 1
#define MAX_NR_SM_LOOKUP_ENTRIES 3
#define MAX_NR_WHITELIST_ENTRIES 4
#define MAX_NR_LE_DEVICE_DB_ENTRIES 4

/* LE device DB in TLV on the flash bank (the SDK's btstack_cyw43.c sets
 * that up unconditionally). */
#define NVM_NUM_DEVICE_DB_ENTRIES 4

/* No malloc for BTstack: a fixed-size ATT DB. */
#define MAX_ATT_DB_SIZE 512

/* HAL */
#define HAVE_EMBEDDED_TIME_MS
#define HAVE_ASSERT
#define HCI_RESET_RESEND_TIMEOUT_MS 1000
#define ENABLE_SOFTWARE_AES128

#endif
