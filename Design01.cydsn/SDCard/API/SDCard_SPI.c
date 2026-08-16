/****************************************************************************
*****************************************************************************
  FILENAME: SPIM_SD_SPI.c
  Version `$CY_MAJOR_VERSION`.`$CY_MINOR_VERSION`
  

  DESCRIPTION: SDCard User Module software implementation file
               for the low level SPI hardware.

  NOTE: User Module APIs conform to the fastcall16 convention for marshalling
        arguments and observe the associated "Registers are volatile" policy.
        This means it is the caller's responsibility to preserve any values
        in the X and A registers that are still needed after the API functions
        returns. For Large Memory Model devices it is also the caller's 
        responsibility to perserve any value in the CUR_PP, IDX_PP, MVR_PP and 
        MVW_PP registers. Even though some of these registers may not be modified
        now, there is no guarantee that will remain the case in future releases.
-----------------------------------------------------------------------------
  Copyright (c) Cypress Semiconductor 2013-14. All Rights Reserved.
*****************************************************************************
*****************************************************************************
*/

//#include "project.h"
//#include "SPIM_SD.h"
#include "SDCard_SPI.h"
#include "SDCard.h"

/*
;-----------------------------------------------------------------------------
;  FUNCTION NAME: SPIM_SD_InitHdwr
;
;  DESCRIPTION:
;     Sets the start bit, SPI mode, and LSB/MSB first configuration of the SPIM
;     user module.
;
;     Transmission will begin transmitting when a byte is written into the TX buffer
;     using the SPIM_SD_SendTxData function.
;
;-----------------------------------------------------------------------------
;
;  ARGUMENTS:
;     uint8 bConfiguration - Consists of SPI Mode and LSB/MSB first bit.
;           Use defined masks - masks can be OR'd together.
;     PASSED in Accumulator.
;
;  RETURNS:  none
;
;  SIDE EFFECTS: 
;    The A and X registers may be modified by this or future implementations
;    of this function.  The same is true for all RAM page pointer registers in
;    the Large Memory Model.  When necessary, it is the calling function's
;    responsibility to perserve their values across calls to fastcall16 
;    functions.
;
;  THEORY of OPERATION or PROCEDURE:
;     1) Set all Slave Select outputs high
;     2) Set the specified SPI configuration bits in the Control register.
;
*/
void  SPIM_SD_InitHdwr(uint8 bConfiguration)
{
	Pin_MOSI_Write(0);
	Clock_SD_SetDividerRegister(0, 1);
	SPIM_SD_Start();
	SPIM_SD_ClearTxBuffer();
	SPIM_SD_ClearRxBuffer();
}

/*
;-----------------------------------------------------------------------------
;  FUNCTION NAME: SPIM_SD_Stop
;
;  DESCRIPTION:
;     Disables SPIM operation.
;
;-----------------------------------------------------------------------------
;
;  ARGUMENTS:  none
;
;  RETURNS:  none
;
;  SIDE EFFECTS: 
;    The A and X registers may be modified by this or future implementations
;    of this function.  The same is true for all RAM page pointer registers in
;    the Large Memory Model.  When necessary, it is the calling function's
;    responsibility to perserve their values across calls to fastcall16 
;    functions.
;
;  THEORY of OPERATION or PROCEDURE:
;     Clear the start bit in the Control register.
;
*/
void  SPIM_SD_UnInitHdwr(void)
{
	Pin_MOSI_Write(1);
	SPIM_SD_Select(0);
	SPIM_SD_Stop();
}

/*
;-----------------------------------------------------------------------------
;  FUNCTION NAME: void SPIM_SD_Select(uint8 bEnable)
;
;  DESCRIPTION:
;     Enable or disable card select signal.
;
;-----------------------------------------------------------------------------
;
;  ARGUMENTS:
;     uint8 bEnable => 0 Disable Card
;                  => 1 Enable Card
;     PASSED in Accumulator.
;
;  RETURNS:  none
;
;  SIDE EFFECTS: 
;    The A and X registers may be modified by this or future implementations
;    of this function.  The same is true for all RAM page pointer registers in
;    the Large Memory Model.  When necessary, it is the calling function's
;    responsibility to perserve their values across calls to fastcall16 
;    functions.
;
;  THEORY of OPERATION or PROCEDURE:
;
;-----------------------------------------------------------------------------
*/

void  SPIM_SD_Select(uint8 bEnable)
{
	if(bEnable)
		{
		Pin_SS_Write(0);
		//SPIM_SD_SD_PWR_Write(1);
		}
	else
		{
		Pin_SS_Write(1);
		//SPIM_SD_SD_PWR_Write(0);
		}
}

/*
;-----------------------------------------------------------------------------
;  FUNCTION NAME: SPIM_SD_SendTxData
;
;  DESCRIPTION:
;     Initiates an SPI data transfer.
;
;-----------------------------------------------------------------------------
;
;  ARGUMENTS:
;     uint8  bTxData - data to transmit.
;        Passed in Accumulator.
;
;  RETURNS:  none
;
;  SIDE EFFECTS: 
;    The A and X registers may be modified by this or future implementations
;    of this function.  The same is true for all RAM page pointer registers in
;    the Large Memory Model.  When necessary, it is the calling function's
;    responsibility to perserve their values across calls to fastcall16 
;    functions.
;
;  THEORY of OPERATION or PROCEDURE:
;     Writes data to the TX buffer register.
;
*/
void  SPIM_SD_SendTxData(uint8 bTxData)
{
	SPIM_SD_WriteTxData((uint32) bTxData);
}

/*
;-----------------------------------------------------------------------------
;  FUNCTION NAME: SPIM_SD_WriteBuff256(char * sRamBuff)
;
;  DESCRIPTION:
;     Writes a 256 byte buffer to the SPI port
;
;-----------------------------------------------------------------------------
;
;  ARGUMENTS:
;     A:X  Pointer to String
;          A contains MSB of string address
;          X contains LSB of string address
;
;
;  RETURNS:  none
;
;  SIDE EFFECTS: 
;    The A and X registers may be modified by this or future implementations
;    of this function.  The same is true for all RAM page pointer registers in
;    the Large Memory Model.  When necessary, it is the calling function's
;    responsibility to perserve their values across calls to fastcall16 
;    functions.
;
;
;  THEORY of OPERATION or PROCEDURE:
;     Writes data to the TX buffer register.
;
*/
void  SPIM_SD_WriteBuff256(char * sRamBuff)
{
	uint32 i, temp;
	for(i=0; i<256; i++)
	{
		SPIM_SD_WriteTxData((uint32)sRamBuff[i]);
		while(! (SPIM_SD_GetRxBufferSize() > 0));
		temp = SPIM_SD_ReadRxData();
	}
}

/*
;-----------------------------------------------------------------------------
;  FUNCTION NAME: SPIM_SD_WriteBuff(uint8 * pBuff, uint8 bCnt)
;
;  DESCRIPTION:
;     Writes n bytes to the SPI port
;
;-----------------------------------------------------------------------------
;
;  ARGUMENTS:
;  FASTCALL16 ARGUMENTS:
;
;   [SP-3] => pBuff LSB Address.
;   [SP-4] => pBuff MSB Address.
;   [SP-5] => Buffer length to write.
;
;  RETURNS:  none
;
;  SIDE EFFECTS: 
;    The A and X registers may be modified by this or future implementations
;    of this function.  The same is true for all RAM page pointer registers in
;    the Large Memory Model.  When necessary, it is the calling function's
;    responsibility to perserve their values across calls to fastcall16 
;    functions.
;
;
;  THEORY of OPERATION or PROCEDURE:
;     Writes n data byte to the TX buffer register.
;
*/
void  SPIM_SD_WriteBuff(char * sRamBuff, uint8 bCnt)
{
	uint32 i, temp;
	for(i=0; i<bCnt; i++)
	{
		SPIM_SD_WriteTxData((uint32)sRamBuff[i]);
		while(! (SPIM_SD_GetRxBufferSize() > 0));
		temp = SPIM_SD_ReadRxData();
	}
}

/*
;-----------------------------------------------------------------------------
;  FUNCTION NAME: SPIM_SD_ReadBuff(uint8 * pBuff, uint8 bCnt)
;
;  DESCRIPTION:
;     Reads n bytes from the SPI port
;
;-----------------------------------------------------------------------------
;
;  ARGUMENTS:
;  FASTCALL16 ARGUMENTS:
;
;   [SP-3] => pBuff LSB Address.
;   [SP-4] => pBuff MSB Address.
;   [SP-5] => Buffer length to read (bCnt).
;
;
;  RETURNS:  none
;
;  SIDE EFFECTS: 
;    The A and X registers may be modified by this or future implementations
;    of this function.  The same is true for all RAM page pointer registers in
;    the Large Memory Model.  When necessary, it is the calling function's
;    responsibility to perserve their values across calls to fastcall16 
;    functions.
;
;
;  THEORY of OPERATION or PROCEDURE:
;     Writes data to the TX buffer register.
;
*/
void  SPIM_SD_ReadBuff(char * sRamBuff, uint8 bCnt)
{
	uint32 i;
	for(i=0; i<bCnt; i++)
	{
		SPIM_SD_WriteTxData(0xFF);
		while(! (SPIM_SD_GetRxBufferSize() > 0));
		sRamBuff[i] = (char) SPIM_SD_ReadRxData();
	}
}

/*
;-----------------------------------------------------------------------------
;  FUNCTION NAME: SPIM_SD_ReadBuff256(char * sRamBuff)
;
;  DESCRIPTION:
;     Reads 256 bytes into a buffer from the SPI port
;
;-----------------------------------------------------------------------------
;
;  ARGUMENTS:
;     A:X  Pointer to String
;          A contains MSB of string address
;          X contains LSB of string address
;
;
;  RETURNS:  none
;
;  SIDE EFFECTS: 
;    The A and X registers may be modified by this or future implementations
;    of this function.  The same is true for all RAM page pointer registers in
;    the Large Memory Model.  When necessary, it is the calling function's
;    responsibility to perserve their values across calls to fastcall16 
;    functions.
;
;
;  THEORY of OPERATION or PROCEDURE:
;     Reads data from the TX buffer register.
;
*/
void  SPIM_SD_ReadBuff256(char * sRamBuff)
{
	uint32 i;
	for(i=0; i<256; i++)
	{
		SPIM_SD_WriteTxData(0xFF);
		while(! (SPIM_SD_GetRxBufferSize() > 0));
		sRamBuff[i] = (char) SPIM_SD_ReadRxData();
	}
}

/*
;-----------------------------------------------------------------------------
;  FUNCTION NAME: SPIM_SD_bReadRxData
;
;  DESCRIPTION:
;     Reads the RX buffer register.  Should check the status regiser to make
;     sure data is valid.
;
;-----------------------------------------------------------------------------
;
;  ARGUMENTS:  none
;
;  RETURNS:
;     bRxData - returned in A.
;
;  SIDE EFFECTS: 
;    The A and X registers may be modified by this or future implementations
;    of this function.  The same is true for all RAM page pointer registers in
;    the Large Memory Model.  When necessary, it is the calling function's
;    responsibility to perserve their values across calls to fastcall16 
;    functions.
;
;  THEORY of OPERATION or PROCEDURE:
;
*/
uint8  SPIM_SD_bReadRxData(void)
{
	uint8 buf;
	buf = (uint8) SPIM_SD_ReadRxData();
	return(buf);
}

/*
;-----------------------------------------------------------------------------
;  FUNCTION NAME: SPIM_SD_ReadStatus
;
;  DESCRIPTION:
;     Reads the SPIM Status bits in the Control/Status register.
;
;-----------------------------------------------------------------------------
;
;  ARGUMENTS:  none
;
;  RETURNS:
;     uint8  bStatus - transmit status data.  Use the defined bit masks.
;        Returned in Accumulator.
;
;  SIDE EFFECTS: 
;    The A and X registers may be modified by this or future implementations
;    of this function.  The same is true for all RAM page pointer registers in
;    the Large Memory Model.  When necessary, it is the calling function's
;    responsibility to perserve their values across calls to fastcall16 
;    functions.
;
;  THEORY of OPERATION or PROCEDURE:
;     Read the status and control register.
;
*/
uint8  SPIM_SD_bReadStatus(void)
{
	uint8 stat;
	stat = SPIM_SD_GetRxBufferSize();
	return(stat);
}

// End of File SPIM_SD_SPI.c
