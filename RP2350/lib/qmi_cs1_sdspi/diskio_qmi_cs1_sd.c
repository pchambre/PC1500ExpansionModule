/* diskio_qmi_cs1_sd.c
 *
 * FatFs diskio.h glue for the single SD card driven by qmi_cs1_sd.h.
 * Fixed to physical drive 0 -- this project (and this module generally)
 * only ever has one card. Every write is synchronous (sd_write_sector
 * only returns after the card reports done), so CTRL_SYNC is a no-op.
 */
#include "diskio.h"
#include "qmi_cs1_sd.h"

#include <string.h>

static bool g_initialized = false;

DSTATUS disk_status(BYTE pdrv) {
    if (pdrv != 0) return STA_NOINIT;
    return g_initialized ? 0 : STA_NOINIT;
}

/* Caller (main.c, on core0 during startup) must have already run
 * qmi_cs1_spi_init() before this is invoked via f_mount()'s own lazy
 * disk_initialize() call -- this function only runs the SD-level
 * protocol init (sd_init()), not the QMI/GPIO-level setup, since that's
 * a one-time board-level concern this module doesn't own the timing of. */
DSTATUS disk_initialize(BYTE pdrv) {
    if (pdrv != 0) return STA_NOINIT;
    /* 125MHz is this board's nominal clk_sys (Pico SDK default); 12.5MHz
     * is a conservative SD SPI operating clock well within spec for a
     * first bring-up (spec allows up to 25MHz in default speed mode). */
    g_initialized = sd_init(125000000u, 12500000u);
    return g_initialized ? 0 : STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv != 0) return RES_PARERR;
    if (!g_initialized) return RES_NOTRDY;
    for (UINT i = 0; i < count; i++) {
        if (!sd_read_sector((uint32_t)sector + i, buff + (size_t)i * 512)) return RES_ERROR;
    }
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv != 0) return RES_PARERR;
    if (!g_initialized) return RES_NOTRDY;
    for (UINT i = 0; i < count; i++) {
        if (!sd_write_sector((uint32_t)sector + i, buff + (size_t)i * 512)) return RES_ERROR;
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    if (pdrv != 0) return RES_PARERR;
    if (!g_initialized) return RES_NOTRDY;
    switch (cmd) {
        case CTRL_SYNC:
            return RES_OK; /* every write already completes synchronously */
        case GET_SECTOR_COUNT:
            *(LBA_t *)buff = (LBA_t)sd_get_sector_count();
            return RES_OK;
        case GET_SECTOR_SIZE:
            *(WORD *)buff = 512;
            return RES_OK;
        case GET_BLOCK_SIZE:
            /* Erase block size in sectors -- not read from the card's
             * SSR/SD-Status register here (that needs ACMD13, unimplemented);
             * 128 sectors (64KiB) is a commonly-used safe default for
             * f_mkfs's own allocation-unit sizing. */
            *(DWORD *)buff = 128;
            return RES_OK;
        default:
            return RES_PARERR;
    }
}
