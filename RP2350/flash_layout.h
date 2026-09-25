/* flash_layout.h -- what the firmware keeps at the top of its flash, above
 * the program image. From the end of flash down:
 *
 *   last sector        FNSAVE's function keys      (mcu_store.c, slot 0)
 *   1 sector           MCONF settings              (mcu_config.c)
 *   9 sectors          STSAVE's state: a header page + 0000H-7FFFH
 *                                                  (mcu_store.c, slot 1)
 *   LOGSIZE KB         the MCU log ring            (mcu_log.c)
 *
 * Everything is placed relative to PICO_FLASH_SIZE_BYTES, so the same
 * layout works on 2MB and 4MB parts; mcu_log.c keeps the log ring clear of
 * the program image. (Until 2026-09-25 the last sector held the old
 * single-sector log.) */
#pragma once

#include "hardware/flash.h"

#define FLASH_FNKEYS_OFFSET (PICO_FLASH_SIZE_BYTES - 1 * FLASH_SECTOR_SIZE)
#define FLASH_FNKEYS_SIZE FLASH_SECTOR_SIZE

#define FLASH_CONFIG_OFFSET (PICO_FLASH_SIZE_BYTES - 2 * FLASH_SECTOR_SIZE)

#define FLASH_STATE_SIZE (9 * FLASH_SECTOR_SIZE)
#define FLASH_STATE_OFFSET (FLASH_CONFIG_OFFSET - FLASH_STATE_SIZE)

/* the log ring ends here */
#define FLASH_LOG_TOP FLASH_STATE_OFFSET
