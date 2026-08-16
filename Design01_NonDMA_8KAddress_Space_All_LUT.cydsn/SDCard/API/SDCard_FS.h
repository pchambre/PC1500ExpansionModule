//*****************************************************************************
//*****************************************************************************
//  FILENAME: SPIM_SD_FS.h
//  Version `$CY_MAJOR_VERSION`.`$CY_MINOR_VERSION`
//  
//
//  DESCRIPTION: SDCard User Module C Language header file for the 
//               PSoC family of devices.  This header file contains the low
//               level file function calls not intended to be used by the
//               programmer.  See the SPIM_SD.h file for a list of
//               user APIs.
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

#ifndef SPIM_SD_FS_HEADER
#define SPIM_SD_FS_HEADER

//#include "SPIM_SD.h"
#include "project.h"
/*#include "SPIM_SD_SD_CS.h"
#include "SPIM_SD_SD_PWR.h"
#include "SPIM_SD_SD_WP.h"
*/
#include "cytypes.h"
#include "CyLib.h"
#include "Pin_MISO.h"
#include "Pin_MOSI.h"
#include "Pin_SS.h"


//---------------------------------
// Type #defines used for this UM
//---------------------------------
#define uchar   unsigned char   
#define uint    unsigned int
#define ulong   uint32


//--------------------------------------------------------------
// Optional card Socket functions
//  NOTE: selected by Module Parameters "SPIM_SD_PRESENT" and 
//        "SPIM_SD_WPROT" being set to any PSOC Pin. Disabled if 
//          Parameter set to NONE
//--------------------------------------------------------------
#define SPIM_SD_ENABLE_PRESENT    0
#define SPIM_SD_ENABLE_WPROTECT   0

#define SPIM_SD_CSPort    SPIM_SD_SD_CS_Read()
#define SPIM_SD_CSMask    1

#if ( SPIM_SD_ENABLE_PRESENT == 1 )
#define SPIM_SD_CPPort    Pin_MISO_Read()
#define SPIM_SD_CPMask    1
#endif

#if ( SPIM_SD_ENABLE_WPROTECT == 1 )
#define SPIM_SD_WPPort    SPIM_SD_SD_WP_Read()
#define SPIM_SD_WPMask    1
#endif

#define SPIM_SD_ENABLE  1
#define SPIM_SD_DISABLE 0


//==============================================================
//  Define Resource usage configuration option
// Only one of these options should be use at a time 
//==============================================================
//#define SPIM_SD_Build_Config_Level   `@BUILD_CONFIG`
#define ENABLE_FULL_FILE_SYSTEM         0
#define ENABLE_STANDARD_FILE_SYSTEM     1
#define ENABLE_BASIC_FILE_SYSTEM        2
#define ENABLE_READONLY_FILE_SYSTEM     3
#define ENABLE_BASIC_READWRITE          4
#define ENABLE_CUSTOM_CONFIGURATION     5

//==============================================================
// This section works with the "Resource usage configuration"
// parameter to enable the correct options.
//==============================================================
/*
#if  ( ENABLE_CUSTOM_CONFIGURATION == SPIM_SD_Build_Config_Level )
// Not used. Will not work.
// #include "SPIM_SD_Config.h"
#else
*/
// Maximum number of files that can be open at one time
#define SPIM_SD_MAXFILES 2
#endif

//==============================================================
// The following list of #defines can be use to create Custom
// configurations Just set the "Resource usage configuration"
// parameter to "CUSTOM" then add a header file named SPIM_SD_Config.h
// then place any of the #defines below to create you configuration
//==============================================================
//--------------------------------------------------------------
// File systems to support if ENABLE_FILESYSTEM defined 
// Note: If file systems enabled - FAT16 is enabled
//--------------------------------------------------------------
//#define ENABLE_FILESYSTEM
//#define ENABLE_FAT32
//--------------------------------------------------------------
// Write supported
//#define ENABLE_WRITE
//--------------------------------------------------------------
// Enable subdirectory feautures (Not yet developed)
//#define ENABLE_SUBDIR
//--------------------------------------------------------------
// High level File commands
//#define ENABLE_FILECOPY
//#define ENABLE_FILERENAME
//#define ENABLE_FILEREMOVE
//--------------------------------------------------------------
// DEBUG FUNCTIONS
//#define ENABLE_DEBUGFUNCT
//==============================================================



#if ( ENABLE_FULL_FILE_SYSTEM == SPIM_SD_Build_Config_Level )
#define ENABLE_FILESYSTEM
#define ENABLE_FAT32
#define ENABLE_WRITE   
//#define ENABLE_SUBDIR       // Not yet developed 
#define ENABLE_FILECOPY
#define ENABLE_FILERENAME
#define ENABLE_FILEREMOVE
//#define ENABLE_DEBUGFUNCT   // Only NEED for debug
#endif

#if ( ENABLE_STANDARD_FILE_SYSTEM == SPIM_SD_Build_Config_Level )
#define ENABLE_FILESYSTEM
//#define ENABLE_FAT32
#define ENABLE_WRITE    
//#define ENABLE_SUBDIR       // Not yet developed 
#define ENABLE_FILECOPY
#define ENABLE_FILERENAME
#define ENABLE_FILEREMOVE
//#define ENABLE_DEBUGFUNCT   // Only NEED for debug
#endif

#if ( ENABLE_BASIC_FILE_SYSTEM == SPIM_SD_Build_Config_Level )
#define ENABLE_FILESYSTEM
//#define ENABLE_FAT32
#define ENABLE_WRITE    
//#define ENABLE_SUBDIR      // Not yet developed 
//#define ENABLE_FILECOPY
//#define ENABLE_FILERENAME
#define ENABLE_FILEREMOVE
//#define ENABLE_DEBUGFUNCT   // Only NEED for debug
#endif

#if ( ENABLE_READONLY_FILE_SYSTEM == SPIM_SD_Build_Config_Level )
#define ENABLE_FILESYSTEM
//#define ENABLE_FAT32
//#define ENABLE_WRITE    
//#define ENABLE_SUBDIR       // Not yet developed 
//#define ENABLE_FILECOPY
//#define ENABLE_FILERENAME
//#define ENABLE_FILEREMOVE
//#define ENABLE_DEBUGFUNCT   // Only NEED for debug
#endif

#if ( ENABLE_BASIC_READWRITE == SPIM_SD_Build_Config_Level )
//#define ENABLE_FILESYSTEM
//#define ENABLE_FAT32
#define ENABLE_WRITE    
//#define ENABLE_SUBDIR       // Not yet developed 
//#define ENABLE_FILECOPY
//#define ENABLE_FILERENAME
//#define ENABLE_FILEREMOVE
//#define ENABLE_DEBUGFUNCT   // Only NEED for debug
#endif

//==============================================================
#define SPIM_SD_ON      1
#define SPIM_SD_OFF     0

//==============================================================

//----------------------------
// R1 mask defines
//----------------------------
#define   SPIM_SD_IDLE              0x01
#define   SPIM_SD_ERASE_CMD         0x02
#define   SPIM_SD_ILLEGAL_CMD       0x04
#define   SPIM_SD_CRC_ERROR         0x08
#define   SPIM_SD_ERASE_SEQ_ERROR   0x10
#define   SPIM_SD_ADDR_ERROR        0x20
#define   SPIM_SD_PARAM_ERROR       0x40

//----------------------------
// SPIM_SD_Status defines (SD card)
//----------------------------
// IFN   Invalid File Name
// FNF   File Not Found
// PRE   Parameter Range Error
// CE    Card Error
// FFE   File Format Error
// WRE   Write Error (Unused)
// FPE   File Pointer Error
// EOF   End Of File
//----------------------------
#define SPIM_SD_IFN          0x01   
#define SPIM_SD_FNF          0x02
#define SPIM_SD_PRE          0x04
#define SPIM_SD_CE           0x08
#define SPIM_SD_FFE          0x10
#define SPIM_SD_FPE          0x20
#define SPIM_SD_WRE          0x40
#define SPIM_SD_EOF          0x80

// System defines
#define SPIM_SD_FAIL         0xAA
#define SPIM_SD_PASS         0x00
#define SPIM_SD_XCARD        0x0F
#define SPIM_SD_XFAT         0xF0
#define SPIM_SD_UNUSED       0xFFFF

//----------------------------
// Card command defines
//----------------------------
#define SPIM_SD_CMD0         0x40
#define SPIM_SD_CMD1         0x41
#define SPIM_SD_CMD9         0x49
#define SPIM_SD_CMD10        0x4A
#define SPIM_SD_CMD12        0x4C
#define SPIM_SD_CMD13        0x4D
#define SPIM_SD_CMD16        0x50
#define SPIM_SD_CMD17        0x51
#define SPIM_SD_CMD18        0x52
#define SPIM_SD_CMD24        0x58
#define SPIM_SD_CMD25        0x59
#define SPIM_SD_CMD27        0x5B
#define SPIM_SD_CMD28        0x5C
#define SPIM_SD_CMD29        0x5D
#define SPIM_SD_CMD30        0x5E
#define SPIM_SD_CMD32        0x60
#define SPIM_SD_CMD33        0x61
#define SPIM_SD_CMD38        0x66
#define SPIM_SD_CMD55        0x77
#define SPIM_SD_CMD56        0x78
#define SPIM_SD_CMD58        0x7A
#define SPIM_SD_CMD59        0x7B

#define SPIM_SD_ACMD13       0x4D
#define SPIM_SD_ACMD18       0x52
#define SPIM_SD_ACMD22       0x56
#define SPIM_SD_ACMD23       0x57
#define SPIM_SD_ACMD25       0x59
#define SPIM_SD_ACMD26       0x5A
#define SPIM_SD_ACMD38       0x66
#define SPIM_SD_ACMD41       0x69
#define SPIM_SD_ACMD42       0x6A
#define SPIM_SD_ACMD51       0x73

#define SPIM_SD_NOARGS       0x00000000

//--------------------------------
// Declare external variables
//--------------------------------
#ifdef ENABLE_FILESYSTEM
#ifdef ENABLE_FAT32
extern ulong   SPIM_SD_CurStart[SPIM_SD_MAXFILES+1];   // Starting FAT entry value for file
extern ulong   SPIM_SD_CurFat[SPIM_SD_MAXFILES+1];     // Current FAT entry value of cluster
#else
extern uint    SPIM_SD_CurStart[SPIM_SD_MAXFILES+1];   // Starting FAT entry value for file
extern uint    SPIM_SD_CurFat[SPIM_SD_MAXFILES+1];     // Current FAT entry value of cluster
#endif  // End ENABLE_FAT32

extern uchar   SPIM_SD_CurSect[SPIM_SD_MAXFILES+1];    // Current sector of the current cluster
extern uchar   SPIM_SD_CurAttr[SPIM_SD_MAXFILES];      // Current file attributes - Bits 6 and seven are unused
extern uchar   SPIM_SD_FileError[SPIM_SD_MAXFILES];    // Current error flags for file
extern uchar   SPIM_SD_FileMode[SPIM_SD_MAXFILES];     // Current Mode bits = "r"=0, "w"=1, "a"=2, "+"=3
#endif // End ENABLE_FILESYSTEM

extern ulong   SPIM_SD_CurSize[SPIM_SD_MAXFILES+1];    // Current file size (in bytes)
extern ulong   SPIM_SD_CurOffset[SPIM_SD_MAXFILES+1];  // Current offset into file

//extern uchar   Status;               // Temp value for returned status byte
extern uchar   SPIM_SD_CardType;               // Card Type:    0=None,   1=MMC,    2=SD (lower nibble)
                                                        // FAT Type:     0=None,   10=FAT12, 20=FAT16, 30=FAT32 (upper nibble)
extern uchar   *SPIM_SD_Buffer1;               // Temporary buffer pointer for data reads and writes (part 1)
extern uchar   *SPIM_SD_Buffer2;               // Temporary buffer pointer for data reads and writes (part 2)

#ifdef ENABLE_FILESYSTEM
extern uchar   SPIM_SD_ClusterSize;            // Cluster size in sectors
extern ulong   SPIM_SD_Fat1Start;              // Start of 1st FAT Table (in bytes)
extern ulong   SPIM_SD_Fat2Start;              // Start of 2nd FAT Table (in bytes)
extern ulong   SPIM_SD_DirStart;               // Start of Root directory structure
#endif

extern ulong   SPIM_SD_DataStart;              // Start of Data area

//---------------------------------------------------------------------------------------------
//                                   Function Prototypes
//---------------------------------------------------------------------------------------------


//---------------------------
// Low level card commands
//---------------------------
void  SPIM_SD_Cmd(uchar CmdNum, ulong Param);   // Sends out the command string including parameters  
uchar SPIM_SD_GetR1(void);                      // Returns R1 type command responsed  
uint  SPIM_SD_GetR2(void);                      // Returns R2 type command responsed  
void  SPIM_SD_EndCmd(void);                     // Sends out the end of acommand string  
void  SPIM_SD_Wait(void);                       // Wait a small amount of time  (Value not final)  
void  SPIM_SD_Wait2(void);                      // Wait for approximately 50ms  (Value not final)  
uchar SPIM_SD_Cmd_00(void);                     // Resets Card to power up condition  
uchar SPIM_SD_Cmd_01(void);                     // Runs an MMC card's internal init routine  
uint  SPIM_SD_Cmd_13(void);                     // Returns the card status response 
uchar SPIM_SD_ACmd_41(void);                    // Runs an SD  card's internal init routine  
uchar SPIM_SD_Cmd_55(void);                     // Make the next command an App command  
uchar SPIM_SD_CheckReply(uchar check);          // Compare check value to response  
uchar SPIM_SD_SetSize(ulong size);              // Set sector size  
ulong SPIM_SD_GetTable(void);                   // Get Partition Table information and return boot sector start  
void  SPIM_SD_XferWait(void);                   // Wait until transfer is complete
void  SPIM_SD_IncOffset(uchar Fptr, uchar mode); // Increment file position for CurOffset 

//---------------------------
// Low level file commands
//---------------------------
uchar   SPIM_SD_ReadSect(ulong address);

#ifdef ENABLE_FILESYSTEM
uchar SPIM_SD_GetBoot(ulong BootStart);        // Get boot sector information and put it in Buffer  
void  SPIM_SD_NextFat(uchar Fptr);             // Loads CurFat[Fptr] with next FAT entry or FatEnd for end  

//------------------------------------------------
// Low level commands affecting FAT16/32 switches
//------------------------------------------------
#ifdef ENABLE_FAT32
ulong   SPIM_SD_FatTrack(ulong FatNum);          // Returns the next FAT entry in the chain.  
ulong   SPIM_SD_LastFat(uchar Fptr);             // Returns the last FAT entry used in a file chain  
ulong   SPIM_SD_GetFatAddr(ulong Fat);           // Return the sector address for the FAT entry  
char  * SPIM_SD_ReadFatSect(ulong Fat);          // Read FAT entry sector into buffer and return buffer pointer  
#else
uint    SPIM_SD_FatTrack(uint FatNum);           // Returns the next FAT entry in the chain.  
uint    SPIM_SD_LastFat(uchar Fptr);             // Returns the last FAT entry used in a file chain  
ulong   SPIM_SD_GetFatAddr(uint Fat);            // Return the sector address for the FAT entry  
char  * SPIM_SD_ReadFatSect(uint Fat);           // Read FAT entry sector into buffer and return buffer pointer  
#endif  // ENABLE_FAT32

uchar   SPIM_SD_NextSect(uchar Fptr);                // Loads CurSect[Fptr] with next sector entry and sets FAT if needed  
uchar   SPIM_SD_FindFile(uchar Fptr, uint FileNum);  // Find File in root directory for file pointer  
uchar   SPIM_SD_ReadFileSect(uchar Fptr);            // Read a sector of a file   
void    SPIM_SD_GetAddress(uchar Fptr, uchar mode);  // Calculate the address based on the current file/sub-directory position - mode 0=sector, 1=sect and offset  
uchar * SPIM_SD_GetBuffPtr(uint Offset);         // Returns the pointer for read/write buffer offset  
char  * SPIM_SD_ReadDirSect(uchar Fptr);         // Read directory entry sector into buffer and return buffer pointer  
uchar   SPIM_SD_ParseFilename(uchar * Filename); // Parse the filename string and convert it into 8x3 dir format  

//--------------------------------
// Low level file write commands
//--------------------------------

#ifdef ENABLE_WRITE
void  SPIM_SD_CopyDir(uchar Fptr1, uchar Fptr2 ); // Copy file directory information from file 1 to file 2  
void  SPIM_SD_UpdateDir(uchar Fptr);              // Update the directory entry from the current file info  
void  SPIM_SD_WriteDirSect(uchar Fptr);           // Write directory entry sector from the buffer  
uchar SPIM_SD_NewFile(uchar Fptr, uchar Filename[]);   // Create new file using filename   
uchar SPIM_SD_FileZero(uchar Fptr, uchar Del);    // Sets file size to zero and resets FAT entries or deletes file 
uchar SPIM_SD_WriteFileSect(uchar Fptr);          // Write a sector to a file  
uchar SPIM_SD_AddFat(uchar Fptr);                 // Add a new FAT entry to the FAT chain, or create one  

#ifdef ENABLE_FAT32
uchar SPIM_SD_FatReclaim(ulong FirstFat);         // Zero FAT chain starting at first FAT entry  
void  SPIM_SD_WriteFatSect(ulong Fat);            // Write FAT entry sector from the buffer  
#else
uchar SPIM_SD_FatReclaim(uint FirstFat);          // Zero FAT chain starting at first FAT entry  
void  SPIM_SD_WriteFatSect(uint Fat);             // Write FAT entry sector from the buffer  
#endif   // ENABLE_FAT32
#endif   // End ENABLE_WRITE
#endif   // End ENABLE_FILESYSTEM

// Debug functions
#ifdef ENABLE_DEBUGFUNCT
uchar SPIM_SD_ACmd_13(void);              // Get SD card status command  
uchar SPIM_SD_GetCID(void);               // Send out command to read CID data into buffer  
uchar SPIM_SD_GetOCR(void);               // Read card's OCR register routine  
uchar SPIM_SD_GetCSD(void);               // Send out command to read CSD data into buffer  
uchar SPIM_SD_ReadByte(ulong address);    // Return the byte pointed to by the absolute address
#endif

//#endif
// TEST ONLY
