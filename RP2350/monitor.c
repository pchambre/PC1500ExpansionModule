/* monitor.c
 *
 * Ported from Design01_NonDMA_8K_PV_Swap.cydsn/main.c -- see that file
 * and this repo's plan history for the full background. Two halves,
 * same structure as the original:
 *
 *   1. monitor_run()'s bus loop: on the GreenPAK's read-trigger
 *      (CS && Read && OE), drive the data pins from buffer[page][laddress];
 *      on the write-trigger (CS && Write), latch data into the buffer,
 *      or call DoCommand() if the write landed on the instruction
 *      address. Direct SIO register reads throughout (sio_hw->gpio_in/
 *      gpio_out/gpio_oe), not per-pin gpio_get()/gpio_put() calls --
 *      those go through more machinery than a ~2us budget can afford.
 *   2. DoCommand(): the same ~30-case EXP_COMMAND_* switch, translated
 *      case-by-case from SEGGER emFile's FS_* API to FatFs's f_* API.
 *
 * RAM RESIDENCY -- deliberately NOT applied here, and here's why (this
 * matters, don't "fix" it without reading this first): DoCommand() sets
 * EXP_STATUS_BUSY in `buffer` before starting SD work and blocks
 * synchronously for the whole operation, exactly like the PSoC5
 * original -- the outer bus loop does not resume servicing other bus
 * cycles until DoCommand() returns, on either platform. The PSoC5
 * design already accepts that any bus read landing on the status byte
 * *while DoCommand() is running* sees whatever was last latched onto the
 * physical data pins before the call, not a live answer -- the ROM-side
 * driver's own polling loop only ever observes a fresh, correct value
 * once DoCommand() returns and the loop resumes. That's an existing
 * property of the protocol, not something this port changes, so
 * DoCommand() and this file's loop don't need __not_in_flash_func the
 * way qmi_cs1_spi.c/qmi_cs1_sd.c do -- those needed it to keep their own
 * internal per-byte transfer loops from deadlocking against their own
 * open QMI transaction (see their own top comments), a QMI-specific
 * hazard the original PSoC5 firmware never had to think about at all.
 */
#include "monitor.h"

#include <string.h>

#include "hardware/structs/sio.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"

#include "ff.h"
#include "board_pins.h"
#include "pc_exp.h"

/* ---- Shared 8K data window: pages 0-7 (2K live data window) plus
 * pages 8-31 (6K ROM region, only actually driven through this buffer
 * while Control_Mode_Control-equivalent GreenPAK state selects
 * ROM_FROM_MCU during the boot copy -- see PC_EXP.h's own top comment).
 * ---- */
static uint8_t buffer[32][256];

static FIL currentFile;
static bool currentFileOpen = false;
static uint8_t currentFileStatus = EXP_SD_FILE_STATUS_CLOSED;
static uint32_t fileEnd = 0;
static char currentFileName[64];

static FIL channelFile[EXP_MAX_SD_CHANNELS];
static bool channelOpen[EXP_MAX_SD_CHANNELS];
static char channelName[EXP_MAX_SD_CHANNELS][EXP_PATH_ARG_LEN + 1];
static uint32_t channelReadPos[EXP_MAX_SD_CHANNELS];

/* ============================================================
 * Buffer/string helpers -- filesystem-API-agnostic, ported verbatim
 * from main.c. (DataFromBuffer is not ported: it's dead code in the
 * original too, referenced only from a commented-out call.)
 * ============================================================ */

static void LongToBuffer(uint8_t buf[16][256], int8_t page, int8_t start, uint32_t value) {
    buf[page][start] = (uint8_t)((value >> 24) & 0xFF);
    buf[page][start + 1] = (uint8_t)((value >> 16) & 0xFF);
    buf[page][start + 2] = (uint8_t)((value >> 8) & 0xFF);
    buf[page][start + 3] = (uint8_t)(value & 0xFF);
}

static void StringFromBuffer(uint8_t buf[16][256], uint8_t page, uint8_t start, uint16_t length,
                              char result[]) {
    const uint8_t *b = &buf[page][start];
    for (uint16_t i = 0; i < length; i++) result[i] = (char)*b++;
    result[length] = 0;
}

static void FormatSizeText(uint32_t value, uint8_t result[], uint8_t width) {
    uint8_t i;
    for (i = 0; i < width; i++) result[i] = ' ';
    i = width;
    do {
        i--;
        result[i] = (uint8_t)('0' + (value % 10));
        value /= 10;
    } while (value != 0 && i > 0);
}

static uint8_t WriteDecimal(uint32_t value, uint8_t result[]) {
    uint8_t buf[10];
    uint8_t n = 0;
    uint8_t i;
    if (value == 0) {
        result[0] = '0';
        return 1;
    }
    while (value != 0) {
        buf[n++] = (uint8_t)('0' + (value % 10));
        value /= 10;
    }
    for (i = 0; i < n; i++) result[i] = buf[n - 1 - i];
    return n;
}

static void FormatSummaryLine(uint16_t count, uint32_t totalBytes, uint32_t freeBytes,
                               uint8_t result[], uint8_t width) {
    char temp[48];
    uint8_t pos = 0;
    uint8_t i;
    pos = (uint8_t)(pos + WriteDecimal(count, (uint8_t *)(temp + pos)));
    temp[pos++] = ' ';
    temp[pos++] = 'F';
    temp[pos++] = 'I';
    temp[pos++] = 'L';
    temp[pos++] = 'E';
    temp[pos++] = 'S';
    temp[pos++] = ' ';
    pos = (uint8_t)(pos + WriteDecimal(totalBytes, (uint8_t *)(temp + pos)));
    temp[pos++] = 'B';
    temp[pos++] = ' ';
    pos = (uint8_t)(pos + WriteDecimal(freeBytes, (uint8_t *)(temp + pos)));
    temp[pos++] = 'F';
    for (i = 0; i < width; i++) result[i] = (i < pos) ? (uint8_t)temp[i] : ' ';
}

static void NormalizeSdPathFromFs(const char *raw, char *out, uint8_t outSize) {
    const char *colon = strchr(raw, ':');
    const char *src = colon ? colon + 1 : raw;
    uint8_t n = 0;
    out[n++] = '/';
    for (; *src != 0 && n < outSize - 1; src++) {
        char c = (*src == '\\') ? '/' : *src;
        if (n == 1 && c == '/') continue;
        out[n++] = c;
    }
    out[n] = 0;
}

static void ConvertSdPathToFsSeparators(char *path) {
    for (; *path != 0; path++)
        if (*path == '/') *path = '\\';
}

static void ConvertPlusToTilde(char *name) {
    for (; *name != 0; name++)
        if (*name == '+') *name = '~';
}

static void ConvertTildeToPlus(char *name) {
    for (; *name != 0; name++)
        if (*name == '~') *name = '+';
}

static void PrepareFsName(const char *raw, char *out, uint8_t outSize) {
    strncpy(out, raw, outSize - 1);
    out[outSize - 1] = 0;
    ConvertPlusToTilde(out);
    ConvertSdPathToFsSeparators(out);
}

static const char *GetBasename(const char *path) {
    const char *base = path;
    for (const char *p = path; *p != 0; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    return base;
}

/* FatFs equivalent of FS_GetFileAttributes(path) & FS_ATTR_DIRECTORY --
 * f_stat() failing (file doesn't exist) is treated as "not a directory",
 * same as the original's implicit behavior when the emFile call fails. */
static bool IsDirectory(const char *path) {
    FILINFO fno;
    if (f_stat(path, &fno) != FR_OK) return false;
    return (fno.fattrib & AM_DIR) != 0;
}

static void ResolveCopyOrMoveDestination(const char *srcBasename, const char *destArg, char *out,
                                          uint8_t outSize) {
    char converted[EXP_PATH_ARG_LEN + 1];
    PrepareFsName(destArg, converted, sizeof(converted));
    if (IsDirectory(converted)) {
        uint8_t n = 0;
        for (; converted[n] != 0 && n < outSize - 1; n++) out[n] = converted[n];
        if (n > 0 && out[n - 1] != '\\' && n < outSize - 1) out[n++] = '\\';
        uint8_t i = 0;
        while (srcBasename[i] != 0 && n < outSize - 1) out[n++] = srcBasename[i++];
        out[n] = 0;
    } else {
        strncpy(out, converted, outSize - 1);
        out[outSize - 1] = 0;
    }
}

/* FS_CopyFile has no FatFs equivalent -- manual read/write-loop copy. */
static bool CopyFileFatFs(const char *srcPath, const char *destPath) {
    FIL src, dst;
    if (f_open(&src, srcPath, FA_READ) != FR_OK) return false;
    if (f_open(&dst, destPath, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
        f_close(&src);
        return false;
    }
    uint8_t chunk[512];
    UINT br, bw;
    FRESULT fr = FR_OK;
    for (;;) {
        fr = f_read(&src, chunk, sizeof(chunk), &br);
        if (fr != FR_OK || br == 0) break;
        fr = f_write(&dst, chunk, br, &bw);
        if (fr != FR_OK || bw != br) {
            fr = FR_DISK_ERR;
            break;
        }
        if (br < sizeof(chunk)) break; /* short read == end of file */
    }
    f_close(&src);
    f_close(&dst);
    return fr == FR_OK;
}

/* FS_FSeek(file, offset, FS_SEEK_END) equivalent -- FatFs's f_lseek only
 * takes an absolute offset, so "seek to end" means seek to f_size(). */
static void SeekToEnd(FIL *fp) { f_lseek(fp, f_size(fp)); }

/* FS_GetVolumeFreeSpace/FS_GetVolumeSize equivalent, via the standard
 * FatFs idiom (f_getfree + fs->csize + sector size), not emFile's
 * single-call equivalents. */
static void GetVolumeSpace(uint32_t *totalBytes, uint32_t *freeBytes) {
    DWORD freeClusters = 0;
    FATFS *fs = NULL;
    if (f_getfree("", &freeClusters, &fs) != FR_OK || fs == NULL) {
        *totalBytes = 0;
        *freeBytes = 0;
        return;
    }
    *freeBytes = (uint32_t)freeClusters * fs->csize * FF_MAX_SS;
    *totalBytes = (uint32_t)(fs->n_fatent - 2) * fs->csize * FF_MAX_SS;
}

static void WriteStatus(uint8_t buf[16][256], uint8_t status) {
    buf[EXP_INSTRUCTION_PAGE][EXP_INSTRUCTION_ADDRESS] = status;
}

/* ============================================================
 * DoCommand -- ~30-case switch, ported from main.c's DoCommand(),
 * FS_* -> f_*. Case-by-case commentary kept where the FatFs mapping
 * isn't 1:1; see this file's own header and RP2350/README.md for the
 * general notes (FS_RmDir/f_unlink-on-a-directory, FS_ChDir/FF_FS_RPATH,
 * FS_CopyFile/CopyFileFatFs, volume free/total via f_getfree).
 * ============================================================ */
static void DoCommand(uint8_t req, uint8_t buf[16][256]) {
    WriteStatus(buf, EXP_STATUS_BUSY);
    switch (req) {
        case EXP_COMMAND_ROM_FROM_SRAM: {
            /* GreenPAK mode-select, over the I2C link -- board_pins.h's
             * PIN_GREENPAK_SDA/SCL. Not yet implemented: this project's
             * GreenPAK comms protocol itself isn't designed yet (see
             * plan history) -- this case is a placeholder matching the
             * original's own trivial one-line body until that exists. */
            WriteStatus(buf, EXP_STATUS_SUCCESS);
            break;
        }
        case EXP_COMMAND_ROM_FROM_MCU: {
            WriteStatus(buf, EXP_STATUS_SUCCESS);
            break;
        }
        case EXP_COMMAND_GET_SD_FREE_SPACE: {
            WriteStatus(buf, EXP_STATUS_BUSY);
            uint32_t total, free_;
            GetVolumeSpace(&total, &free_);
            LongToBuffer(buf, EXP_BUFFER_START_PAGE, EXP_BUFFER_START_ADDRESS, free_);
            WriteStatus(buf, EXP_STATUS_SUCCESS);
            break;
        }
        case EXP_COMMAND_GET_SD_VOLUME_SIZE: {
            WriteStatus(buf, EXP_STATUS_BUSY);
            uint32_t total, free_;
            GetVolumeSpace(&total, &free_);
            LongToBuffer(buf, EXP_BUFFER_START_PAGE, EXP_BUFFER_START_ADDRESS, total);
            WriteStatus(buf, EXP_STATUS_SUCCESS);
            break;
        }
        case EXP_COMMAND_GET_SD_FILE_SIZE: {
            if (!currentFileOpen) {
                WriteStatus(buf, EXP_STATUS_ERROR);
                break;
            }
            WriteStatus(buf, EXP_STATUS_BUSY);
            uint32_t fileSize = (uint32_t)f_size(&currentFile);
            LongToBuffer(buf, EXP_BUFFER_START_PAGE, EXP_BUFFER_START_ADDRESS, fileSize);
            WriteStatus(buf, EXP_STATUS_SUCCESS);
            break;
        }
        case EXP_COMMAND_READ_SD_VOLUME_LABEL: {
            WriteStatus(buf, EXP_STATUS_BUSY);
            uint16_t volumeNameLen = buf[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS] * 256 +
                                      buf[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS + 1];
            if (volumeNameLen == 0) {
                WriteStatus(buf, EXP_STATUS_ERROR);
                break;
            }
            char volumeName[EXP_PATH_ARG_LEN + 1];
            if (volumeNameLen > EXP_PATH_ARG_LEN) volumeNameLen = EXP_PATH_ARG_LEN;
            StringFromBuffer(buf, EXP_BUFFER_START_PAGE, EXP_BUFFER_START_ADDRESS + 2,
                              volumeNameLen, volumeName);
            char label[34];
            if (f_getlabel(volumeName, label, NULL) != FR_OK) {
                WriteStatus(buf, EXP_STATUS_ERROR);
                break;
            }
            uint8_t labelLen = (uint8_t)strlen(label);
            buf[EXP_SCRATCH_PAGE][0] = labelLen;
            for (uint8_t i = 0; i < labelLen; i++) buf[EXP_SCRATCH_PAGE][i + 1] = label[i];
            WriteStatus(buf, EXP_STATUS_SUCCESS);
            break;
        }
        case EXP_COMMAND_GET_SD_FILE_NAME: {
            if (!currentFileOpen) {
                WriteStatus(buf, EXP_STATUS_ERROR);
                break;
            }
            WriteStatus(buf, EXP_STATUS_BUSY);
            uint8_t nameLen = (uint8_t)strlen(currentFileName);
            buf[EXP_SCRATCH_PAGE][0] = nameLen;
            for (uint8_t i = 0; i < nameLen; i++) buf[EXP_SCRATCH_PAGE][i + 1] = currentFileName[i];
            WriteStatus(buf, EXP_STATUS_SUCCESS);
            break;
        }
        case EXP_COMMAND_REMOVE_SD_FILE: {
            WriteStatus(buf, EXP_STATUS_BUSY);
            uint16_t rmNameLen = buf[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS] * 256 +
                                  buf[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS + 1];
            if (rmNameLen == 0) {
                WriteStatus(buf, EXP_STATUS_ERROR);
                break;
            }
            char rmName[EXP_PATH_ARG_LEN + 1];
            if (rmNameLen > EXP_PATH_ARG_LEN) rmNameLen = EXP_PATH_ARG_LEN;
            StringFromBuffer(buf, EXP_BUFFER_START_PAGE, EXP_BUFFER_START_ADDRESS + 2, rmNameLen,
                              rmName);
            char rmFsName[EXP_PATH_ARG_LEN + 1];
            PrepareFsName(rmName, rmFsName, sizeof(rmFsName));
            /* SDRM must only ever delete a file, never a directory --
             * SDRMDIR is the only sanctioned way to remove one. */
            if (IsDirectory(rmFsName)) {
                WriteStatus(buf, EXP_STATUS_ERROR);
                break;
            }
            WriteStatus(buf, f_unlink(rmFsName) == FR_OK ? EXP_STATUS_SUCCESS : EXP_STATUS_ERROR);
            break;
        }
        case EXP_COMMAND_CHANGE_SD_DIR: {
            WriteStatus(buf, EXP_STATUS_BUSY);
            uint16_t cdNameLen = buf[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS] * 256 +
                                  buf[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS + 1];
            if (cdNameLen == 0) {
                WriteStatus(buf, EXP_STATUS_ERROR);
                break;
            }
            char cdName[EXP_PATH_ARG_LEN + 1];
            if (cdNameLen > EXP_PATH_ARG_LEN) cdNameLen = EXP_PATH_ARG_LEN;
            StringFromBuffer(buf, EXP_BUFFER_START_PAGE, EXP_BUFFER_START_ADDRESS + 2, cdNameLen,
                              cdName);
            char cdFsName[EXP_PATH_ARG_LEN + 1];
            PrepareFsName(cdName, cdFsName, sizeof(cdFsName));
            WriteStatus(buf, f_chdir(cdFsName) == FR_OK ? EXP_STATUS_SUCCESS : EXP_STATUS_ERROR);
            break;
        }
        case EXP_COMMAND_MAKE_SD_DIR: {
            WriteStatus(buf, EXP_STATUS_BUSY);
            uint16_t mkNameLen = buf[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS] * 256 +
                                  buf[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS + 1];
            if (mkNameLen == 0) {
                WriteStatus(buf, EXP_STATUS_ERROR);
                break;
            }
            char mkName[EXP_PATH_ARG_LEN + 1];
            if (mkNameLen > EXP_PATH_ARG_LEN) mkNameLen = EXP_PATH_ARG_LEN;
            StringFromBuffer(buf, EXP_BUFFER_START_PAGE, EXP_BUFFER_START_ADDRESS + 2, mkNameLen,
                              mkName);
            char mkFsName[EXP_PATH_ARG_LEN + 1];
            PrepareFsName(mkName, mkFsName, sizeof(mkFsName));
            WriteStatus(buf, f_mkdir(mkFsName) == FR_OK ? EXP_STATUS_SUCCESS : EXP_STATUS_ERROR);
            break;
        }
        case EXP_COMMAND_REMOVE_SD_DIR: {
            WriteStatus(buf, EXP_STATUS_BUSY);
            uint16_t rmdirNameLen = buf[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS] * 256 +
                                     buf[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS + 1];
            if (rmdirNameLen == 0) {
                WriteStatus(buf, EXP_STATUS_ERROR);
                break;
            }
            char rmdirName[EXP_PATH_ARG_LEN + 1];
            if (rmdirNameLen > EXP_PATH_ARG_LEN) rmdirNameLen = EXP_PATH_ARG_LEN;
            StringFromBuffer(buf, EXP_BUFFER_START_PAGE, EXP_BUFFER_START_ADDRESS + 2,
                              rmdirNameLen, rmdirName);
            char rmdirFsName[EXP_PATH_ARG_LEN + 1];
            PrepareFsName(rmdirName, rmdirFsName, sizeof(rmdirFsName));
            /* f_unlink() on a directory only succeeds if it's empty
             * (FR_DENIED otherwise) -- same semantics as emFile's
             * FS_RmDir, no extra check needed. f_rmdir is #define'd to
             * f_unlink in ff.h. */
            WriteStatus(buf, f_unlink(rmdirFsName) == FR_OK ? EXP_STATUS_SUCCESS : EXP_STATUS_ERROR);
            break;
        }
        case EXP_COMMAND_GET_SD_CWD: {
            WriteStatus(buf, EXP_STATUS_BUSY);
            char cwdRaw[64];
            char cwd[64];
            if (f_getcwd(cwdRaw, sizeof(cwdRaw)) != FR_OK) {
                WriteStatus(buf, EXP_STATUS_ERROR);
                break;
            }
            NormalizeSdPathFromFs(cwdRaw, cwd, sizeof(cwd));
            ConvertTildeToPlus(cwd);
            uint8_t cwdLen = (uint8_t)strlen(cwd);
            buf[EXP_SCRATCH_PAGE][0] = cwdLen;
            for (uint8_t i = 0; i < cwdLen; i++) buf[EXP_SCRATCH_PAGE][i + 1] = cwd[i];
            WriteStatus(buf, EXP_STATUS_SUCCESS);
            break;
        }
        case EXP_COMMAND_COPY_SD_FILE: {
            WriteStatus(buf, EXP_STATUS_BUSY);
            uint8_t *window = &buf[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS];
            uint16_t srcLen = (uint16_t)window[0] * 256 + window[1];
            uint16_t destLen = (uint16_t)window[EXP_TWO_NAME_SLOT_LEN] * 256 +
                                window[EXP_TWO_NAME_SLOT_LEN + 1];
            if (srcLen == 0 || destLen == 0 || srcLen > EXP_PATH_ARG_LEN ||
                destLen > EXP_PATH_ARG_LEN) {
                WriteStatus(buf, EXP_STATUS_ERROR);
                break;
            }
            char srcArg[EXP_PATH_ARG_LEN + 1], destArg[EXP_PATH_ARG_LEN + 1];
            for (uint16_t i = 0; i < srcLen; i++) srcArg[i] = (char)window[2 + i];
            srcArg[srcLen] = 0;
            for (uint16_t i = 0; i < destLen; i++)
                destArg[i] = (char)window[EXP_TWO_NAME_SLOT_LEN + 2 + i];
            destArg[destLen] = 0;
            char srcFsName[EXP_PATH_ARG_LEN + 1];
            PrepareFsName(srcArg, srcFsName, sizeof(srcFsName));
            char destFsName[EXP_PATH_ARG_LEN + 16];
            ResolveCopyOrMoveDestination(GetBasename(srcArg), destArg, destFsName,
                                          sizeof(destFsName));
            WriteStatus(buf, CopyFileFatFs(srcFsName, destFsName) ? EXP_STATUS_SUCCESS
                                                                    : EXP_STATUS_ERROR);
            break;
        }
        case EXP_COMMAND_MOVE_SD_FILE: {
            WriteStatus(buf, EXP_STATUS_BUSY);
            uint8_t *window = &buf[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS];
            uint16_t srcLen = (uint16_t)window[0] * 256 + window[1];
            uint16_t destLen = (uint16_t)window[EXP_TWO_NAME_SLOT_LEN] * 256 +
                                window[EXP_TWO_NAME_SLOT_LEN + 1];
            if (srcLen == 0 || destLen == 0 || srcLen > EXP_PATH_ARG_LEN ||
                destLen > EXP_PATH_ARG_LEN) {
                WriteStatus(buf, EXP_STATUS_ERROR);
                break;
            }
            char srcArg[EXP_PATH_ARG_LEN + 1], destArg[EXP_PATH_ARG_LEN + 1];
            for (uint16_t i = 0; i < srcLen; i++) srcArg[i] = (char)window[2 + i];
            srcArg[srcLen] = 0;
            for (uint16_t i = 0; i < destLen; i++)
                destArg[i] = (char)window[EXP_TWO_NAME_SLOT_LEN + 2 + i];
            destArg[destLen] = 0;
            char srcFsName[EXP_PATH_ARG_LEN + 1];
            PrepareFsName(srcArg, srcFsName, sizeof(srcFsName));
            char destFsName[EXP_PATH_ARG_LEN + 16];
            ResolveCopyOrMoveDestination(GetBasename(srcArg), destArg, destFsName,
                                          sizeof(destFsName));
            WriteStatus(buf, f_rename(srcFsName, destFsName) == FR_OK ? EXP_STATUS_SUCCESS
                                                                        : EXP_STATUS_ERROR);
            break;
        }
        case EXP_COMMAND_CHECK_SD_COPY_MOVE_DEST_EXISTS: {
            WriteStatus(buf, EXP_STATUS_BUSY);
            uint8_t *window = &buf[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS];
            uint16_t srcLen = (uint16_t)window[0] * 256 + window[1];
            uint16_t destLen = (uint16_t)window[EXP_TWO_NAME_SLOT_LEN] * 256 +
                                window[EXP_TWO_NAME_SLOT_LEN + 1];
            if (srcLen == 0 || destLen == 0 || srcLen > EXP_PATH_ARG_LEN ||
                destLen > EXP_PATH_ARG_LEN) {
                WriteStatus(buf, EXP_STATUS_ERROR);
                break;
            }
            char srcArg[EXP_PATH_ARG_LEN + 1], destArg[EXP_PATH_ARG_LEN + 1];
            for (uint16_t i = 0; i < srcLen; i++) srcArg[i] = (char)window[2 + i];
            srcArg[srcLen] = 0;
            for (uint16_t i = 0; i < destLen; i++)
                destArg[i] = (char)window[EXP_TWO_NAME_SLOT_LEN + 2 + i];
            destArg[destLen] = 0;
            char destFsName[EXP_PATH_ARG_LEN + 16];
            ResolveCopyOrMoveDestination(GetBasename(srcArg), destArg, destFsName,
                                          sizeof(destFsName));
            FILINFO fno;
            WriteStatus(buf, f_stat(destFsName, &fno) == FR_OK ? EXP_STATUS_SUCCESS
                                                                 : EXP_STATUS_ERROR);
            break;
        }
        case EXP_COMMAND_SD_OPEN_CHANNEL: {
            WriteStatus(buf, EXP_STATUS_BUSY);
            uint8_t *window = &buf[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS];
            uint8_t channel = window[0];
            if (channel < 1 || channel > EXP_MAX_SD_CHANNELS) {
                WriteStatus(buf, EXP_STATUS_ERROR);
                break;
            }
            uint16_t nameLen = (uint16_t)window[1] * 256 + window[2];
            if (nameLen == 0 || nameLen > EXP_PATH_ARG_LEN) {
                WriteStatus(buf, EXP_STATUS_ERROR);
                break;
            }
            char name[EXP_PATH_ARG_LEN + 1];
            for (uint16_t i = 0; i < nameLen; i++) name[i] = (char)window[3 + i];
            name[nameLen] = 0;
            char fsName[EXP_PATH_ARG_LEN + 1];
            PrepareFsName(name, fsName, sizeof(fsName));

            uint8_t idx = channel - 1;
            if (channelOpen[idx]) {
                f_close(&channelFile[idx]);
                channelOpen[idx] = false;
            }
            /* Open for combined read+append -- create if it doesn't
             * exist, never truncate one that does (FA_OPEN_ALWAYS). */
            channelOpen[idx] = f_open(&channelFile[idx], fsName, FA_READ | FA_WRITE | FA_OPEN_ALWAYS) == FR_OK;
            if (!channelOpen[idx]) {
                WriteStatus(buf, EXP_STATUS_ERROR);
                break;
            }
            strncpy(channelName[idx], name, EXP_PATH_ARG_LEN);
            channelName[idx][EXP_PATH_ARG_LEN] = 0;
            channelReadPos[idx] = 0;
            WriteStatus(buf, EXP_STATUS_SUCCESS);
            break;
        }
        case EXP_COMMAND_SD_CLOSE_CHANNEL: {
            WriteStatus(buf, EXP_STATUS_BUSY);
            uint8_t *window = &buf[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS];
            uint8_t channel = window[0];
            if (channel == 0) {
                for (uint8_t i = 0; i < EXP_MAX_SD_CHANNELS; i++) {
                    if (channelOpen[i]) {
                        f_close(&channelFile[i]);
                        channelOpen[i] = false;
                    }
                    channelName[i][0] = 0;
                    channelReadPos[i] = 0;
                }
                WriteStatus(buf, EXP_STATUS_SUCCESS);
                break;
            }
            if (channel > EXP_MAX_SD_CHANNELS) {
                WriteStatus(buf, EXP_STATUS_ERROR);
                break;
            }
            uint8_t idx = channel - 1;
            if (channelOpen[idx]) {
                f_close(&channelFile[idx]);
                channelOpen[idx] = false;
            }
            channelName[idx][0] = 0;
            channelReadPos[idx] = 0;
            WriteStatus(buf, EXP_STATUS_SUCCESS);
            break;
        }
        case EXP_COMMAND_SD_LIST_CHANNELS: {
            WriteStatus(buf, EXP_STATUS_BUSY);
            uint8_t *window = &buf[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS];
            uint16_t count = 0;
            for (uint8_t i = 0; i < EXP_MAX_SD_CHANNELS && count < EXP_DIR_MAX_ENTRIES; i++) {
                if (!channelOpen[i]) continue;
                uint8_t *entry = window + 2 + (uint16_t)count * EXP_DIR_RECORD_SIZE;
                char label[EXP_DIR_NAME_LEN];
                uint8_t pos = 0;
                pos = (uint8_t)(pos + WriteDecimal((uint32_t)(i + 1), (uint8_t *)(label + pos)));
                if (pos < EXP_DIR_NAME_LEN) label[pos++] = ':';
                uint8_t j;
                for (j = 0; channelName[i][j] != 0 && pos < EXP_DIR_NAME_LEN; j++)
                    label[pos++] = channelName[i][j];
                uint8_t labelLen = pos;
                for (j = 0; j < EXP_DIR_NAME_LEN; j++)
                    entry[j] = (j < labelLen) ? (uint8_t)label[j] : ' ';
                for (j = 0; j < EXP_DIR_SIZE_TEXT_LEN; j++) (entry + EXP_DIR_NAME_LEN)[j] = ' ';
                for (j = 0; j < 4; j++) entry[EXP_DIR_NAME_LEN + EXP_DIR_SIZE_TEXT_LEN + j] = 0;
                count++;
            }
            window[0] = (uint8_t)(count >> 8);
            window[1] = (uint8_t)(count & 0xFF);
            {
                uint16_t summaryOffset = (uint16_t)(2 + (uint16_t)count * EXP_DIR_RECORD_SIZE);
                if (summaryOffset + EXP_DIR_SUMMARY_LEN <= 4095) {
                    char summary[16];
                    uint8_t pos = 0;
                    pos = (uint8_t)(pos + WriteDecimal((uint32_t)count, (uint8_t *)(summary + pos)));
                    summary[pos++] = ' ';
                    summary[pos++] = 'O';
                    summary[pos++] = 'P';
                    summary[pos++] = 'E';
                    summary[pos++] = 'N';
                    uint8_t k;
                    for (k = 0; k < EXP_DIR_SUMMARY_LEN; k++)
                        window[summaryOffset + k] = (k < pos) ? (uint8_t)summary[k] : ' ';
                }
            }
            WriteStatus(buf, EXP_STATUS_SUCCESS);
            break;
        }
        case EXP_COMMAND_SD_WRITE_VALUE: {
            WriteStatus(buf, EXP_STATUS_BUSY);
            uint8_t *window = &buf[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS];
            uint8_t channel = window[0];
            if (channel < 1 || channel > EXP_MAX_SD_CHANNELS || !channelOpen[channel - 1]) {
                WriteStatus(buf, EXP_STATUS_ERROR);
                break;
            }
            uint8_t idx = channel - 1;
            uint8_t tag = window[1];
            uint16_t chunkLen;
            if (tag == 'N') chunkLen = 1 + 8;
            else if (tag == 'S') chunkLen = (uint16_t)(1 + 1 + window[2]);
            else {
                WriteStatus(buf, EXP_STATUS_ERROR);
                break;
            }
            SeekToEnd(&channelFile[idx]);
            UINT written = 0;
            f_write(&channelFile[idx], window + 1, chunkLen, &written);
            if (written != chunkLen) {
                WriteStatus(buf, EXP_STATUS_ERROR);
                break;
            }
            WriteStatus(buf, EXP_STATUS_SUCCESS);
            break;
        }
        case EXP_COMMAND_SD_READ_VALUE: {
            WriteStatus(buf, EXP_STATUS_BUSY);
            uint8_t *window = &buf[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS];
            uint8_t channel = window[0];
            if (channel < 1 || channel > EXP_MAX_SD_CHANNELS || !channelOpen[channel - 1]) {
                WriteStatus(buf, EXP_STATUS_ERROR);
                break;
            }
            uint8_t idx = channel - 1;
            f_lseek(&channelFile[idx], channelReadPos[idx]);
            uint8_t tag;
            UINT got = 0;
            f_read(&channelFile[idx], &tag, 1, &got);
            if (got != 1) {
                WriteStatus(buf, EXP_STATUS_EOF);
                break;
            }
            uint16_t payloadLen;
            uint8_t lenByte = 0;
            if (tag == 'N') {
                payloadLen = 8;
            } else if (tag == 'S') {
                f_read(&channelFile[idx], &lenByte, 1, &got);
                if (got != 1) {
                    WriteStatus(buf, EXP_STATUS_EOF);
                    break;
                }
                payloadLen = lenByte;
            } else {
                WriteStatus(buf, EXP_STATUS_ERROR); /* corrupt file */
                break;
            }
            uint8_t payload[255];
            if (payloadLen > 0) {
                f_read(&channelFile[idx], payload, payloadLen, &got);
                if (got != payloadLen) {
                    WriteStatus(buf, EXP_STATUS_EOF);
                    break;
                }
            }
            uint16_t pos = 0;
            window[pos++] = tag;
            if (tag == 'S') window[pos++] = lenByte;
            for (uint16_t i = 0; i < payloadLen; i++) window[pos++] = payload[i];
            channelReadPos[idx] = (uint32_t)f_tell(&channelFile[idx]);
            WriteStatus(buf, EXP_STATUS_SUCCESS);
            break;
        }
        case EXP_COMMAND_SD_SKIP_VALUES: {
            WriteStatus(buf, EXP_STATUS_BUSY);
            uint8_t *window = &buf[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS];
            uint8_t channel = window[0];
            if (channel < 1 || channel > EXP_MAX_SD_CHANNELS || !channelOpen[channel - 1]) {
                WriteStatus(buf, EXP_STATUS_ERROR);
                break;
            }
            uint8_t idx = channel - 1;
            uint16_t count = (uint16_t)window[1] * 256 + window[2];
            uint32_t pos = channelReadPos[idx];
            bool ok = true;
            for (uint16_t i = 0; i < count; i++) {
                f_lseek(&channelFile[idx], pos);
                uint8_t tag;
                UINT got = 0;
                f_read(&channelFile[idx], &tag, 1, &got);
                if (got != 1) {
                    ok = false;
                    break;
                }
                if (tag == 'N') {
                    pos += 1 + 8;
                } else if (tag == 'S') {
                    uint8_t lenByte = 0;
                    f_read(&channelFile[idx], &lenByte, 1, &got);
                    if (got != 1) {
                        ok = false;
                        break;
                    }
                    pos += (uint32_t)(1 + 1 + lenByte);
                } else {
                    ok = false;
                    break;
                }
            }
            if (!ok) {
                WriteStatus(buf, EXP_STATUS_ERROR);
                break;
            }
            channelReadPos[idx] = pos;
            WriteStatus(buf, EXP_STATUS_SUCCESS);
            break;
        }
        case EXP_COMMAND_VALIDATE_SD_NAME: {
            WriteStatus(buf, EXP_STATUS_BUSY);
            uint16_t nameLen = (uint16_t)buf[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS] * 256 +
                                buf[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS + 1];
            uint8_t *name = &buf[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS + 2];
            uint8_t nameCount = 0, extCount = 0, dotSeen = 0, dotOnly = 1, valid = 1;
            uint16_t i;
            for (i = 0; i < nameLen && valid; i++) {
                uint8_t c = name[i];
                if (c >= 'a' && c <= 'z') c = (uint8_t)(c - 'a' + 'A');
                if (c == '/') {
                    nameCount = 0;
                    extCount = 0;
                    dotSeen = 0;
                    dotOnly = 1;
                } else if (c == '.') {
                    if (dotOnly) {
                        /* "." or ".." so far -- allow, don't touch counters */
                    } else if (dotSeen) {
                        valid = 0;
                    } else {
                        dotSeen = 1;
                    }
                } else {
                    dotOnly = 0;
                    if (!dotSeen) {
                        nameCount++;
                        if (nameCount > 8) valid = 0;
                    } else {
                        extCount++;
                        if (extCount > 3) valid = 0;
                    }
                }
                name[i] = c;
            }
            if (nameLen == 0) valid = 0;
            WriteStatus(buf, valid ? EXP_STATUS_SUCCESS : EXP_STATUS_ERROR);
            break;
        }
        case EXP_COMMAND_GET_SD_DF_TEXT: {
            WriteStatus(buf, EXP_STATUS_BUSY);
            uint32_t total, free_;
            GetVolumeSpace(&total, &free_);
            char text[32];
            uint8_t pos = 0;
            pos = (uint8_t)(pos + WriteDecimal(free_, (uint8_t *)(text + pos)));
            text[pos++] = 'F';
            text[pos++] = ' ';
            text[pos++] = '/';
            text[pos++] = ' ';
            pos = (uint8_t)(pos + WriteDecimal(total, (uint8_t *)(text + pos)));
            text[pos++] = 'T';
            buf[EXP_SCRATCH_PAGE][0] = pos;
            for (uint8_t i = 0; i < pos; i++) buf[EXP_SCRATCH_PAGE][i + 1] = (uint8_t)text[i];
            WriteStatus(buf, EXP_STATUS_SUCCESS);
            break;
        }
        case EXP_COMMAND_LIST_SD_DIR: {
            WriteStatus(buf, EXP_STATUS_BUSY);
            uint8_t *window = &buf[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS];
            DIR dir;
            FILINFO fno;
            uint16_t count = 0;
            uint32_t totalBytes = 0;
            FRESULT fr = f_findfirst(&dir, &fno, "", "*");
            while (fr == FR_OK && fno.fname[0] != 0 && count < EXP_DIR_MAX_ENTRIES) {
                /* FatFs's own find functions never return "."/".." --
                 * unlike emFile, so this loop never sees them. */
                uint8_t *entry = window + 2 + (uint16_t)count * EXP_DIR_RECORD_SIZE;
                ConvertTildeToPlus(fno.fname);
                uint8_t nameLen = (uint8_t)strlen(fno.fname);
                bool isDir = (fno.fattrib & AM_DIR) != 0;
                if (nameLen > EXP_DIR_NAME_LEN) nameLen = EXP_DIR_NAME_LEN;
                for (uint8_t i = 0; i < EXP_DIR_NAME_LEN; i++)
                    entry[i] = (i < nameLen) ? (uint8_t)fno.fname[i] : ' ';
                if (isDir) {
                    static const char dirText[] = "<DIR>";
                    uint8_t dirTextLen = (uint8_t)(sizeof(dirText) - 1);
                    uint8_t j;
                    for (j = 0; j < EXP_DIR_SIZE_TEXT_LEN; j++) (entry + EXP_DIR_NAME_LEN)[j] = ' ';
                    for (j = 0; j < dirTextLen; j++)
                        (entry + EXP_DIR_NAME_LEN)[EXP_DIR_SIZE_TEXT_LEN - dirTextLen + j] =
                            (uint8_t)dirText[j];
                } else {
                    FormatSizeText((uint32_t)fno.fsize, entry + EXP_DIR_NAME_LEN,
                                    EXP_DIR_SIZE_TEXT_LEN);
                }
                {
                    uint32_t sizeValue = isDir ? 0 : (uint32_t)fno.fsize;
                    entry[EXP_DIR_NAME_LEN + EXP_DIR_SIZE_TEXT_LEN] = (uint8_t)(sizeValue >> 24);
                    entry[EXP_DIR_NAME_LEN + EXP_DIR_SIZE_TEXT_LEN + 1] = (uint8_t)(sizeValue >> 16);
                    entry[EXP_DIR_NAME_LEN + EXP_DIR_SIZE_TEXT_LEN + 2] = (uint8_t)(sizeValue >> 8);
                    entry[EXP_DIR_NAME_LEN + EXP_DIR_SIZE_TEXT_LEN + 3] = (uint8_t)(sizeValue);
                }
                if (!isDir) totalBytes += (uint32_t)fno.fsize;
                count++;
                fr = f_findnext(&dir, &fno);
            }
            f_closedir(&dir);
            window[0] = (uint8_t)(count >> 8);
            window[1] = (uint8_t)(count & 0xFF);
            {
                uint16_t summaryOffset = (uint16_t)(2 + (uint16_t)count * EXP_DIR_RECORD_SIZE);
                if (summaryOffset + EXP_DIR_SUMMARY_LEN <= 4095) {
                    uint32_t total, free_;
                    GetVolumeSpace(&total, &free_);
                    FormatSummaryLine(count, totalBytes, free_, window + summaryOffset,
                                       EXP_DIR_SUMMARY_LEN);
                }
            }
            WriteStatus(buf, EXP_STATUS_SUCCESS);
            break;
        }
        case EXP_COMMAND_CLEAR_STATUS: {
            WriteStatus(buf, EXP_STATUS_READY);
            break;
        }
        case EXP_COMMAND_CREATE_SD_FILE: {
            WriteStatus(buf, EXP_STATUS_BUSY);
            uint16_t fileNameLen = buf[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS] * 256 +
                                    buf[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS + 1];
            if (fileNameLen == 0 || fileNameLen > EXP_PATH_ARG_LEN) {
                WriteStatus(buf, EXP_STATUS_ERROR);
                break;
            }
            char fileName[EXP_PATH_ARG_LEN + 1];
            StringFromBuffer(buf, EXP_BUFFER_START_PAGE, EXP_BUFFER_START_ADDRESS + 2, fileNameLen,
                              fileName);
            char fsName[EXP_PATH_ARG_LEN + 1];
            PrepareFsName(fileName, fsName, sizeof(fsName));
            currentFileOpen = f_open(&currentFile, fsName, FA_CREATE_ALWAYS | FA_WRITE) == FR_OK;
            if (!currentFileOpen) {
                WriteStatus(buf, EXP_STATUS_ERROR);
            } else {
                currentFileStatus = EXP_SD_FILE_STATUS_OPEN_WRITE;
                strncpy(currentFileName, fileName, sizeof(currentFileName) - 1);
                currentFileName[sizeof(currentFileName) - 1] = 0;
                WriteStatus(buf, EXP_STATUS_SUCCESS);
            }
            break;
        }
        case EXP_COMMAND_OPEN_SD_FILE_READ: {
            WriteStatus(buf, EXP_STATUS_BUSY);
            uint16_t readNameLen = buf[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS] * 256 +
                                    buf[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS + 1];
            if (readNameLen == 0 || readNameLen > EXP_PATH_ARG_LEN) {
                WriteStatus(buf, EXP_STATUS_ERROR);
                break;
            }
            char readFileName[EXP_PATH_ARG_LEN + 1];
            StringFromBuffer(buf, EXP_BUFFER_START_PAGE, EXP_BUFFER_START_ADDRESS + 2, readNameLen,
                              readFileName);
            char readFsName[EXP_PATH_ARG_LEN + 1];
            PrepareFsName(readFileName, readFsName, sizeof(readFsName));
            currentFileOpen = f_open(&currentFile, readFsName, FA_READ) == FR_OK;
            if (!currentFileOpen) {
                WriteStatus(buf, EXP_STATUS_ERROR);
            } else {
                currentFileStatus = EXP_SD_FILE_STATUS_OPEN_READ;
                strncpy(currentFileName, readFileName, sizeof(currentFileName) - 1);
                currentFileName[sizeof(currentFileName) - 1] = 0;
                WriteStatus(buf, EXP_STATUS_SUCCESS);
            }
            break;
        }
        case EXP_COMMAND_GET_SD_FILE_STATUS: {
            WriteStatus(buf, EXP_STATUS_BUSY);
            buf[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS] = currentFileStatus;
            WriteStatus(buf, EXP_STATUS_SUCCESS);
            break;
        }
        case EXP_COMMAND_WRITE_TO_SD_FILE: {
            if (!currentFileOpen || currentFileStatus != EXP_SD_FILE_STATUS_OPEN_WRITE) break;
            WriteStatus(buf, EXP_STATUS_BUSY);
            uint16_t dataLen = (buf[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS] << 8) +
                                buf[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS + 1];
            if (dataLen == 0) {
                WriteStatus(buf, EXP_STATUS_ERROR);
                break;
            }
            const uint8_t *src = &buf[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS + 2];
            UINT dataWritten = 0;
            f_write(&currentFile, src, dataLen, &dataWritten);
            if (dataWritten == dataLen) {
                WriteStatus(buf, EXP_STATUS_SUCCESS);
                fileEnd += dataLen;
            } else {
                WriteStatus(buf, EXP_STATUS_ERROR);
            }
            break;
        }
        case EXP_COMMAND_READ_FROM_SD_FILE: {
            if (!currentFileOpen || currentFileStatus != EXP_SD_FILE_STATUS_OPEN_READ) {
                WriteStatus(buf, EXP_STATUS_ERROR);
                break;
            }
            WriteStatus(buf, EXP_STATUS_BUSY);
            uint16_t requestLen = (buf[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS] << 8) +
                                   buf[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS + 1];
            if (requestLen == 0 || requestLen > 254) {
                WriteStatus(buf, EXP_STATUS_ERROR);
                break;
            }
            uint8_t *readDest = &buf[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS + 2];
            UINT bytesRead = 0;
            f_read(&currentFile, readDest, requestLen, &bytesRead);
            buf[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS] = (uint8_t)(bytesRead >> 8);
            buf[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS + 1] = (uint8_t)(bytesRead & 255);
            WriteStatus(buf, EXP_STATUS_SUCCESS);
            break;
        }
        case EXP_COMMAND_CLOSE_SD_FILE: {
            if (!currentFileOpen || currentFileStatus == EXP_SD_FILE_STATUS_CLOSED) break;
            WriteStatus(buf, EXP_STATUS_BUSY);
            FRESULT result = f_close(&currentFile);
            if (result == FR_OK) {
                currentFileOpen = false;
                currentFileStatus = EXP_SD_FILE_STATUS_CLOSED;
                WriteStatus(buf, EXP_STATUS_SUCCESS);
                LongToBuffer(buf, EXP_BUFFER_START_PAGE, EXP_BUFFER_START_ADDRESS, fileEnd);
                fileEnd = 0;
            } else {
                WriteStatus(buf, EXP_STATUS_ERROR);
                LongToBuffer(buf, EXP_BUFFER_START_PAGE, EXP_BUFFER_START_ADDRESS, (uint32_t)result);
            }
            break;
        }
        case EXP_COMMAND_FORMAT_SD_CARD: {
            WriteStatus(buf, EXP_STATUS_BUSY);
            uint16_t volumeNameLen = buf[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS] * 256 +
                                      buf[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS + 1];
            if (volumeNameLen == 0) {
                WriteStatus(buf, EXP_STATUS_ERROR);
                break;
            }
            char volumeName[EXP_PATH_ARG_LEN + 1];
            if (volumeNameLen > EXP_PATH_ARG_LEN) volumeNameLen = EXP_PATH_ARG_LEN;
            StringFromBuffer(buf, EXP_BUFFER_START_PAGE, EXP_BUFFER_START_ADDRESS + 2,
                              volumeNameLen, volumeName);
            static uint8_t mkfsWork[FF_MAX_SS];
            MKFS_PARM opt = {.fmt = FM_ANY, .n_fat = 0, .align = 0, .n_root = 0, .au_size = 0};
            WriteStatus(buf, f_mkfs(volumeName, &opt, mkfsWork, sizeof(mkfsWork)) == FR_OK
                                  ? EXP_STATUS_SUCCESS
                                  : EXP_STATUS_ERROR);
            break;
        }
        case EXP_COMMAND_TEST_COPY_STRING: {
            WriteStatus(buf, EXP_STATUS_BUSY);
            uint16_t dataLen = buf[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS] * 256 +
                                buf[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS + 1];
            if (dataLen == 0 || dataLen > 254) {
                WriteStatus(buf, EXP_STATUS_ERROR);
                break;
            }
            char testString[255];
            StringFromBuffer(buf, EXP_BUFFER_START_PAGE, EXP_BUFFER_START_ADDRESS + 2, dataLen,
                              testString);
            for (uint16_t i = 0; i < dataLen; i++) buf[4][i + 1] = testString[i];
            buf[4][0] = (uint8_t)dataLen;
            WriteStatus(buf, EXP_STATUS_SUCCESS);
            break;
        }
        default: {
            WriteStatus(buf, EXP_STATUS_NOT_IMPLEMENTED);
            break;
        }
    }
}

/* ============================================================
 * Bus loop -- ported from main()'s for(;;) in main.c, restructured
 * around the GreenPAK's two combined trigger lines (see board_pins.h)
 * instead of separately-sampled CS/RW/OE pins.
 * ============================================================ */

static void InitGpio(void) {
    for (int p = ADDR_PIN_BASE; p < ADDR_PIN_BASE + ADDR_PIN_COUNT; p++) gpio_init(p);
    for (int p = DATA_PIN_LOW_BASE; p < DATA_PIN_LOW_BASE + DATA_PIN_LOW_COUNT; p++) gpio_init(p);
    for (int p = DATA_PIN_HIGH_BASE; p < DATA_PIN_HIGH_BASE + DATA_PIN_HIGH_COUNT; p++) gpio_init(p);
    gpio_init(PIN_TRIG_RD);
    gpio_init(PIN_TRIG_WR);
    /* Data pins start as inputs (Hi-Z) -- only driven while servicing a
     * read-trigger, see the loop below. Address/trigger pins are always
     * inputs from this chip's side. */
}

static inline uint16_t ReadAddress(uint32_t gpio_in) {
    return (uint16_t)((gpio_in >> ADDR_PIN_BASE) & ADDR_PIN_MASK);
}

static inline uint8_t ReadDataIn(uint32_t gpio_in) {
    uint8_t low = (uint8_t)((gpio_in >> DATA_PIN_LOW_BASE) & ((1u << DATA_PIN_LOW_COUNT) - 1u));
    uint8_t high = (uint8_t)((gpio_in >> DATA_PIN_HIGH_BASE) & ((1u << DATA_PIN_HIGH_COUNT) - 1u));
    return (uint8_t)(low | (high << DATA_PIN_LOW_COUNT));
}

static inline void DriveData(uint8_t value) {
    uint32_t low_mask = ((1u << DATA_PIN_LOW_COUNT) - 1u) << DATA_PIN_LOW_BASE;
    uint32_t high_mask = ((1u << DATA_PIN_HIGH_COUNT) - 1u) << DATA_PIN_HIGH_BASE;
    uint32_t low_bits = ((uint32_t)value & ((1u << DATA_PIN_LOW_COUNT) - 1u)) << DATA_PIN_LOW_BASE;
    uint32_t high_bits = (((uint32_t)value >> DATA_PIN_LOW_COUNT) & ((1u << DATA_PIN_HIGH_COUNT) - 1u))
                          << DATA_PIN_HIGH_BASE;
    sio_hw->gpio_oe_set = low_mask | high_mask; /* switch data pins to output */
    sio_hw->gpio_out = (sio_hw->gpio_out & ~(low_mask | high_mask)) | low_bits | high_bits;
}

static inline void ReleaseData(void) {
    uint32_t low_mask = ((1u << DATA_PIN_LOW_COUNT) - 1u) << DATA_PIN_LOW_BASE;
    uint32_t high_mask = ((1u << DATA_PIN_HIGH_COUNT) - 1u) << DATA_PIN_HIGH_BASE;
    sio_hw->gpio_oe_clr = low_mask | high_mask; /* back to Hi-Z input */
}

void monitor_run(void) {
    InitGpio();

    /* buffer[page][laddress] addressing matches the original's
     * 16-page-wide (or 32-page for the ROM region) local layout: the
     * 13-bit flat address here splits the same way -- low 8 bits are
     * `laddress`, remaining bits are `page`, exactly like
     * Pin_Address_Low_PS/Status_Page_Status did on the PSoC5 side. */
    for (;;) {
        uint32_t gpio_in = sio_hw->gpio_in;
        bool readTrigger = (gpio_in & (1u << PIN_TRIG_RD)) != 0;
        bool writeTrigger = (gpio_in & (1u << PIN_TRIG_WR)) != 0;

        if (readTrigger) {
            uint16_t addr = ReadAddress(gpio_in);
            uint8_t page = (uint8_t)(addr >> 8);
            uint8_t laddress = (uint8_t)(addr & 0xFF);
            DriveData(buffer[page][laddress]);
            /* Hold the drive until the trigger deasserts -- matches the
             * PSoC5 original's own "leave Pin_Data_DR at this value
             * until the next loop iteration overwrites it" behavior
             * (there, an external tri-state buffer gated the actual bus
             * drive; here, this chip's own GPIO output-enable does). */
            while (sio_hw->gpio_in & (1u << PIN_TRIG_RD)) {
            }
            ReleaseData();
        } else if (writeTrigger) {
            uint16_t addr = ReadAddress(gpio_in);
            uint8_t page = (uint8_t)(addr >> 8);
            uint8_t laddress = (uint8_t)(addr & 0xFF);
            uint8_t data = ReadDataIn(gpio_in);
            if (page == EXP_INSTRUCTION_PAGE && laddress == EXP_INSTRUCTION_ADDRESS) {
                DoCommand(data, buffer);
            } else {
                buffer[page][laddress] = data;
            }
            while (sio_hw->gpio_in & (1u << PIN_TRIG_WR)) {
            }
        }
    }
}
