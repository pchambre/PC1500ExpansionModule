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
//These two are adjusted to remove the upper four CS bits
#define EXP_INSTRUCTION_ADDRESS 0xFF //Command port for PC to submit requests
#define EXP_INSTRUCTION_PAGE 0x1F //Command page for PC to submit requests
#define EXP_BUFFER_START_PAGE 0x00 //First page of the read/write data exchange area
#define EXP_BUFFER_START_ADDRESS 0x00 //First laddress of the read/write data exchange area

#define EXP_FULL_INSTRUCTION_ADDRESS = 0x1FFF

#define EXP_STATUS_BUSY 1
#define EXP_STATUS_READY 0
#define EXP_STATUS_ERROR 128
#define EXP_STATUS_NOT_IMPLEMENTED 64
#define EXP_STATUS_SUCCESS 2

#define EXP_COMMAND_GET_SD_FREE_SPACE 1
#define EXP_COMMAND_CREATE_SD_FILE 2
#define EXP_COMMAND_WRITE_TO_SD_FILE 3
#define EXP_COMMAND_CLOSE_SD_FILE 4
#define EXP_COMMAND_GET_SD_FILE_SIZE 5
#define EXP_COMMAND_READ_SD_VOLUME_LABEL 6
#define EXP_COMMAND_GET_SD_FILE_NAME 7
#define EXP_COMMAND_GET_SD_FILE_STATUS 8
#define EXP_COMMAND_FORMAT_SD_CARD 9

#define EXP_COMMAND_ROM_FROM_MCU 0x20
#define EXP_COMMAND_ROM_FROM_SRAM 0x21

#define EXP_COMMAND_TEST_COPY_STRING 129

#define EXP_COMMAND_CLEAR_STATUS 0xFF

#define EXP_SD_FILE_STATUS_CLOSED 0
#define EXP_SD_FILE_STATUS_OPEN_WRITE 1
#define EXP_SD_FILE_STATUS_OPEN_READ 2

    /* Defines for DMA_1 */
#define DMA_1_BYTES_PER_BURST 1
#define DMA_1_REQUEST_PER_BURST 1
#define DMA_1_SRC_BASE (CYDEV_PERIPH_BASE)
#define DMA_1_DST_BASE (CYDEV_SRAM_BASE)

/* [] END OF FILE */
