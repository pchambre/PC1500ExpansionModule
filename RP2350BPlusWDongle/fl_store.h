/* fl_store.h -- the FL store (2026-11-02): named values in a bounded
 * region of the MCU's flash, for programs on the PC-1500 -- BASIC with
 * the FL* keywords (keywords.c), machine code with the wire commands
 * (pc_exp.h EXP_COMMAND_FL_*). ZIP/1500 keeps its last game there, and
 * its console its word wrap (ZWRAP, zterm.h).
 *
 * A value is a number or a string, kept as the value chunk SDPRINT writes
 * ('N' + the 8-byte BASIC number, or 'S' + length + up to 80 characters).
 * A key is 1-16 printable characters, no '"' or '='.
 *
 * The region is two flash sectors used in turn: the whole store (at most
 * FL_CAPACITY bytes of records) is written to the other sector at each
 * change, with a sequence number and a CRC, so a power cut in the middle
 * leaves the previous copy. A record is [key len][key][chunk].
 *
 * Portable C, no allocation: the firmware (mcu_store.c's slot) and
 * pc1500emu's mock (a vector) give it the region's flash. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FL_SECTOR 4096
#define FL_HEADER 16
#define FL_CAPACITY (FL_SECTOR - FL_HEADER) /* bytes of records */
#define FL_KEY_MAX 16
#define FL_STRING_MAX 80

/* The region: two FL_SECTOR sectors at offsets 0 and FL_SECTOR. */
typedef struct {
    void *ctx;
    bool (*read)(void *ctx, uint32_t offset, uint8_t *data, uint32_t len);
    bool (*erase)(void *ctx, uint32_t offset); /* one sector */
    bool (*write)(void *ctx, uint32_t offset, const uint8_t *data, uint32_t len); /* whole sector */
} fl_flash_t;

/* One EXP_COMMAND_FL_* with its arguments at window[0..]; the answer is
 * written back there. Returns the EXP_STATUS_*. The store is read from
 * flash at the first command. */
uint8_t fl_command(const fl_flash_t *flash, uint8_t command, uint8_t *window);

/* A setting kept as a number (FLSET "key",n -- 2026-11-03, ZIP's ZWRAP):
 * true if it's nonzero; `dflt` if there's none, or it's a string. */
bool fl_get_flag(const fl_flash_t *flash, const char *key, bool dflt);

/* ...stored as 1 or 0. False if it couldn't be. */
bool fl_set_flag(const fl_flash_t *flash, const char *key, bool on);

/* Forget what was read: the next command reads the flash again (a test's
 * cold boot). */
void fl_forget(void);

#ifdef __cplusplus
}
#endif
