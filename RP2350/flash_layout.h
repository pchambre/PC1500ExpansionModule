/* flash_layout.h -- what the firmware keeps at the top of its flash, above
 * the program image. From the end of flash down:
 *
 *   last sector        FNSAVE's function keys      (mcu_store.c, slot 0)
 *   1 sector           MCONF settings              (mcu_config.c)
 *   9 sectors          STSAVE's state: a header page + 0000H-7FFFH
 *                                                  (mcu_store.c, slot 1)
 *   2 sectors          BTstack's key/device store  (pico_btstack_flash_bank)
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
 * pico_flash_bank_get_storage_offset_func (CMakeLists.txt, ble_spike.c).
 * Adding it moved the log ring down 2 sectors. */
#pragma once

#include "hardware/flash.h"

#define FLASH_FNKEYS_OFFSET (PICO_FLASH_SIZE_BYTES - 1 * FLASH_SECTOR_SIZE)
#define FLASH_FNKEYS_SIZE FLASH_SECTOR_SIZE

#define FLASH_CONFIG_OFFSET (PICO_FLASH_SIZE_BYTES - 2 * FLASH_SECTOR_SIZE)

#define FLASH_STATE_SIZE (9 * FLASH_SECTOR_SIZE)
#define FLASH_STATE_OFFSET (FLASH_CONFIG_OFFSET - FLASH_STATE_SIZE)

#define FLASH_BTSTACK_SIZE (2 * FLASH_SECTOR_SIZE)
#define FLASH_BTSTACK_OFFSET (FLASH_STATE_OFFSET - FLASH_BTSTACK_SIZE)

/* the log ring ends here */
#define FLASH_LOG_TOP FLASH_BTSTACK_OFFSET
