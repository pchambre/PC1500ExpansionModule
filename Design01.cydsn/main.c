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

FS_FILE *currentFile;
uint8 currentFileStatus = EXP_SD_FILE_STATUS_CLOSED;
uint32 fileEnd = 0;
uint8 DMAData;
uint8 DMATD;
uint8* buff;

void InitBuffer(uint8 rom[16][256])
{
    uint8 i = 0;
    uint8 j; 
    while (i < 16)
    {
        j = 0;
        while (j < 255)
            rom[i][j++]=0;
        i++;
    }
            
    rom[0][0]=72;
    rom[0][1]=69;
    rom[0][2]=76;
    rom[0][3]=76;
    rom[0][4]=79;
    rom[0][5]=32;
    rom[0][6]=87;
    rom[0][7]=79;
    rom[0][8]=82;
    rom[0][9]=76;
    rom[0][10]=68;
    //try some custom commands in page 2, with names for them in page 1
    //ERN
    rom[1][0] = 0xC5;
    rom[1][1] = 69;
    rom[1][2] = 82;
    rom[1][3] = 78;
    rom[1][4] = 0xF0;
    rom[1][5] = 0x5E;
    rom[1][6] = 0x82;
    rom[1][7] = 0x00;
    //ERL
    rom[1][8] = 0xC5;
    rom[1][9] = 69;
    rom[1][10] = 82;
    rom[1][11] = 76;
    rom[1][12] = 0xF0;
    rom[1][13] = 0x5F;
    rom[1][14] = 0x82;
    rom[1][15] = 0x06;
    //ERN
    rom[2][0] = 0xA5;
    rom[2][1] = 0x78;
    rom[2][2] = 0x9B;
    rom[2][3] = 0xBA;
    rom[2][4] = 0xD9;
    rom[2][5] = 0xE4;
    //ERL
    rom[2][6] = 0xF4;
    rom[2][7] = 0x78;
    rom[2][8] = 0xB4;
    rom[2][9] = 0xBA;
    rom[2][10] = 0xDA;
    rom[2][11] = 0x6C;
    
    //Display invert ML routine
    rom[3][0] = 72;
    rom[3][1] = 118;
    rom[3][2] = 74;
    rom[3][3] = 0;
    rom[3][4] = 5;
    rom[3][5] = 189;
    rom[3][6] = 255;
    rom[3][7] = 65;
    rom[3][8] = 78;
    rom[3][9] = 78;
    rom[3][10] = 153;
    rom[3][11] = 8;
    rom[3][12] = 76;
    rom[3][13] = 119;
    rom[3][14] = 139;
    rom[3][15] = 6;
    rom[3][16] = 72;
    rom[3][17] = 119;
    rom[3][18] = 74;
    rom[3][19] = 0;
    rom[3][20] = 158;
    rom[3][21] = 18;
    rom[3][22] = 154;
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

void WriteStatus(uint8 buffer[16][256], uint8 status) 
{
    buffer[EXP_INSTRUCTION_PAGE][EXP_INSTRUCTION_ADDRESS] = status;
}    

void DoCommand(uint8 req, uint8 buffer[16][256]) __attribute__((noinline));        

void DoCommand(uint8 req, uint8 buffer[16][256])
{
    Pin_Data_Write(EXP_STATUS_BUSY);
    switch (req) {
        case EXP_COMMAND_GET_SD_FREE_SPACE:
            {
                WriteStatus(buffer, EXP_STATUS_BUSY);
                uint32 freeSpace = FS_GetVolumeFreeSpace("PC1500");
                LongToBuffer(buffer, EXP_BUFFER_START_PAGE, EXP_BUFFER_START_ADDRESS, freeSpace);
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
                    WriteStatus(buffer, EXP_STATUS_ERROR);
                else 
                    currentFileStatus = EXP_SD_FILE_STATUS_OPEN_WRITE;
                    WriteStatus(buffer, EXP_STATUS_SUCCESS);
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

void DMA_1Init()
{
    /* Variable declarations for DMA_1 */
    /* Move these variable declarations to the top of the function */
    uint8 DMA_1_Chan;
    uint8 DMA_1_TD[1];

    /* DMA Configuration for DMA_1 */
    DMA_1_Chan = DMA_1_DmaInitialize(DMA_1_BYTES_PER_BURST, DMA_1_REQUEST_PER_BURST, 
        HI16(DMA_1_SRC_BASE), HI16(DMA_1_DST_BASE));
    
    //Data TD
    DMA_1_TD[0] = CyDmaTdAllocate();
    CyDmaTdSetConfiguration(DMA_1_TD[0], 1, DMA_1_TD[0], 0);
    //CyDmaTdSetConfiguration(DMA_1_TD[0], 1, CY_DMA_DISABLE_TD, 0);
    //CyDmaTdSetAddress(DMA_1_TD[0], LO16((uint32)&Pin_Data_PS), LO16(((uint32)buffer+(((uint32)Pin_Address_High_PS & 15) << 8) + (uint32)Pin_Address_Low_PS)));
    CyDmaTdSetAddress(DMA_1_TD[0], LO16((uint32)&Pin_Data_PS), LO16((uint32)&DMAData));

    CyDmaChSetInitialTd(DMA_1_Chan, DMA_1_TD[0]);
    CyDmaChEnable(DMA_1_Chan, 1);
    
    DMATD = DMA_1_TD[0];
}

void DMA_AddressInit(uint8 DMADataTD)
{
    /* Variable declarations for DMA_1 */
    /* Move these variable declarations to the top of the function */
    uint8 DMA_Address_Chan;
    uint8 DMA_Address_TD[2];

    /* DMA Configuration for DMA_Address */
    DMA_Address_Chan = DMA_Address_DmaInitialize(2, DMA_1_REQUEST_PER_BURST, 
        HI16(DMA_1_SRC_BASE), HI16(CYDEV_PHUB_TDMEM0_BASE));

    // Get TDs
    DMA_Address_TD[0] = CyDmaTdAllocate();
    DMA_Address_TD[1] = CyDmaTdAllocate();

    // Low Address TD
    CyDmaTdSetConfiguration(DMA_Address_TD[1], 1, DMA_Address_TD[0], 0);
    CyDmaTdSetAddress(DMA_Address_TD[1], LO16((uint32)&Pin_Address_Low_PS), LO16((uint32)&DMAC_TDMEM[DMADataTD].TD1[2]));

    // Page TD
    CyDmaTdSetConfiguration(DMA_Address_TD[0], 1, DMA_Address_TD[1], 0);
    CyDmaTdSetAddress(DMA_Address_TD[0], LO16((uint32)&Status_Page_Status), LO16((uint32)&DMAC_TDMEM[DMADataTD].TD1[3]));

    CyDmaChSetInitialTd(DMA_Address_Chan, DMA_Address_TD[0]);
    CyDmaChEnable(DMA_Address_Chan, 1);
}

int main(void)
{
    //Set busy status until we're done with init
    Pin_Data_DR = EXP_STATUS_BUSY;
    
    uint8 laddress = 0;
    uint8 page = 0;
    asm (".set buffer, 0x20002000");
    extern uint8 buffer[16][256];
    //uint8 buffer[16][256];
    InitBuffer(buffer);
        
    CyGlobalIntEnable; /* Enable global interrupts. */
    
    DMA_1Init();
    DMA_AddressInit(DMATD);
    FS_Init();
    FS_X_AddDevices();
    
    for(;;)
    {        
        if (Status_CS_Read())
        {
            laddress = Pin_Address_Low_PS;
            page = Pin_Address_High_PS & 15;
            
            /*
            if (page > 11)
                CY_SET_REG16(DMADestAddress, LO16(buff+page*256+laddress));
            else
                CY_SET_REG16(DMADestAddress, LO16(dummy));
            */
            
            //It's always safe to do this, because strong drive of the data
            //pins is enabled by CS and 'OD
            //if (page < 15)
            //Pin_Data_DR = Status_Page_Status;
                Pin_Data_DR = buffer[page][laddress];
            //else
            //    Pin_Data_DR = debug;
        }
    }        
}


/* [] END OF FILE */
