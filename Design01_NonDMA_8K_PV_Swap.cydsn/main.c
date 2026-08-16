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
                if (FS_Remove(rmName) == 0)
                    WriteStatus(buffer, EXP_STATUS_SUCCESS);
                else
                    WriteStatus(buffer, EXP_STATUS_ERROR);
                break;
            }
        case EXP_COMMAND_LIST_SD_DIR:
            {
                WriteStatus(buffer, EXP_STATUS_BUSY);
                //Whole listing in one shot, fixed-width records, straight into
                //the data window -- see PC_EXP.h's EXP_DIR_* comment for the
                //wire format. Flat pointer (not buffer[page][..]) since a
                //large listing spans multiple pages; buffer[0..15] is one
                //contiguous 4K run in memory, so this is safe.
                uint8* window = &buffer[EXP_BUFFER_START_PAGE][EXP_BUFFER_START_ADDRESS];
                FS_FIND_DATA findData;
                char findName[64];
                uint16 count = 0;
                uint32 totalBytes = 0;
                char findResult = FS_FindFirstFile(&findData, "*.*", findName, sizeof(findName));
                while (findResult == 0 && count < EXP_DIR_MAX_ENTRIES)
                {
                    uint8* entry = window + 2 + (uint16)count * EXP_DIR_RECORD_SIZE;
                    uint8 nameLen = (uint8)strlen(findName);
                    uint8 i;
                    if (nameLen > EXP_DIR_NAME_LEN)
                        nameLen = EXP_DIR_NAME_LEN;
                    for (i = 0; i < EXP_DIR_NAME_LEN; i++)
                        entry[i] = (i < nameLen) ? (uint8)findName[i] : ' ';
                    //Size text sits right after the name (not the binary size)
                    //so the ROM side can blit name+text in one contiguous
                    //DISP_N_CHARS0 call -- see PC_EXP.h's own EXP_DIR_* comment.
                    FormatSizeText(findData.FileSize, entry + EXP_DIR_NAME_LEN, EXP_DIR_SIZE_TEXT_LEN);
                    entry[EXP_DIR_NAME_LEN+EXP_DIR_SIZE_TEXT_LEN] = (uint8)(findData.FileSize >> 24);
                    entry[EXP_DIR_NAME_LEN+EXP_DIR_SIZE_TEXT_LEN+1] = (uint8)(findData.FileSize >> 16);
                    entry[EXP_DIR_NAME_LEN+EXP_DIR_SIZE_TEXT_LEN+2] = (uint8)(findData.FileSize >> 8);
                    entry[EXP_DIR_NAME_LEN+EXP_DIR_SIZE_TEXT_LEN+3] = (uint8)(findData.FileSize);
                    totalBytes += findData.FileSize;
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
                currentFile = FS_FOpen(fileName, "wb");
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
                currentFile = FS_FOpen(readFileName, "rb");
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
