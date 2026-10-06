/* flash_layout.h -- what the firmware keeps at the top of its flash, above
 * the program image. From the end of flash down:
 *
 *   last sector        FNSAVE's function keys      (mcu_store.c, slot 0)
 *   1 sector           MCONF settings              (mcu_config.c)
 *   9 sectors          STSAVE's state: a header page + 0000H-7FFFH
 *                                                  (mcu_store.c, slot 1)
 *   2 sectors          BTstack's key/device store  (pico_btstack_flash_bank)
 *   1 sector           the Link's identity and pairings (link_store.c,
 *                      mcu_store.c slot 2)
 *   1 sector           Bluetooth bonds -- the external keyboard's
 *                      (bt_store.c, mcu_store.c slot 3)
 *   1 sector           remembered Wi-Fi networks   (wifi_store.c,
 *                      mcu_store.c slot 4)
 *   LOGSIZE KB         the MCU log ring            (mcu_log.c)
 *
 * Everything is placed relative to PICO_FLASH_SIZE_BYTES, so the same
 * layout works on 2MB and 4MB parts; mcu_log.c keeps the log ring clear of
 * the program image. (Until 2026-09-25 the last sector held the old
 * single-sector log.)
 *
 * The BTstack store (2026-09-27): the SDK's default place for it on RP2350
 * is one sector below the end of flash -- right over MCONF and the top of
 * STSAVE here -- and BTstack erases and rewrites it by itself. So it gets
 * its own two sectors, handed to the SDK through
 * pico_flash_bank_get_storage_offset_func (CMakeLists.txt, ble_link.c).
 * Adding it moved the log ring down 2 sectors.
 *
 * The Link's pairings (2026-10-03, BLE_PROTOCOL.md sec.7): one sector more,
 * which moved the log ring down another.
 *
 * Bluetooth bonds (2026-10-04, the external keyboard): BTstack's own store
 * above can't be used for them -- it writes flash from core0, BTstack's
 * core -- so bt_store.c keeps them in RAM and core1 saves them here. Not in
 * the BTstack store's sectors: the SDK erases those at start-up whenever
 * they don't hold its own format. One sector more, the log ring down
 * another.
 *
 * Remembered Wi-Fi networks (2026-10-06, the WF* keywords): one sector
 * more, the log ring down another. */
#pragma once

#include "hardware/flash.h"

#define FLASH_FNKEYS_OFFSET (PICO_FLASH_SIZE_BYTES - 1 * FLASH_SECTOR_SIZE)
#define FLASH_FNKEYS_SIZE FLASH_SECTOR_SIZE

#define FLASH_CONFIG_OFFSET (PICO_FLASH_SIZE_BYTES - 2 * FLASH_SECTOR_SIZE)

#define FLASH_STATE_SIZE (9 * FLASH_SECTOR_SIZE)
#define FLASH_STATE_OFFSET (FLASH_CONFIG_OFFSET - FLASH_STATE_SIZE)

#define FLASH_BTSTACK_SIZE (2 * FLASH_SECTOR_SIZE)
#define FLASH_BTSTACK_OFFSET (FLASH_STATE_OFFSET - FLASH_BTSTACK_SIZE)

#define FLASH_LINK_SIZE FLASH_SECTOR_SIZE
#define FLASH_LINK_OFFSET (FLASH_BTSTACK_OFFSET - FLASH_LINK_SIZE)

#define FLASH_BTBONDS_SIZE FLASH_SECTOR_SIZE
#define FLASH_BTBONDS_OFFSET (FLASH_LINK_OFFSET - FLASH_BTBONDS_SIZE)

#define FLASH_WIFI_SIZE FLASH_SECTOR_SIZE
#define FLASH_WIFI_OFFSET (FLASH_BTBONDS_OFFSET - FLASH_WIFI_SIZE)

/* the log ring ends here */
#define FLASH_LOG_TOP FLASH_WIFI_OFFSET
