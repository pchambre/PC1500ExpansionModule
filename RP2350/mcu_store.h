/* mcu_store.h -- flash stores for FNSAVE/FNLOAD (the function keys) and
 * STSAVE/STLOAD (0000H-7FFFH), 2026-09-25. See flash_layout.h for where
 * they live and pc_exp.h's EXP_COMMAND_STORE_* for how keywords.c uses
 * them. One set of each for now; SD-card save sets can come later. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Empties a slot (EXP_STORE_SLOT_*): erases its flash. */
bool mcu_store_erase(uint8_t slot);

/* Programs `len` bytes at `offset` (a multiple of 256) into a slot's erased
 * flash. False for a bad slot or a range outside it. */
bool mcu_store_write(uint8_t slot, uint32_t offset, const uint8_t *data, uint32_t len);

/* Copies `len` bytes at `offset` out of a slot. */
bool mcu_store_read(uint8_t slot, uint32_t offset, uint8_t *data, uint32_t len);
