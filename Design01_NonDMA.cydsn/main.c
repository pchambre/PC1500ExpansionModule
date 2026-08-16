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
     
    /*
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
    */
    
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
    
    //Memcopy routine (two bytes per input value, big endian)
    //source start address at &8F00
    //dest start address at &8F02
    //number of bytes to copy at &8F04
    rom[4][0]=0xA5; //LDA from memory
    rom[4][1]=0x8F;
    rom[4][2]=0x00;
    rom[4][3]=0x08;//STX - high byte of source start address
    rom[4][4]=0xA5; //LDA from memory
    rom[4][5]=0x8F;
    rom[4][6]=0x01;
    rom[4][7]=0x0A;//STX - low byte of source start address
    rom[4][8]=0xA5; //LDA from memory
    rom[4][9]=0x8F;
    rom[4][10]=0x02;
    rom[4][11]=0x18;//STX - high byte of dest start address
    rom[4][12]=0xA5; //LDA from memory
    rom[4][13]=0x8F;
    rom[4][14]=0x03;
    rom[4][15]=0x1A;//STX - low byte of dest start address
    rom[4][16]=0xA5; //LDA from memory
    rom[4][17]=0x8F;
    rom[4][18]=0x04;
    rom[4][19]=0x28;//STX - high byte of byte count
    rom[4][20]=0xA5; //LDA from memory
    rom[4][21]=0x8F;
    rom[4][22]=0x05;
    rom[4][23]=0x2A;//STX - low byte of byte count
    //Setup done    
    rom[4][24]=0xF5; //TIN - transfer one byte source to dest and increment X and Y
    rom[4][25]=0x66; //Decrement U (byte count)
    rom[4][26]=0x6C; //Compare UH with 0
    rom[4][27]=0x00; //Zero
    rom[4][28]=0x99; //Jump back n bytes if UH was not 0
    rom[4][29]=0x06; //Number of bytes to skip back when jumping
    rom[4][30]=0x6E; //Compare UL with 0 (only when UH is already 0)
    rom[4][31]=0x00; //Zero
    rom[4][32]=0x99; //Jump back n bytes if UL was not 0
    rom[4][33]=0x0A; //Number of bytes to skip back when jumping
    rom[4][34]=0x9A; //Return

	rom[0][0]=0x55;
	rom[0][2]=0x53;
	rom[0][3]=0x44;
	rom[0][4]=0x43;
	rom[0][5]=0x41;
	rom[0][6]=0x52;
	rom[0][7]=0x44;
	rom[0][8]=0x0d;
	rom[0][10]=0x9a;
	rom[0][11]=0x9a;
	rom[0][12]=0x9a;
	rom[0][13]=0x9a;
	rom[0][14]=0x9a;
	rom[0][15]=0x9a;
	rom[0][16]=0x9a;
	rom[0][17]=0x9a;
	rom[0][18]=0x9a;
	rom[0][19]=0x9a;
	rom[0][20]=0x9a;
	rom[0][21]=0x9a;
	rom[0][30]=0xc4;
	rom[0][31]=0xaf;
	rom[0][32]=0xff;
	rom[0][56]=0x80;
	rom[0][57]=0x56;
	rom[0][84]=0xd5;
	rom[0][85]=0x4d;
	rom[0][86]=0x4d;
	rom[0][87]=0x43;
	rom[0][88]=0x50;
	rom[0][89]=0x59;
	rom[0][90]=0xe0;
	rom[0][91]=0xc0;
	rom[0][92]=0x80;
	rom[0][93]=0x60;
	rom[0][94]=0xd0;
	rom[0][95]=0x00;
	rom[0][96]=0xde;
	rom[0][97]=0x2e;
	rom[0][98]=0xd0;
	rom[0][99]=0x00;
	rom[0][100]=0x2b;
	rom[0][101]=0xf6;
	rom[0][102]=0x78;
	rom[0][103]=0x00;
	rom[0][104]=0xc2;
	rom[0][105]=0x2c;
	rom[0][106]=0x26;
	rom[0][107]=0xde;
	rom[0][108]=0x23;
	rom[0][109]=0xd0;
	rom[0][110]=0x00;
	rom[0][111]=0x20;
	rom[0][112]=0xf6;
	rom[0][113]=0x78;
	rom[0][114]=0x02;
	rom[0][115]=0xc2;
	rom[0][116]=0x2c;
	rom[0][117]=0x1b;
	rom[0][118]=0xde;
	rom[0][119]=0x18;
	rom[0][120]=0xd0;
	rom[0][121]=0x00;
	rom[0][122]=0x15;
	rom[0][123]=0xfd;
	rom[0][124]=0x98;
	rom[0][125]=0xcc;
	rom[0][126]=0x02;
	rom[0][127]=0xfd;
	rom[0][128]=0x5a;
	rom[0][129]=0xcc;
	rom[0][130]=0x00;
	rom[0][131]=0x62;
	rom[0][132]=0x83;
	rom[0][133]=0x04;
	rom[0][134]=0xfd;
	rom[0][135]=0x62;
	rom[0][136]=0x81;
	rom[0][137]=0x03;
	rom[0][138]=0xf5;
	rom[0][139]=0x9e;
	rom[0][140]=0x0a;
	rom[0][141]=0xfd;
	rom[0][142]=0x1a;
	rom[0][143]=0xe2;
	rom[0][144]=0xe0;
	rom[0][145]=0xe4;

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

int main(void)
{
    //Set busy status until we're done with init
    Pin_Data_DR = EXP_STATUS_BUSY;
    
    uint8 laddress = 0;
    uint8 page = 0;
    uint8 buffer[16][256];
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
            if (!(rwpin = Pin_RW_Read()))
            {    
                if (page == EXP_INSTRUCTION_PAGE && laddress == EXP_INSTRUCTION_ADDRESS) 
                {
                    DoCommand(data_read, buffer);
                }
                else if (page > 11)
                    buffer[page][laddress] = data_read;
            }
            else 
            {
                //Try to wait in case RW goes low later
                while (page == Status_Page_Status && Pin_Address_Low_PS == laddress && (rwpin = Pin_RW_Read())) 
                {
                }
                if (!rwpin)
                {
                    if (page == EXP_INSTRUCTION_PAGE && laddress == EXP_INSTRUCTION_ADDRESS) 
                    {
                        DoCommand(data_read, buffer);
                    }
                    else if (page > 11) 
                        buffer[page][laddress] = data_read;
                }
            }                        
        }
    }        
}


/* [] END OF FILE */
