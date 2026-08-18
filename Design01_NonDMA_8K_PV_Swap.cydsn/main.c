/* ========================================
 *
 * Copyright YOUR COMPANY, THE YEAR
 * All Rights Reserved
 * UNPUBLISHED, LICENSED SOFTWARE.
 *
 * CONFIDENTIAL AND PROPRIETARY INFORMATION
 * WHICH IS THE PROPERTY OF your company.
 *
 * ========================================
*/
#include "project.h"
#include "FS.h"
#include "PC_EXP.h"
#include <string.h>
#include "rom/rom_image.h"

FS_FILE *currentFile;
uint8 currentFileStatus = EXP_SD_FILE_STATUS_CLOSED;
uint32 fileEnd = 0;
uint8* buff;
char currentFileName[64];

//SDOPEN/SDCLOSE/SDINPUT#/SDPRINT#/SDSKIP#'s own real multi-file state --
//entirely separate from currentFile/currentFileName above (SDLOAD/SDSAVE's
//own single-open-file mechanism); the two don't interact. Index i holds
//channel (i+1). channelName is kept only for EXP_COMMAND_SD_LIST_CHANNELS'
//own display; channelReadPos is the persistent SDINPUT#/SDSKIP# cursor,
//deliberately independent of the file's own internal position (which
//EXP_COMMAND_SD_WRITE_VALUE moves to the end and back on every SDPRINT#
//call). See PC_EXP.h's own comment for the full writeup and the chunk
//format both sides share.
FS_FILE *channelFile[EXP_MAX_SD_CHANNELS];
char channelName[EXP_MAX_SD_CHANNELS][EXP_PATH_ARG_LEN + 1];
uint32 channelReadPos[EXP_MAX_SD_CHANNELS];

void InitBuffer(uint8 rom[32][256])
{
    memset(rom, 0, 32 * 256);
    LoadRomImage(rom);
}

void LongToBuffer(uint8 buffer[16][256], int8 page, int8 start, uint32 value) 
{
    buffer[page][start] = (int)((value >> 24) & 0xFF) ;
    buffer[page][start+1] = (int)((value >> 16) & 0xFF) ;
    buffer[page][start+2] = (int)((value >> 8) & 0XFF);
    buffer[page][start+3] = (int)((value & 0XFF));
}

void StringFromBuffer(uint8 buffer[16][256], uint8 page, uint8 start, uint16 length, char result[])
{
    const uint8* buff = &buffer[page][start];
    for (uint16 i = 0; i < length; i++)
    {
        result[i] = (char)*buff++;
    }
    result[length]=0;
}

void DataFromBuffer(uint8 buffer[16][256], uint8 page, uint8 start, uint16 length, uint8 result[])
{
    const uint8* buff = &buffer[page][start];
    for (uint16 i = 0; i < length; i++)
    {
        result[i] = *buff++;
    }
}

//Right-justifies `value` as ASCII decimal into result[0..width-1], space-
//padded on the left -- e.g. FormatSizeText(6234, buf, 10) -> "      6234".
//Deliberately done here (MCU-side), not on the LH5801: the ROM has no
//binary-to-decimal routine of its own, and there's no need to invent one
//when this side can trivially render display-ready text instead of a raw
//number (see PC_EXP.h's EXP_DIR_SIZE_TEXT_LEN comment).
void FormatSizeText(uint32 value, uint8 result[], uint8 width)
{
    uint8 i;
    for (i = 0; i < width; i++)
        result[i] = ' ';
    i = width;
    do
    {
        i--;
        result[i] = (uint8)('0' + (value % 10));
        value /= 10;
    } while (value != 0 && i > 0);
}

//Writes `value` as left-justified ASCII decimal (no padding) starting at
//result[0]; returns the digit count written. `result` must have room for
//at least 10 bytes (uint32's max digit count) -- callers here always pass
//into a generously-sized scratch buffer, not directly into a fixed-width
//wire-format field.
uint8 WriteDecimal(uint32 value, uint8 result[])
{
    uint8 buf[10];
    uint8 n = 0;
    uint8 i;
    if (value == 0)
    {
        result[0] = '0';
        return 1;
    }
    while (value != 0)
    {
        buf[n++] = (uint8)('0' + (value % 10));
        value /= 10;
    }
    for (i = 0; i < n; i++)
        result[i] = buf[n - 1 - i];
    return n;
}

//This project's own SD-path convention (SDCD/SDMKDIR/SDRMDIR/SDPWD) is a
//single '/' separator, no volume-name prefix, and exactly "/" at the SD
//root -- '/' specifically because it's an unshifted key on the real
//PC-1500 keyboard, easy for a user to actually type. emFile's own paths
//look like "PC1500:\SUBDIR" (backslash-separated, volume-prefixed); these
//two helpers convert between the two conventions at the boundary, so nothing
//above the DoCommand() switch (or the ROM side, which just blits whatever
//text comes back) ever needs to know emFile's own convention exists.
void NormalizeSdPathFromFs(const char* raw, char* out, uint8 outSize)
{
    const char* colon = strchr(raw, ':');
    const char* src = colon ? colon + 1 : raw;
    uint8 n = 0;
    out[n++] = '/';
    for (; *src != 0 && n < outSize - 1; src++)
    {
        char c = (*src == '\\') ? '/' : *src;
        //Avoid a doubled leading slash: after stripping the volume prefix,
        //emFile's own root is just "\" (or empty), which would otherwise
        //normalize to a second '/' right after the one already placed above.
        if (n == 1 && c == '/') continue;
        out[n++] = c;
    }
    out[n] = 0;
}

//Converts a '/'-separated path (as typed by the user, e.g. via SDCD) to
//emFile's own native '\' convention, in place, before handing it to
//FS_ChDir/etc.
void ConvertSdPathToFsSeparators(char* path)
{
    for (; *path != 0; path++)
        if (*path == '/') *path = '\\';
}

//This project's own SD-name convention also swaps '+' for a real short
//FAT name's '~' -- the PC-1500 keyboard has a '+' key but no '~' key, and
//a card prepared on a normal PC (long filenames) and then read by this
//firmware's NLFN emFile build shows its auto-generated short names, which
//often contain a literal '~' (e.g. "MYAPPL~1.BAS"). Every SD command now
//enforces uppercase 8.3 shape on its own argument (see rom.asm's
//SD_PARSE_QUOTED_NAME), so '+' can never legitimately appear in a name for
//any other reason -- the swap is unconditional and unambiguous in both
//directions (unlike '-', which is a legal FAT 8.3 character and could
//collide with a real hyphenated name). Apply ConvertPlusToTilde to any
//name arriving from the PC-1500 before it touches the real filesystem
//(lookups *and* creates -- typing "APPL+1.BAS" to SDSAVE deliberately
//creates a file literally named "APPL~1.BAS"), and ConvertTildeToPlus to
//any real on-disk name before it's shown back to the PC-1500.
void ConvertPlusToTilde(char* name)
{
    for (; *name != 0; name++)
        if (*name == '+') *name = '~';
}

void ConvertTildeToPlus(char* name)
{
    for (; *name != 0; name++)
        if (*name == '~') *name = '+';
}

//Every SD command's name argument is a full path now (a plain filename, a
//relative path with '.'/'..'/multiple components, or an absolute one
//starting with '/' from the SD root) -- rom.asm's SD_PARSE_QUOTED_NAME
//already validates 8.3 shape per '/'-segment regardless of what the
//argument is actually used for. PrepareFsName is the one place that
//converts a raw typed argument into the exact string an FS_* call needs:
//'+'->'~' (see ConvertPlusToTilde's own comment) and '/'->'\\' (see
//ConvertSdPathToFsSeparators's own comment). A leading '\\' with no
//volume prefix is passed straight through to FS_ChDir/FS_FOpen/etc.,
//relying on emFile's own path parser to treat it as "from the SD root" --
//the same assumption CHANGE_SD_DIR's own FS_ChDir call already made
//(confirmed live for SDCD; not independently re-verified for every other
//FS_* function here, but there is no reason emFile would parse the same
//syntax differently depending on which function receives it).
//`raw` and `out` may safely be different buffers only -- write into a
//buffer separate from wherever the original (unconverted, for later
//redisplay) argument is kept.
void PrepareFsName(const char* raw, char* out, uint8 outSize)
{
    strncpy(out, raw, outSize - 1);
    out[outSize - 1] = 0;
    ConvertPlusToTilde(out);
    ConvertSdPathToFsSeparators(out);
}

//Returns a pointer into `path` at its last '/' or '\\' component (or
//`path` itself if there's none) -- the plain filename a full path ends
//with. Used to get SDCP/SDMV's source basename for ResolveCopyOrMove
//Destination below. Works on the raw ('+'-preserved, '/'-separated)
//argument -- checking both separators means it also works if called
//after PrepareFsName has already converted it, though nothing here does
//that.
const char* GetBasename(const char* path)
{
    const char* base = path;
    for (const char* p = path; *p != 0; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    return base;
}

//SDCP/SDMV's destination resolution: if destArg (after PrepareFsName)
//refers to an *existing directory*, the real target is that directory
//plus the source's own basename, matching Unix cp/mv's own "copy/move
//INTO a directory" behavior; otherwise destArg itself (converted) is the
//target. `out` must be able to hold the longest possible result:
//EXP_PATH_ARG_LEN (converted destArg) + 1 ('\\' separator) + up to a
//12-character 8.3 basename + a null terminator.
void ResolveCopyOrMoveDestination(const char* srcBasename, const char* destArg, char* out,
                                   uint8 outSize)
{
    char converted[EXP_PATH_ARG_LEN + 1];
    PrepareFsName(destArg, converted, sizeof(converted));
    if (FS_GetFileAttributes(converted) & FS_ATTR_DIRECTORY)
    {
        uint8 n = 0;
        for (; converted[n] != 0 && n < outSize - 1; n++) out[n] = converted[n];
        if (n > 0 && out[n-1] != '\\' && n < outSize - 1) out[n++] = '\\';
        uint8 i = 0;
        while (srcBasename[i] != 0 && n < outSize - 1) out[n++] = srcBasename[i++];
        out[n] = 0;
    }
    else
    {
        strncpy(out, converted, outSize - 1);
        out[outSize - 1] = 0;
    }
}

//Writes "<count> FILES <totalBytes>B <freeBytes>F" left-justified into
//result[0..width-1], space-padded/truncated to width -- e.g. "3 FILES
//23051B 2122343F". Mirrors pc1500emu's ExpansionMock::listSdDir summary
//line exactly (kept in sync by hand); see PC_EXP.h's EXP_DIR_SUMMARY_LEN
//comment. Built in a generously-sized local buffer first so a longer
//listing/volume just truncates safely instead of overflowing `result`.
void FormatSummaryLine(uint16 count, uint32 totalBytes, uint32 freeBytes, uint8 result[],
                        uint8 width)
{
    char temp[48];
    uint8 pos = 0;
    uint8 i;
    pos = (uint8)(pos + WriteDecimal(count, (uint8*)(temp + pos)));
    temp[pos++] = ' '; temp[pos++] = 'F'; temp[pos++] = 'I'; temp[pos++] = 'L';
    temp[pos++] = 'E'; temp[pos++] = 'S'; temp[pos++] = ' ';
    pos = (uint8)(pos + WriteDecimal(totalBytes, (uint8*)(temp + pos)));
    temp[pos++] = 'B'; temp[pos++] = ' ';
    pos = (uint8)(pos + WriteDecimal(freeBytes, (uint8*)(temp + pos)));
    temp[pos++] = 'F';
    for (i = 0; i < width; i++)
        result[i] = (i < pos) ? (uint8)temp[i] : ' ';
}

void WriteStatus(uint8 buffer[16][256], uint8 status)
{
    buffer[EXP_INSTRUCTION_PAGE][EXP_INSTRUCTION_ADDRESS] = status;
}    

void DoCommand(uint8 req, uint8 buffer[16][256]) __attribute__((noinline));        

void DoCommand(uint8 req, uint8 buffer[16][256])
{
    Pin_Data_Write(EXP_STATUS_BUSY);
    switch (req) {
        case EXP_COMMAND_ROM_FROM_SRAM:
            {
                Control_Mode_Control = 0;
                WriteStatus(buffer, EXP_STATUS_SUCCESS);
                break;
            }
        case EXP_COMMAND_ROM_FROM_MCU:
            {
                Control_Mode_Control = 1;
                WriteStatus(buffer, EXP_STATUS_SUCCESS);
                break;
            }
        case EXP_COMMAND_GET_SD_FREE_SPACE:
            {
                WriteStatus(buffer, EXP_STATUS_BUSY);
                uint32 freeSpace = FS_GetVolumeFreeSpace("PC1500");
                LongToBuffer(buffer, EXP_BUFFER_START_PAGE, EXP_BUFFER_START_ADDRESS, freeSpace);
                WriteStatus(buffer, EXP_STATUS_SUCCESS);
                break;
            }
        case EXP_COMMAND_GET_SD_VOLUME_SIZE:
            {
                WriteStatus(buffer, EXP_STATUS_BUSY);
                uint32 volumeSize = FS_GetVolumeSize("PC1500");
                LongToBuffer(buffer, EXP_BUFFER_START_PAGE, EXP_BUFFER_START_ADDRESS, volumeSize);
                WriteStatus(buffer, EXP_STATUS_SUCCESS);
                break;
            }
        case EXP_COMMAND_GET_SD_FILE_SIZE:
            {
                if (currentFile == NULL || currentFile == 0)
                {
                    WriteStatus(buffer, EXP_STATUS_ERROR);
                    break;
                }
                WriteStatus(buffer, EXP_STATUS_BUSY);
                uint32 fileSize = FS_GetFileSize(currentFile);
                LongToBuffer(buffer, EXP_BUFFER_START_PAGE, EXP_BUFFER_START_ADDRESS, fileSize);
                WriteStatus(buffer, EXP_STATUS_SUCCESS);
                break;
            }
        case EXP_COMMAND_READ_SD_VOLUME_LABEL:
            {
                WriteStatus(buffer, EXP_STATUS_BUSY);
                uint16 volumeNameLen = buffer[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS] * 256
                    + buffer[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS+1];
                if (volumeNameLen == 0)
                {
                    WriteStatus(buffer, EXP_STATUS_ERROR);
                    break;
                }
                char volumeName[volumeNameLen+1];
                StringFromBuffer(buffer, EXP_BUFFER_START_PAGE, EXP_BUFFER_START_ADDRESS+2, volumeNameLen, volumeName);
                char label[36];
                if (FS_GetVolumeLabel(volumeName, label, sizeof(label)) != 0)
                {
                    WriteStatus(buffer, EXP_STATUS_ERROR);
                    break;
                }
                uint8 labelLen = (uint8)strlen(label);
                buffer[EXP_SCRATCH_PAGE][0] = labelLen;
                for (uint8 i = 0; i < labelLen; i++)
                    buffer[EXP_SCRATCH_PAGE][i+1] = label[i];
                WriteStatus(buffer, EXP_STATUS_SUCCESS);
                break;
            }
        case EXP_COMMAND_GET_SD_FILE_NAME:
            {
                if (currentFile == NULL || currentFile == 0)
                {
                    WriteStatus(buffer, EXP_STATUS_ERROR);
                    break;
                }
                WriteStatus(buffer, EXP_STATUS_BUSY);
                uint8 nameLen = (uint8)strlen(currentFileName);
                buffer[EXP_SCRATCH_PAGE][0] = nameLen;
                for (uint8 i = 0; i < nameLen; i++)
                    buffer[EXP_SCRATCH_PAGE][i+1] = currentFileName[i];
                WriteStatus(buffer, EXP_STATUS_SUCCESS);
                break;
            }
        case EXP_COMMAND_REMOVE_SD_FILE:
            {
                WriteStatus(buffer, EXP_STATUS_BUSY);
                uint16 rmNameLen = buffer[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS] * 256
                    + buffer[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS+1];
                if (rmNameLen == 0)
                {
                    WriteStatus(buffer, EXP_STATUS_ERROR);
                    break;
                }
                char rmName[rmNameLen+1];
                StringFromBuffer(buffer, EXP_BUFFER_START_PAGE, EXP_BUFFER_START_ADDRESS+2, rmNameLen, rmName);
                char rmFsName[rmNameLen+1];
                PrepareFsName(rmName, rmFsName, sizeof(rmFsName));
                //SDRM (rom.asm) must only ever delete a file, never a
                //directory -- SDRMDIR is the only sanctioned way to remove
                //one. FS_GetFileAttributes/FS_ATTR_DIRECTORY: same standard
                //SEGGER emFile FAT-attribute convention already used (and
                //already flagged as unverified against this project's
                //actual FS.h -- no ARM/PSoC toolchain here to compile-check
                //this file) by EXP_COMMAND_LIST_SD_DIR above.
                if (FS_GetFileAttributes(rmFsName) & FS_ATTR_DIRECTORY)
                {
                    WriteStatus(buffer, EXP_STATUS_ERROR);
                    break;
                }
                if (FS_Remove(rmFsName) == 0)
                    WriteStatus(buffer, EXP_STATUS_SUCCESS);
                else
                    WriteStatus(buffer, EXP_STATUS_ERROR);
                break;
            }
        case EXP_COMMAND_CHANGE_SD_DIR:
            {
                WriteStatus(buffer, EXP_STATUS_BUSY);
                uint16 cdNameLen = buffer[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS] * 256
                    + buffer[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS+1];
                if (cdNameLen == 0)
                {
                    WriteStatus(buffer, EXP_STATUS_ERROR);
                    break;
                }
                char cdName[cdNameLen+1];
                StringFromBuffer(buffer, EXP_BUFFER_START_PAGE, EXP_BUFFER_START_ADDRESS+2, cdNameLen, cdName);
                char cdFsName[cdNameLen+1];
                PrepareFsName(cdName, cdFsName, sizeof(cdFsName));
                if (FS_ChDir(cdFsName) == 0)
                    WriteStatus(buffer, EXP_STATUS_SUCCESS);
                else
                    WriteStatus(buffer, EXP_STATUS_ERROR);
                break;
            }
        case EXP_COMMAND_MAKE_SD_DIR:
            {
                WriteStatus(buffer, EXP_STATUS_BUSY);
                uint16 mkNameLen = buffer[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS] * 256
                    + buffer[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS+1];
                if (mkNameLen == 0)
                {
                    WriteStatus(buffer, EXP_STATUS_ERROR);
                    break;
                }
                char mkName[mkNameLen+1];
                StringFromBuffer(buffer, EXP_BUFFER_START_PAGE, EXP_BUFFER_START_ADDRESS+2, mkNameLen, mkName);
                char mkFsName[mkNameLen+1];
                PrepareFsName(mkName, mkFsName, sizeof(mkFsName));
                if (FS_MkDir(mkFsName) == 0)
                    WriteStatus(buffer, EXP_STATUS_SUCCESS);
                else
                    WriteStatus(buffer, EXP_STATUS_ERROR);
                break;
            }
        case EXP_COMMAND_REMOVE_SD_DIR:
            {
                WriteStatus(buffer, EXP_STATUS_BUSY);
                uint16 rmdirNameLen = buffer[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS] * 256
                    + buffer[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS+1];
                if (rmdirNameLen == 0)
                {
                    WriteStatus(buffer, EXP_STATUS_ERROR);
                    break;
                }
                char rmdirName[rmdirNameLen+1];
                StringFromBuffer(buffer, EXP_BUFFER_START_PAGE, EXP_BUFFER_START_ADDRESS+2, rmdirNameLen, rmdirName);
                char rmdirFsName[rmdirNameLen+1];
                PrepareFsName(rmdirName, rmdirFsName, sizeof(rmdirFsName));
                //FS_RmDir only removes an EMPTY directory (standard emFile
                //semantics, matching POSIX rmdir) -- a non-empty one fails
                //here and reports EXP_STATUS_ERROR, same as any other
                //filesystem-level failure.
                if (FS_RmDir(rmdirFsName) == 0)
                    WriteStatus(buffer, EXP_STATUS_SUCCESS);
                else
                    WriteStatus(buffer, EXP_STATUS_ERROR);
                break;
            }
        case EXP_COMMAND_GET_SD_CWD:
            {
                WriteStatus(buffer, EXP_STATUS_BUSY);
                char cwdRaw[64];
                char cwd[64];
                if (FS_GetCWD(cwdRaw, sizeof(cwdRaw)) != 0)
                {
                    WriteStatus(buffer, EXP_STATUS_ERROR);
                    break;
                }
                //"/" convention, not emFile's own "PC1500:\..." -- see
                //NormalizeSdPathFromFs's own comment. This is also what
                //makes the SD root report as exactly "/" rather than
                //whatever emFile's own root representation happens to be.
                NormalizeSdPathFromFs(cwdRaw, cwd, sizeof(cwd));
                ConvertTildeToPlus(cwd);  //show any real '~' as '+', matching SDLS
                //Length-prefixed response into EXP_SCRATCH_PAGE, same
                //convention as GET_SD_FILE_NAME's own response just above --
                //single-byte length, so capped at 255 (cwd's own 64-byte
                //buffer is already well under that).
                uint8 cwdLen = (uint8)strlen(cwd);
                buffer[EXP_SCRATCH_PAGE][0] = cwdLen;
                for (uint8 i = 0; i < cwdLen; i++)
                    buffer[EXP_SCRATCH_PAGE][i+1] = cwd[i];
                WriteStatus(buffer, EXP_STATUS_SUCCESS);
                break;
            }
        case EXP_COMMAND_COPY_SD_FILE:
            {
                WriteStatus(buffer, EXP_STATUS_BUSY);
                //Two fixed EXP_TWO_NAME_SLOT_LEN-byte slots back-to-back --
                //see PC_EXP.h's own comment. Flat pointer, same reasoning as
                //EXP_COMMAND_LIST_SD_DIR's own window pointer below.
                uint8* window = &buffer[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS];
                uint16 srcLen = (uint16)window[0] * 256 + window[1];
                uint16 destLen = (uint16)window[EXP_TWO_NAME_SLOT_LEN] * 256
                    + window[EXP_TWO_NAME_SLOT_LEN + 1];
                if (srcLen == 0 || destLen == 0)
                {
                    WriteStatus(buffer, EXP_STATUS_ERROR);
                    break;
                }
                char srcArg[srcLen+1];
                char destArg[destLen+1];
                for (uint16 i = 0; i < srcLen; i++) srcArg[i] = (char)window[2+i];
                srcArg[srcLen] = 0;
                for (uint16 i = 0; i < destLen; i++)
                    destArg[i] = (char)window[EXP_TWO_NAME_SLOT_LEN + 2 + i];
                destArg[destLen] = 0;
                char srcFsName[srcLen+1];
                PrepareFsName(srcArg, srcFsName, sizeof(srcFsName));
                //If destArg resolves to an existing directory, the real
                //target is that directory plus srcArg's own basename --
                //see ResolveCopyOrMoveDestination's own comment. Overwriting
                //an existing target is confirmed first by the ROM
                //(EXP_COMMAND_CHECK_SD_COPY_MOVE_DEST_EXISTS below) unless
                //-Y was given; this command itself always overwrites
                //unconditionally once dispatched.
                char destFsName[EXP_PATH_ARG_LEN + 16];
                ResolveCopyOrMoveDestination(GetBasename(srcArg), destArg, destFsName,
                                              sizeof(destFsName));
                if (FS_CopyFile(srcFsName, destFsName) == 0)
                    WriteStatus(buffer, EXP_STATUS_SUCCESS);
                else
                    WriteStatus(buffer, EXP_STATUS_ERROR);
                break;
            }
        case EXP_COMMAND_MOVE_SD_FILE:
            {
                WriteStatus(buffer, EXP_STATUS_BUSY);
                uint8* window = &buffer[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS];
                uint16 srcLen = (uint16)window[0] * 256 + window[1];
                uint16 destLen = (uint16)window[EXP_TWO_NAME_SLOT_LEN] * 256
                    + window[EXP_TWO_NAME_SLOT_LEN + 1];
                if (srcLen == 0 || destLen == 0)
                {
                    WriteStatus(buffer, EXP_STATUS_ERROR);
                    break;
                }
                char srcArg[srcLen+1];
                char destArg[destLen+1];
                for (uint16 i = 0; i < srcLen; i++) srcArg[i] = (char)window[2+i];
                srcArg[srcLen] = 0;
                for (uint16 i = 0; i < destLen; i++)
                    destArg[i] = (char)window[EXP_TWO_NAME_SLOT_LEN + 2 + i];
                destArg[destLen] = 0;
                char srcFsName[srcLen+1];
                PrepareFsName(srcArg, srcFsName, sizeof(srcFsName));
                char destFsName[EXP_PATH_ARG_LEN + 16];
                ResolveCopyOrMoveDestination(GetBasename(srcArg), destArg, destFsName,
                                              sizeof(destFsName));
                if (FS_Move(srcFsName, destFsName) == 0)
                    WriteStatus(buffer, EXP_STATUS_SUCCESS);
                else
                    WriteStatus(buffer, EXP_STATUS_ERROR);
                break;
            }
        case EXP_COMMAND_CHECK_SD_COPY_MOVE_DEST_EXISTS:
            {
                WriteStatus(buffer, EXP_STATUS_BUSY);
                //Same wire layout and destination resolution as COPY_SD_FILE/
                //MOVE_SD_FILE above (shared -- the resolution logic, including
                //directory-target basename-join, is identical for both).
                //SUCCESS means the real (resolved) target already exists, so
                //SDCP_ROUTINE/SDMV_ROUTINE (rom.asm) know to show the
                //overwrite-confirmation prompt unless -Y was given.
                uint8* window = &buffer[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS];
                uint16 srcLen = (uint16)window[0] * 256 + window[1];
                uint16 destLen = (uint16)window[EXP_TWO_NAME_SLOT_LEN] * 256
                    + window[EXP_TWO_NAME_SLOT_LEN + 1];
                if (srcLen == 0 || destLen == 0)
                {
                    WriteStatus(buffer, EXP_STATUS_ERROR);
                    break;
                }
                char srcArg[srcLen+1];
                char destArg[destLen+1];
                for (uint16 i = 0; i < srcLen; i++) srcArg[i] = (char)window[2+i];
                srcArg[srcLen] = 0;
                for (uint16 i = 0; i < destLen; i++)
                    destArg[i] = (char)window[EXP_TWO_NAME_SLOT_LEN + 2 + i];
                destArg[destLen] = 0;
                char destFsName[EXP_PATH_ARG_LEN + 16];
                ResolveCopyOrMoveDestination(GetBasename(srcArg), destArg, destFsName,
                                              sizeof(destFsName));
                FS_FILE* probe = FS_FOpen(destFsName, "rb");
                if (probe != NULL && probe != 0)
                {
                    FS_FClose(probe);
                    WriteStatus(buffer, EXP_STATUS_SUCCESS);  //exists
                }
                else
                {
                    WriteStatus(buffer, EXP_STATUS_ERROR);  //doesn't exist
                }
                break;
            }
        case EXP_COMMAND_SD_OPEN_CHANNEL:
            {
                WriteStatus(buffer, EXP_STATUS_BUSY);
                uint8* window = &buffer[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS];
                uint8 channel = window[0];
                if (channel < 1 || channel > EXP_MAX_SD_CHANNELS)
                {
                    WriteStatus(buffer, EXP_STATUS_ERROR);
                    break;
                }
                uint16 nameLen = (uint16)window[1] * 256 + window[2];
                if (nameLen == 0)
                {
                    WriteStatus(buffer, EXP_STATUS_ERROR);
                    break;
                }
                char name[nameLen+1];
                for (uint16 i = 0; i < nameLen; i++) name[i] = (char)window[3+i];
                name[nameLen] = 0;
                char fsName[nameLen+1];
                PrepareFsName(name, fsName, sizeof(fsName));

                uint8 idx = channel - 1;
                //Reusing an already-open channel number closes it first.
                if (channelFile[idx] != NULL && channelFile[idx] != 0)
                {
                    FS_FClose(channelFile[idx]);
                    channelFile[idx] = NULL;
                }
                //Open for combined read+append -- create the file if it
                //doesn't exist, never truncate one that does.
                channelFile[idx] = FS_FOpen(fsName, "r+b");
                if (channelFile[idx] == NULL || channelFile[idx] == 0)
                {
                    FS_FILE* created = FS_FOpen(fsName, "wb");
                    if (created != NULL && created != 0) FS_FClose(created);
                    channelFile[idx] = FS_FOpen(fsName, "r+b");
                }
                if (channelFile[idx] == NULL || channelFile[idx] == 0)
                {
                    WriteStatus(buffer, EXP_STATUS_ERROR);
                    break;
                }
                strncpy(channelName[idx], name, EXP_PATH_ARG_LEN);
                channelName[idx][EXP_PATH_ARG_LEN] = 0;
                channelReadPos[idx] = 0;
                WriteStatus(buffer, EXP_STATUS_SUCCESS);
                break;
            }
        case EXP_COMMAND_SD_CLOSE_CHANNEL:
            {
                WriteStatus(buffer, EXP_STATUS_BUSY);
                uint8* window = &buffer[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS];
                uint8 channel = window[0];
                if (channel == 0)
                {
                    for (uint8 i = 0; i < EXP_MAX_SD_CHANNELS; i++)
                    {
                        if (channelFile[i] != NULL && channelFile[i] != 0)
                        {
                            FS_FClose(channelFile[i]);
                            channelFile[i] = NULL;
                        }
                        channelName[i][0] = 0;
                        channelReadPos[i] = 0;
                    }
                    WriteStatus(buffer, EXP_STATUS_SUCCESS);
                    break;
                }
                if (channel > EXP_MAX_SD_CHANNELS)
                {
                    WriteStatus(buffer, EXP_STATUS_ERROR);
                    break;
                }
                uint8 idx = channel - 1;
                if (channelFile[idx] != NULL && channelFile[idx] != 0)
                {
                    FS_FClose(channelFile[idx]);
                    channelFile[idx] = NULL;
                }
                channelName[idx][0] = 0;
                channelReadPos[idx] = 0;
                WriteStatus(buffer, EXP_STATUS_SUCCESS);
                break;
            }
        case EXP_COMMAND_SD_LIST_CHANNELS:
            {
                WriteStatus(buffer, EXP_STATUS_BUSY);
                //Reuses EXP_COMMAND_LIST_SD_DIR's own wire format exactly
                //(see that case's own comment) so rom.asm's existing
                //SD_LIST_INIT/SD_LIST_DISPLAY browse machinery draws this
                //unmodified -- one record per open channel, name field
                //showing "<n>:<filename>".
                uint8* window = &buffer[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS];
                uint16 count = 0;
                for (uint8 i = 0; i < EXP_MAX_SD_CHANNELS && count < EXP_DIR_MAX_ENTRIES; i++)
                {
                    if (channelFile[i] == NULL || channelFile[i] == 0) continue;
                    uint8* entry = window + 2 + (uint16)count * EXP_DIR_RECORD_SIZE;
                    char label[EXP_DIR_NAME_LEN];
                    uint8 pos = 0;
                    pos = (uint8)(pos + WriteDecimal((uint32)(i + 1), (uint8*)(label + pos)));
                    if (pos < EXP_DIR_NAME_LEN) label[pos++] = ':';
                    uint8 j;
                    for (j = 0; channelName[i][j] != 0 && pos < EXP_DIR_NAME_LEN; j++)
                        label[pos++] = channelName[i][j];
                    uint8 labelLen = pos;
                    for (j = 0; j < EXP_DIR_NAME_LEN; j++)
                        entry[j] = (j < labelLen) ? (uint8)label[j] : ' ';
                    //Size-text field left blank -- an open channel doesn't
                    //have a meaningful "size" the way a directory entry does.
                    for (j = 0; j < EXP_DIR_SIZE_TEXT_LEN; j++)
                        (entry + EXP_DIR_NAME_LEN)[j] = ' ';
                    for (j = 0; j < 4; j++)
                        entry[EXP_DIR_NAME_LEN + EXP_DIR_SIZE_TEXT_LEN + j] = 0;
                    count++;
                }
                window[0] = (uint8)(count >> 8);
                window[1] = (uint8)(count & 0xFF);
                {
                    uint16 summaryOffset = (uint16)(2 + (uint16)count * EXP_DIR_RECORD_SIZE);
                    if (summaryOffset + EXP_DIR_SUMMARY_LEN <= 4095)
                    {
                        char summary[16];
                        uint8 pos = 0;
                        pos = (uint8)(pos + WriteDecimal((uint32)count, (uint8*)(summary + pos)));
                        summary[pos++] = ' '; summary[pos++] = 'O'; summary[pos++] = 'P';
                        summary[pos++] = 'E'; summary[pos++] = 'N';
                        uint8 k;
                        for (k = 0; k < EXP_DIR_SUMMARY_LEN; k++)
                            window[summaryOffset + k] = (k < pos) ? (uint8)summary[k] : ' ';
                    }
                }
                WriteStatus(buffer, EXP_STATUS_SUCCESS);
                break;
            }
        case EXP_COMMAND_SD_WRITE_VALUE:
            {
                WriteStatus(buffer, EXP_STATUS_BUSY);
                //window[0]=channel, window[1..]=one chunk (['N']+8 bytes,
                //or ['S']+1-byte length+that many bytes -- see PC_EXP.h's
                //own comment) built by rom.asm from a looked-up variable's
                //own storage. Always appended to the end of the channel's
                //file, regardless of its own read position.
                uint8* window = &buffer[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS];
                uint8 channel = window[0];
                if (channel < 1 || channel > EXP_MAX_SD_CHANNELS)
                {
                    WriteStatus(buffer, EXP_STATUS_ERROR);
                    break;
                }
                uint8 idx = channel - 1;
                if (channelFile[idx] == NULL || channelFile[idx] == 0)
                {
                    WriteStatus(buffer, EXP_STATUS_ERROR);
                    break;
                }
                uint8 tag = window[1];
                uint16 chunkLen;
                if (tag == 'N')
                    chunkLen = 1 + 8;
                else if (tag == 'S')
                    chunkLen = (uint16)(1 + 1 + window[2]);
                else
                {
                    WriteStatus(buffer, EXP_STATUS_ERROR);
                    break;
                }
                FS_FSeek(channelFile[idx], 0, FS_SEEK_END);
                uint32 written = FS_FWrite(window + 1, 1, chunkLen, channelFile[idx]);
                if (written != chunkLen)
                {
                    WriteStatus(buffer, EXP_STATUS_ERROR);
                    break;
                }
                WriteStatus(buffer, EXP_STATUS_SUCCESS);
                break;
            }
        case EXP_COMMAND_SD_READ_VALUE:
            {
                WriteStatus(buffer, EXP_STATUS_BUSY);
                //window[0]=channel (request); response overwrites
                //window[0..] with the next chunk read from the channel's
                //own persistent read position (advanced past it on
                //return), or EXP_STATUS_EOF if nothing is left -- not an
                //error, SDINPUT# fills any remaining requested variables
                //with 0/blank for that.
                uint8* window = &buffer[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS];
                uint8 channel = window[0];
                if (channel < 1 || channel > EXP_MAX_SD_CHANNELS)
                {
                    WriteStatus(buffer, EXP_STATUS_ERROR);
                    break;
                }
                uint8 idx = channel - 1;
                if (channelFile[idx] == NULL || channelFile[idx] == 0)
                {
                    WriteStatus(buffer, EXP_STATUS_ERROR);
                    break;
                }
                FS_FSeek(channelFile[idx], (I32)channelReadPos[idx], FS_SEEK_SET);
                uint8 tag;
                uint32 got = FS_FRead(&tag, 1, 1, channelFile[idx]);
                if (got != 1)
                {
                    WriteStatus(buffer, EXP_STATUS_EOF);
                    break;
                }
                uint16 payloadLen;
                uint8 lenByte = 0;
                if (tag == 'N')
                {
                    payloadLen = 8;
                }
                else if (tag == 'S')
                {
                    got = FS_FRead(&lenByte, 1, 1, channelFile[idx]);
                    if (got != 1)
                    {
                        WriteStatus(buffer, EXP_STATUS_EOF);
                        break;
                    }
                    payloadLen = lenByte;
                }
                else
                {
                    WriteStatus(buffer, EXP_STATUS_ERROR);  //corrupt file
                    break;
                }
                uint8 payload[255];
                if (payloadLen > 0)
                {
                    got = FS_FRead(payload, 1, payloadLen, channelFile[idx]);
                    if (got != payloadLen)
                    {
                        WriteStatus(buffer, EXP_STATUS_EOF);
                        break;
                    }
                }
                uint16 pos = 0;
                window[pos++] = tag;
                if (tag == 'S') window[pos++] = lenByte;
                for (uint16 i = 0; i < payloadLen; i++) window[pos++] = payload[i];
                channelReadPos[idx] = (uint32)FS_FTell(channelFile[idx]);
                WriteStatus(buffer, EXP_STATUS_SUCCESS);
                break;
            }
        case EXP_COMMAND_SD_SKIP_VALUES:
            {
                WriteStatus(buffer, EXP_STATUS_BUSY);
                //window[0]=channel, window[1..2]=count (2-byte BE) of
                //values to skip forward. All-or-nothing: if the channel
                //runs out before finishing the count, the read position is
                //left completely unchanged and EXP_STATUS_ERROR returned --
                //SDSKIP# (rom.asm) raises a real ERROR 40 for that.
                uint8* window = &buffer[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS];
                uint8 channel = window[0];
                if (channel < 1 || channel > EXP_MAX_SD_CHANNELS)
                {
                    WriteStatus(buffer, EXP_STATUS_ERROR);
                    break;
                }
                uint8 idx = channel - 1;
                if (channelFile[idx] == NULL || channelFile[idx] == 0)
                {
                    WriteStatus(buffer, EXP_STATUS_ERROR);
                    break;
                }
                uint16 count = (uint16)window[1] * 256 + window[2];
                uint32 pos = channelReadPos[idx];
                uint8 ok = 1;
                for (uint16 i = 0; i < count; i++)
                {
                    FS_FSeek(channelFile[idx], (I32)pos, FS_SEEK_SET);
                    uint8 tag;
                    uint32 got = FS_FRead(&tag, 1, 1, channelFile[idx]);
                    if (got != 1) { ok = 0; break; }
                    if (tag == 'N')
                    {
                        pos += 1 + 8;
                    }
                    else if (tag == 'S')
                    {
                        uint8 lenByte = 0;
                        got = FS_FRead(&lenByte, 1, 1, channelFile[idx]);
                        if (got != 1) { ok = 0; break; }
                        pos += (uint32)(1 + 1 + lenByte);
                    }
                    else
                    {
                        ok = 0;
                        break;
                    }
                }
                if (!ok)
                {
                    WriteStatus(buffer, EXP_STATUS_ERROR);
                    break;
                }
                channelReadPos[idx] = pos;
                WriteStatus(buffer, EXP_STATUS_SUCCESS);
                break;
            }
        case EXP_COMMAND_VALIDATE_SD_NAME:
            {
                WriteStatus(buffer, EXP_STATUS_BUSY);
                //Validates+uppercase-folds, in place, the length-prefixed
                //raw name SD_PARSE_QUOTED_NAME (rom.asm) already staged at
                //EXP_BUFFER_START_ABS -- ported verbatim from the LH5801
                //state machine that used to live there (see PC_EXP.h's own
                //comment for the full rule): every SD command's name
                //argument is a full path (a plain filename, a relative
                //path with '/'/'.'/'..' components, or an absolute one
                //starting with '/'), so each '/'-separated segment must
                //independently be <=8 characters, optionally followed by
                //'.' and <=3 more, with at most one '.' -- except a
                //segment that is exactly "." or "..", always allowed
                //through untouched. '+' needs no special handling here --
                //it's an ordinary character for shape-counting purposes;
                //the actual '+'<->'~' translation happens later, at the
                //real filesystem boundary.
                uint16 nameLen = (uint16)buffer[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS] * 256
                    + buffer[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS+1];
                uint8* name = &buffer[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS+2];
                uint8 nameCount = 0, extCount = 0, dotSeen = 0, dotOnly = 1, valid = 1;
                uint16 i;
                for (i = 0; i < nameLen && valid; i++)
                {
                    uint8 c = name[i];
                    if (c >= 'a' && c <= 'z') c = (uint8)(c - 'a' + 'A');
                    if (c == '/')
                    {
                        nameCount = 0;
                        extCount = 0;
                        dotSeen = 0;
                        dotOnly = 1;
                    }
                    else if (c == '.')
                    {
                        if (dotOnly)
                        {
                            //"." or ".." so far -- allow, don't touch counters
                        }
                        else if (dotSeen)
                        {
                            valid = 0;  //second '.' in a real name
                        }
                        else
                        {
                            dotSeen = 1;
                        }
                    }
                    else
                    {
                        dotOnly = 0;
                        if (!dotSeen)
                        {
                            nameCount++;
                            if (nameCount > 8) valid = 0;
                        }
                        else
                        {
                            extCount++;
                            if (extCount > 3) valid = 0;
                        }
                    }
                    name[i] = c;
                }
                if (nameLen == 0) valid = 0;  //shouldn't happen -- rom.asm already rejects an empty name
                WriteStatus(buffer, valid ? EXP_STATUS_SUCCESS : EXP_STATUS_ERROR);
                break;
            }
        case EXP_COMMAND_GET_SD_DF_TEXT:
            {
                WriteStatus(buffer, EXP_STATUS_BUSY);
                uint32 freeSpace = FS_GetVolumeFreeSpace("PC1500");
                uint32 volumeSize = FS_GetVolumeSize("PC1500");
                //Pre-rendered "<free>F / <total>T" text, matching
                //FormatSummaryLine's own "B"/"F" suffix convention (see its
                //own comment) -- the ROM has no decimal-to-ASCII conversion
                //of its own, see EXP_COMMAND_GET_SD_DF_TEXT's own comment in
                //PC_EXP.h. Kept in sync by hand with ExpansionMock::
                //getSdDfText, same caveat as FormatSummaryLine.
                char text[32];
                uint8 pos = 0;
                pos = (uint8)(pos + WriteDecimal(freeSpace, (uint8*)(text + pos)));
                text[pos++] = 'F'; text[pos++] = ' '; text[pos++] = '/'; text[pos++] = ' ';
                pos = (uint8)(pos + WriteDecimal(volumeSize, (uint8*)(text + pos)));
                text[pos++] = 'T';
                buffer[EXP_SCRATCH_PAGE][0] = pos;
                for (uint8 i = 0; i < pos; i++)
                    buffer[EXP_SCRATCH_PAGE][i+1] = (uint8)text[i];
                WriteStatus(buffer, EXP_STATUS_SUCCESS);
                break;
            }
        case EXP_COMMAND_LIST_SD_DIR:
            {
                WriteStatus(buffer, EXP_STATUS_BUSY);
                //Whole listing in one shot, fixed-width records, straight into
                //the data window -- see PC_EXP.h's EXP_DIR_* comment for the
                //wire format. Flat pointer (not buffer[page][..]) since a
                //large listing spans multiple pages; buffer[0..7] is one
                //contiguous 2K run in memory, so this is safe.
                uint8* window = &buffer[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS];
                FS_FIND_DATA findData;
                char findName[64];
                uint16 count = 0;
                uint32 totalBytes = 0;
                char findResult = FS_FindFirstFile(&findData, "*.*", findName, sizeof(findName));
                while (findResult == 0 && count < EXP_DIR_MAX_ENTRIES)
                {
                    uint8* entry;
                    uint8 nameLen;
                    uint8 i;
                    uint8 isDir;
                    //emFile's find functions may return the "." and ".."
                    //pseudo-entries every real directory has -- neither is
                    //meaningful to show a PC-1500 user (SDCD "."/".." already
                    //work without needing to see them listed), so skip them.
                    if (strcmp(findName, ".") == 0 || strcmp(findName, "..") == 0)
                    {
                        findResult = FS_FindNextFile(&findData);
                        continue;
                    }
                    entry = window + 2 + (uint16)count * EXP_DIR_RECORD_SIZE;
                    ConvertTildeToPlus(findName);  //show any real '~' as '+'
                    nameLen = (uint8)strlen(findName);
                    //findData.Attributes/FS_ATTR_DIRECTORY: standard SEGGER
                    //emFile FAT-attribute-byte convention (matching real FAT
                    //semantics emFile models itself on) -- like FS_ChDir/
                    //FS_MkDir/FS_RmDir/FS_GetCWD above, not verified against
                    //this project's actual FS.h (no ARM/PSoC toolchain
                    //available to compile-check this file).
                    isDir = (findData.Attributes & FS_ATTR_DIRECTORY) != 0;
                    if (nameLen > EXP_DIR_NAME_LEN)
                        nameLen = EXP_DIR_NAME_LEN;
                    for (i = 0; i < EXP_DIR_NAME_LEN; i++)
                        entry[i] = (i < nameLen) ? (uint8)findName[i] : ' ';
                    //Size text sits right after the name (not the binary size)
                    //so the ROM side can blit name+text in one contiguous
                    //DISP_N_CHARS0 call -- see PC_EXP.h's own EXP_DIR_* comment.
                    //A directory entry shows "<DIR>" here instead of a real
                    //size, right-justified like FormatSizeText's own numeric
                    //output so it lines up in the same column -- the ROM side
                    //needs no changes at all for this, it just blits whatever
                    //text is here, same as always.
                    if (isDir)
                    {
                        const char dirText[] = "<DIR>";
                        uint8 dirTextLen = (uint8)(sizeof(dirText) - 1);
                        uint8 j;
                        for (j = 0; j < EXP_DIR_SIZE_TEXT_LEN; j++)
                            (entry + EXP_DIR_NAME_LEN)[j] = ' ';
                        for (j = 0; j < dirTextLen; j++)
                            (entry + EXP_DIR_NAME_LEN)[EXP_DIR_SIZE_TEXT_LEN - dirTextLen + j] = (uint8)dirText[j];
                    }
                    else
                    {
                        FormatSizeText(findData.FileSize, entry + EXP_DIR_NAME_LEN, EXP_DIR_SIZE_TEXT_LEN);
                    }
                    //Binary size field (trailing 4 bytes) -- 0 for a
                    //directory, matching there being no meaningful byte size;
                    //a consumer that cares should check the size-text field
                    //for "<DIR>" before trusting this rather than assuming
                    //every entry is a file.
                    {
                        uint32 sizeValue = isDir ? 0 : findData.FileSize;
                        entry[EXP_DIR_NAME_LEN+EXP_DIR_SIZE_TEXT_LEN] = (uint8)(sizeValue >> 24);
                        entry[EXP_DIR_NAME_LEN+EXP_DIR_SIZE_TEXT_LEN+1] = (uint8)(sizeValue >> 16);
                        entry[EXP_DIR_NAME_LEN+EXP_DIR_SIZE_TEXT_LEN+2] = (uint8)(sizeValue >> 8);
                        entry[EXP_DIR_NAME_LEN+EXP_DIR_SIZE_TEXT_LEN+3] = (uint8)(sizeValue);
                    }
                    if (!isDir) totalBytes += findData.FileSize;
                    count++;
                    findResult = FS_FindNextFile(&findData);
                }
                FS_FindClose(&findData);
                window[0] = (uint8)(count >> 8);
                window[1] = (uint8)(count & 0xFF);
                {
                    //Summary line right after the last entry -- see
                    //PC_EXP.h's EXP_DIR_SUMMARY_LEN comment. EXP_DIR_MAX_ENTRIES
                    //is sized to always leave room for this, but the bounds
                    //check is kept anyway (matches pc1500emu's ExpansionMock)
                    //since it's cheap insurance against future constant drift.
                    uint16 summaryOffset = (uint16)(2 + (uint16)count * EXP_DIR_RECORD_SIZE);
                    //4095 = EXP_INSTRUCTION_PAGE*256+EXP_INSTRUCTION_ADDRESS, the flat
                    //offset of the instruction/status byte -- must stay untouched.
                    if (summaryOffset + EXP_DIR_SUMMARY_LEN <= 4095)
                    {
                        uint32 freeSpace = FS_GetVolumeFreeSpace("PC1500");
                        FormatSummaryLine(count, totalBytes, freeSpace, window + summaryOffset,
                                           EXP_DIR_SUMMARY_LEN);
                    }
                }
                WriteStatus(buffer, EXP_STATUS_SUCCESS);
                break;
            }
        case EXP_COMMAND_CLEAR_STATUS:
            {
                WriteStatus(buffer, EXP_STATUS_READY);
                break;
            }
        case EXP_COMMAND_CREATE_SD_FILE:
            {
                WriteStatus(buffer, EXP_STATUS_BUSY);
                uint16 fileNameLen = buffer[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS] * 256 
                    + buffer[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS+1];
                if (fileNameLen == 0) 
                {
                    WriteStatus(buffer, EXP_STATUS_ERROR);
                    break;
                }
                char fileName[fileNameLen+1];
                StringFromBuffer(buffer, EXP_BUFFER_START_PAGE, EXP_BUFFER_START_ADDRESS+2, fileNameLen, fileName);
                //fileName is kept in its raw typed form (path separators and
                //'+' intact) for currentFileName below; PrepareFsName builds
                //a separate, converted copy for the FS_FOpen call itself.
                char fsName[fileNameLen+1];
                PrepareFsName(fileName, fsName, sizeof(fsName));
                currentFile = FS_FOpen(fsName, "wb");
                if (currentFile == NULL || currentFile == 0)
                {
                    WriteStatus(buffer, EXP_STATUS_ERROR);
                }
                else
                {
                    currentFileStatus = EXP_SD_FILE_STATUS_OPEN_WRITE;
                    strncpy(currentFileName, fileName, sizeof(currentFileName) - 1);
                    currentFileName[sizeof(currentFileName) - 1] = 0;
                    WriteStatus(buffer, EXP_STATUS_SUCCESS);
                }
                break;
            }
        case EXP_COMMAND_OPEN_SD_FILE_READ:
            {
                WriteStatus(buffer, EXP_STATUS_BUSY);
                uint16 readNameLen = buffer[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS] * 256
                    + buffer[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS+1];
                if (readNameLen == 0)
                {
                    WriteStatus(buffer, EXP_STATUS_ERROR);
                    break;
                }
                char readFileName[readNameLen+1];
                StringFromBuffer(buffer, EXP_BUFFER_START_PAGE, EXP_BUFFER_START_ADDRESS+2, readNameLen, readFileName);
                //Same raw/converted split as CREATE_SD_FILE above.
                char readFsName[readNameLen+1];
                PrepareFsName(readFileName, readFsName, sizeof(readFsName));
                currentFile = FS_FOpen(readFsName, "rb");
                if (currentFile == NULL || currentFile == 0)
                {
                    WriteStatus(buffer, EXP_STATUS_ERROR);
                }
                else
                {
                    currentFileStatus = EXP_SD_FILE_STATUS_OPEN_READ;
                    strncpy(currentFileName, readFileName, sizeof(currentFileName) - 1);
                    currentFileName[sizeof(currentFileName) - 1] = 0;
                    WriteStatus(buffer, EXP_STATUS_SUCCESS);
                }
                break;
            }
        case EXP_COMMAND_GET_SD_FILE_STATUS:
            {
                WriteStatus(buffer, EXP_STATUS_BUSY);
                buffer[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS] = currentFileStatus;
                WriteStatus(buffer, EXP_STATUS_SUCCESS);
                break;
            }
        case EXP_COMMAND_WRITE_TO_SD_FILE:
            {
                if (currentFile == NULL || currentFile == 0 || currentFileStatus != EXP_SD_FILE_STATUS_OPEN_WRITE)
                    break;
                WriteStatus(buffer, EXP_STATUS_BUSY);
                uint16 dataLen = (buffer[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS] << 8) 
                    + buffer[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS+1];
                if (dataLen == 0)
                {
                    WriteStatus(buffer, EXP_STATUS_ERROR);
                    break;
                }
                buffer[15][0]=dataLen >> 8;
                buffer[15][1]=dataLen & 255;
                //uint8 data[dataLen];
                const uint8* buff = &buffer[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS+2];
                //DataFromBuffer(buffer, EXP_BUFFER_START_PAGE, EXP_BUFFER_START_ADDRESS+2, dataLen, data);
                uint32 dataWritten = FS_FWrite(buff, 1, dataLen, currentFile);
                if (dataWritten == dataLen)
                {
                    WriteStatus(buffer, EXP_STATUS_SUCCESS);
                    fileEnd += dataLen;
                }
                else
                    WriteStatus(buffer, EXP_STATUS_ERROR);
                break;
            }
        case EXP_COMMAND_READ_FROM_SD_FILE:
            {
                if (currentFile == NULL || currentFile == 0 || currentFileStatus != EXP_SD_FILE_STATUS_OPEN_READ)
                {
                    WriteStatus(buffer, EXP_STATUS_ERROR);
                    break;
                }
                WriteStatus(buffer, EXP_STATUS_BUSY);
                uint16 requestLen = (buffer[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS] << 8)
                    + buffer[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS+1];
                //Data goes back into the same page starting at +2, so it must
                //fit in what's left of the 256-byte page (254 bytes).
                if (requestLen == 0 || requestLen > 254)
                {
                    WriteStatus(buffer, EXP_STATUS_ERROR);
                    break;
                }
                uint8* readDest = &buffer[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS+2];
                uint32 bytesRead = FS_FRead(readDest, 1, requestLen, currentFile);
                buffer[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS] = (uint8)(bytesRead >> 8);
                buffer[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS+1] = (uint8)(bytesRead & 255);
                WriteStatus(buffer, EXP_STATUS_SUCCESS);
                break;
            }
        case EXP_COMMAND_CLOSE_SD_FILE:
            {
                if (currentFile == NULL || currentFile == 0 || currentFileStatus == EXP_SD_FILE_STATUS_CLOSED)
                    break;
                WriteStatus(buffer, EXP_STATUS_BUSY);
                int result = FS_FClose(currentFile);
                if (result == 0) 
                {
                    currentFile = NULL;
                    currentFileStatus = EXP_SD_FILE_STATUS_CLOSED;
                    WriteStatus(buffer, EXP_STATUS_SUCCESS);
                    LongToBuffer(buffer, EXP_BUFFER_START_PAGE, EXP_BUFFER_START_ADDRESS, fileEnd);
                    fileEnd = 0;
                }
                else 
                {
                    WriteStatus(buffer, EXP_STATUS_ERROR);
                    LongToBuffer(buffer, EXP_BUFFER_START_PAGE, EXP_BUFFER_START_ADDRESS, result);
                }
                break;
            }
        case EXP_COMMAND_FORMAT_SD_CARD:
            {
                WriteStatus(buffer, EXP_STATUS_BUSY);
                uint16 volumeNameLen = buffer[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS] * 256 
                    + buffer[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS+1];
                if (volumeNameLen == 0) 
                {
                    WriteStatus(buffer, EXP_STATUS_ERROR);
                    break;
                }
                char volumeName[volumeNameLen+1];
                StringFromBuffer(buffer, EXP_BUFFER_START_PAGE, EXP_BUFFER_START_ADDRESS+2, volumeNameLen, volumeName);
                FS_FormatLLIfRequired(volumeName);
                if (FS_IsHLFormatted(volumeName) == 0)
                    WriteStatus(buffer,EXP_STATUS_SUCCESS);
                else
                    WriteStatus(buffer,EXP_STATUS_ERROR);
                break;
            }
        case EXP_COMMAND_TEST_COPY_STRING:
            {
                WriteStatus(buffer, EXP_STATUS_BUSY);
                uint16 dataLen = buffer[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS] * 256 
                    + buffer[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS+1];
                if (dataLen == 0)
                {
                    WriteStatus(buffer, EXP_STATUS_ERROR);
                    break;
                }
                char testString[dataLen+1];
                StringFromBuffer(buffer, EXP_BUFFER_START_PAGE, EXP_BUFFER_START_ADDRESS+2, dataLen, testString);
                for (uint16 i=0;i<dataLen;i++) 
                {
                    buffer[4][i+1]=testString[i];
                }
                buffer[4][0]=dataLen;
                WriteStatus(buffer, EXP_STATUS_SUCCESS);
                break;
            }
        default:
            {
                WriteStatus(buffer, EXP_STATUS_NOT_IMPLEMENTED);
                break;
            }
    }
}

int main(void)
{
    //Set busy status until we're done with init
    Pin_Data_DR = EXP_STATUS_BUSY;
    
    uint8 laddress = 0;
    uint8 page = 0;
    uint8 buffer[32][256];
    uint8 data_read;
    uint8 rwpin;
    
    InitBuffer(buffer);

    CyGlobalIntEnable; /* Enable global interrupts. */
    
    FS_Init();
    FS_X_AddDevices();
    
    for(;;)
    {        
        if (Status_CS_Read())
        {
            laddress = Pin_Address_Low_PS;
            page = Status_Page_Status;
            data_read = Pin_Data_PS;
            
            //It's always safe to do this, because strong drive of the data
            //pins is enabled by CS and `OD
            Pin_Data_DR = buffer[page][laddress];

            //Sometimes R/W will already be low when we get here
            if (!(rwpin = Status_RW_Status))
            {    
                if (page == EXP_INSTRUCTION_PAGE && laddress == EXP_INSTRUCTION_ADDRESS) 
                {
                    DoCommand(data_read, buffer);
                }
                else
                    buffer[page][laddress] = data_read;
            }
            else 
            {
                //Try to wait in case RW goes low later
                while (page == Status_Page_Status && Pin_Address_Low_PS == laddress && (rwpin = Status_RW_Status)) 
                {
                }
                if (!rwpin)
                {
                    if (page == EXP_INSTRUCTION_PAGE && laddress == EXP_INSTRUCTION_ADDRESS) 
                    {
                        DoCommand(data_read, buffer);
                    }
                    else 
                        buffer[page][laddress] = data_read;
                }
            }                        
        }
    }        
}


/* [] END OF FILE */
