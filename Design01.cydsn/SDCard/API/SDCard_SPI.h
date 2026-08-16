//*****************************************************************************
//*****************************************************************************
//  FILENAME: SPIM_SD_SPI.h
//  Version `$CY_MAJOR_VERSION`.`$CY_MINOR_VERSION`
//  
//
//  DESCRIPTION:
//    SDCard User Module low level SPI header file.
//-----------------------------------------------------------------------------
//      Copyright (c) Cypress Semiconductor 2013-14. All Rights Reserved.
//*****************************************************************************
//*****************************************************************************
#ifndef SPIM_SD_SPI_HEADER
#define SPIM_SD_SPI_HEADER

#include "cytypes.h"
#include "CyLib.h"

//#include "SPIM_SD.h"
#include "project.h"
#include "Pin_MISO.h"
#include "Pin_MOSI.h"
#include "Pin_SS.h"    
/*
#include "SPIM_SD_SD_CS.h"
#include "SPIM_SD_SD_PWR.h"
#include "SPIM_SD_SD_WP.h"
#include "SPIM_SD_SPIM.h"
*/
    
//-------------------------------------------------
// Prototypes of the SDCard API.
//-------------------------------------------------
extern void    SPIM_SD_InitHdwr(uint8  bConfiguration);
extern void    SPIM_SD_UnInitHdwr(void);
//void  SPIM_SD_Select(uint8 bEnable);
extern void    SPIM_SD_Select(uint8  bEnable);
extern void    SPIM_SD_SendTxData(uint8  bTxData);
extern uint8   SPIM_SD_bReadRxData(void);
extern uint8   SPIM_SD_bReadStatus(void);
extern void    SPIM_SD_WriteBuff256(char * sRamBuff);
extern void    SPIM_SD_ReadBuff256(char * sRamBuff);
extern void    SPIM_SD_WriteBuff(char * sRamBuff, uint8  bCnt);
extern void    SPIM_SD_ReadBuff(char * sRamBuff, uint8  bCnt);



//-------------------------------------------------
// Constants for SDCard API's.
//-------------------------------------------------

#define SPIM_SD_ENABLE   1
#define SPIM_SD_DISABLE  0

//*******************************
// SPI Configuration definitions
//*******************************
#define  SPIM_SD_SPIM_MODE_0            0x00      // MODE 0 - Leading edge latches data - pos clock
#define  SPIM_SD_SPIM_MSB_FIRST         0x00      // MSB bit transmitted/received first

//********************************
// SPI Status register masks
//********************************
#define  SPIM_SD_SPIM_RX_OVERRUN_ERROR  0x20      // Overrun error in received data
#define  SPIM_SD_SPIM_TX_BUFFER_EMPTY   0x02      // TX Buffer register is ready for next data byte
#define  SPIM_SD_SPIM_RX_BUFFER_FULL    0x08      // RX Buffer register has received current data
#define  SPIM_SD_SPIM_SPI_COMPLETE      0x01      // SPI Tx/Rx cycle has completed

#endif
// end of file SPIM_SD_SPI.h
