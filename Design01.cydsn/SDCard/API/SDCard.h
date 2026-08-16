//*****************************************************************************
//*****************************************************************************
//  FILENAME: SPIM_SD.h
//  Version `$CY_MAJOR_VERSION`.`$CY_MINOR_VERSION`
//  
//
//  DESCRIPTION: SDCard User Module C Language header file for the 
//               PSoC family of devices.  This header file contains only
//               the functions that are should be used by the programmer.
//
//-----------------------------------------------------------------------------
//  The original source for this user module was purchased from
//  Efficient Computer Systems, LLC.
//-----------------------------------------------------------------------------
//      
//  Copyright 2003-2006   Efficient Computer Systems, LLC
//  Licensed only for use on any Cypress PSOC Mixed-Signal Controllers.
//  All rights reserved
//      
//-----------------------------------------------------------------------------
//   
//         Created 12-04-03   By: Lee W. Morin and Herb Winters
//  1.00   Release 04-24-06   By: Lee Morin, Herb Winters, Eric Curtis   
//   
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
//  Copyright (c) Cypress Semiconductor 2013-14. All Rights Reserved.
//*****************************************************************************
//*****************************************************************************
#if !defined(SPIM_SD_HEADER)
#define SPIM_SD_HEADER

//#include "SPIM_SD.h"
//#include "project.h"
#include "SDCard_FS.h"
#include "SDCard_SPI.h"

/*    
#include "SPIM_SD_SD_CD.h"
#include "SPIM_SD_SD_CS.h"
#include "SPIM_SD_SD_PWR.h"
#include "SPIM_SD_SD_WP.h"
*/

    
    
#include "CyLib.h"

//---------------------------------------------------------------------------------------------
//                                   Function Prototypes
//---------------------------------------------------------------------------------------------

//---------------------------------------------------------------------------------------------
//                                Basic Read Write commands
//---------------------------------------------------------------------------------------------

void  SPIM_SD_CustomStart(void);                       // Starts SD card module  
void  SPIM_SD_CustomStop(void);                        // Stops  SD card module  
uchar SPIM_SD_InitCard(void);                    // Runs all commands to init card for use  
uchar SPIM_SD_fseek(uchar Fptr, ulong Offset);   // Seeks a specific offet into file  
uchar SPIM_SD_fgetc(uchar Fptr);                 // Returns the next character from the file pointed to 
uchar SPIM_SD_fbgetc(uchar Fptr);                // Returns the next buffered character from the file pointed to 
void  SPIM_SD_clearerr(uchar Fptr);              // Clears the error flags for the file    
uchar SPIM_SD_ferror(uchar Fptr);                // Returns non-zero for file error, zero if no error  
ulong SPIM_SD_ftell(uchar Fptr);                 // Return the current postion within the file  
uchar SPIM_SD_ReadSect(ulong address);           // Read a sector   

#ifdef SPIM_SD_ENABLE_PRESENT
uchar SPIM_SD_Present(void);                     // Returns a '1' if a card is present in the socket, '0' if not  
#endif

#ifdef SPIM_SD_ENABLE_WPROTECT
uchar SPIM_SD_WriteProtect(void);                // Returns a '1' if the card is write protected using ther slide switch, '0' if not
#endif

#ifdef ENABLE_WRITE
uchar SPIM_SD_WriteSect(ulong address);            // Write a sector  
uchar SPIM_SD_fputc(uchar Data, uchar Fptr);       // Write a character to a file  
uchar SPIM_SD_fputs(char *str, uchar Fptr);        // Writes a null-terminated string to a file. 
uchar SPIM_SD_fputcs(const char *str, uchar Fptr); // Writes a null-terminated const string to a file. 
uchar SPIM_SD_fputBuff(uchar *buff, uint count, uchar Fptr);
uchar SPIM_SD_fputcBuff(const uchar *buff, uint count, uchar Fptr);
void  SPIM_SD_fflush(uchar Fptr);                  // Flush the write buffers (to the file and update dir values in FILESYSTEM mode)  
#endif

//---------------------------
// Top level file functions
//---------------------------
#ifdef ENABLE_FILESYSTEM
uchar   SPIM_SD_fclose(uchar Fptr);                // Closes a file pointer  
uchar   SPIM_SD_fopen(uchar Filename[], const uchar Mode[]);   // Opens a file as type and returns the file pointer 
uchar * SPIM_SD_GetFilename(uint Entry);           // Returns filename for directory entry specified.  
uint    SPIM_SD_GetFileCount(void);                // Returns the number of valid files in the root directory  
ulong   SPIM_SD_GetFileSize(uchar Fptr);           // Returns the file size of the file pointed to  
uchar   SPIM_SD_feof(uchar Fptr);                  // Returns non-zero for EOF, zero if no EOF  

//---------------------------
// Top level writing functions
//---------------------------
#ifdef ENABLE_WRITE
uchar SPIM_SD_Remove(uchar * Filename);         // Delete the file indicated by Filename  
uchar SPIM_SD_Rename(uchar * OldFilename, uchar * NewFilename);  // Rename a file  
uchar SPIM_SD_Copy(uchar * OldFilename, uchar * NewFilename);    // Copy a file  
#endif  // End ENABLE_WRITE
#endif  // End ENABLE_FILESYSTEM


//------------- cardInfo defines ---------------

#define SPIM_SD_FORMAT_FAT12    0x10
#define SPIM_SD_FORMAT_FAT16    0x40
#define SPIM_SD_FORMAT_FAT16a   0x40
#define SPIM_SD_FORMAT_FAT16b   0x60
#define SPIM_SD_FORMAT_FAT32    0xB0
#define SPIM_SD_FORMAT_VALID    0xE0
#define SPIM_SD_FORMAT_MASK     0xF0

#define SPIM_SD_TYPE_NONE       0x00
#define SPIM_SD_TYPE_MMC        0x01
#define SPIM_SD_TYPE_SD         0x02
#define SPIM_SD_TYPE_MASK       0x03

#endif
