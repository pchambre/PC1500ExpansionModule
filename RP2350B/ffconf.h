/*---------------------------------------------------------------------------/
/  Configurations of FatFs Module
/
/  This is a copy of the vendored no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
/  library's own src/include/ffconf.h (FatFs itself has no per-value
/  override mechanism -- the whole file has to be replaced), with exactly
/  two changes for this project, marked below:
/    - FF_USE_LABEL: 0 -> 1, to support EXP_COMMAND_READ_SD_VOLUME_LABEL
/      (f_getlabel()).
/    - FF_FS_NORTC: 0 -> 1, since this project's SD command set never
/      surfaces file timestamps, avoiding the need for a get_fattime()
/      implementation.
/  Everything else is unchanged from the vendored default. Placed on the
/  include path ahead of the submodule's own copy (CMakeLists.txt) so
/  ff.c picks this one up instead.
/---------------------------------------------------------------------------*/

#define FFCONF_DEF	80286	/* Revision ID */

/*---------------------------------------------------------------------------/
/ Function Configurations
/---------------------------------------------------------------------------*/

#define FF_FS_READONLY	0
/* This option switches read-only configuration. (0:Read/Write or 1:Read-only)
/  Read-only configuration removes writing API functions, f_write(), f_sync(),
/  f_unlink(), f_mkdir(), f_chmod(), f_rename(), f_truncate(), f_getfree()
/  and optional writing functions as well. */


#define FF_FS_MINIMIZE	0
/* This option defines minimization level to remove some basic API functions.
/
/   0: Basic functions are fully enabled.
/   1: f_stat(), f_getfree(), f_unlink(), f_mkdir(), f_truncate() and f_rename()
/      are removed.
/   2: f_opendir(), f_readdir() and f_closedir() are removed in addition to 1.
/   3: f_lseek() function is removed in addition to 2. */


#define FF_USE_FIND		1
/* This option switches filtered directory read functions, f_findfirst() and
/  f_findnext(). (0:Disable, 1:Enable 2:Enable with matching altname[] too) */


#define FF_USE_MKFS		1
/* This option switches f_mkfs() function. (0:Disable or 1:Enable) */


#define FF_USE_FASTSEEK	1
/* This option switches fast seek function. (0:Disable or 1:Enable) */


#define FF_USE_EXPAND	1
/* This option switches f_expand function. (0:Disable or 1:Enable) */


#define FF_USE_CHMOD	0
/* This option switches attribute manipulation functions, f_chmod() and f_utime().
/  (0:Disable or 1:Enable) Also FF_FS_READONLY needs to be 0 to enable this option. */


#define FF_USE_LABEL	1
/* This option switches volume label functions, f_getlabel() and f_setlabel().
/  (0:Disable or 1:Enable)
/  CHANGED from the vendored default (0) -- this project's
/  EXP_COMMAND_READ_SD_VOLUME_LABEL needs f_getlabel(). */


#define FF_USE_FORWARD	0
/* This option switches f_forward() function. (0:Disable or 1:Enable) */


#define FF_USE_STRFUNC	1
#define FF_PRINT_LLI	1
#define FF_PRINT_FLOAT	1
#define FF_STRF_ENCODE	3
/* FF_USE_STRFUNC switches string functions, f_gets(), f_putc(), f_puts() and
/  f_printf().
/
/   0: Disable. FF_PRINT_LLI, FF_PRINT_FLOAT and FF_STRF_ENCODE have no effect.
/   1: Enable without LF-CRLF conversion.
/   2: Enable with LF-CRLF conversion.
/
/  FF_PRINT_LLI = 1 makes f_printf() support long long argument and FF_PRINT_FLOAT = 1/2
/  makes f_printf() support floating point argument. These features want C99 or later.
/  When FF_LFN_UNICODE >= 1 with LFN enabled, string functions convert the character
/  encoding in it. FF_STRF_ENCODE selects assumption of character encoding ON THE FILE
/  to be read/written via those functions.
/
/   0: ANSI/OEM in current CP
/   1: Unicode in UTF-16LE
/   2: Unicode in UTF-16BE
/   3: Unicode in UTF-8
*/


/*---------------------------------------------------------------------------/
/ Locale and Namespace Configurations
/---------------------------------------------------------------------------*/

#define FF_CODE_PAGE	437
/* This option specifies the OEM code page to be used on the target system.
/  Incorrect code page setting can cause a file open failure.
/
/   437 - U.S.
/     0 - Include all code pages above and configured by f_setcp()
*/


#define FF_USE_LFN		3
#define FF_MAX_LFN		255
/* The FF_USE_LFN switches the support for LFN (long file name).
/
/   0: Disable LFN. FF_MAX_LFN has no effect.
/   1: Enable LFN with static  working buffer on the BSS. Always NOT thread-safe.
/   2: Enable LFN with dynamic working buffer on the STACK.
/   3: Enable LFN with dynamic working buffer on the HEAP.
/
/  To enable the LFN, ffunicode.c needs to be added to the project. */


#define FF_LFN_UNICODE	2
/* This option switches the character encoding on the API when LFN is enabled.
/
/   0: ANSI/OEM in current CP (TCHAR = char)
/   1: Unicode in UTF-16 (TCHAR = WCHAR)
/   2: Unicode in UTF-8 (TCHAR = char)
/   3: Unicode in UTF-32 (TCHAR = DWORD)
*/


#define FF_LFN_BUF		255
#define FF_SFN_BUF		12
/* This set of options defines size of file name members in the FILINFO structure
/  which is used to read out directory items. */


#define FF_FS_RPATH		2
/* This option configures support for relative path.
/
/   0: Disable relative path and remove related functions.
/   1: Enable relative path. f_chdir() and f_chdrive() are available.
/   2: f_getcwd() function is available in addition to 1.
/
/  Already 2 in the vendored default -- this project's SDCD/SDPWD need
/  f_chdir()/f_getcwd(), so no change needed here. */


/*---------------------------------------------------------------------------/
/ Drive/Volume Configurations
/---------------------------------------------------------------------------*/

# define FF_VOLUMES		4
/* Number of volumes (logical drives) to be used. (1-10) -- only drive 0
/  is ever actually mounted/used by this project (a single SD card), the
/  rest just go unused; left at the vendored default rather than trimmed
/  to 1, since nothing depends on the exact count. */


#define FF_STR_VOLUME_ID	0
#define FF_VOLUME_STRS		"RAM","NAND","CF","SD","SD2","USB","USB2","USB3"


#define FF_MULTI_PARTITION	0
/* This option switches support for multiple volumes on the physical drive. */


#define FF_MIN_SS		512
#define FF_MAX_SS		512
/* Sector size range -- fixed 512, matching disk_ioctl's GET_SECTOR_SIZE
/  response in diskio_qmi_cs1_sd.c. */


#define FF_LBA64		1
/* This option switches support for 64-bit LBA. (0:Disable or 1:Enable)
/  To enable the 64-bit LBA, also exFAT needs to be enabled. (FF_FS_EXFAT == 1) */


#define FF_MIN_GPT		0x10000000
/* Minimum number of sectors to switch GPT as partitioning format. */


#define FF_USE_TRIM		0
/* CTRL_TRIM not implemented in diskio_qmi_cs1_sd.c -- keep disabled. */



/*---------------------------------------------------------------------------/
/ System Configurations
/---------------------------------------------------------------------------*/

#define FF_FS_TINY		0
/* This option switches tiny buffer configuration. (0:Normal or 1:Tiny) */


#define FF_FS_EXFAT		1
/* This option switches support for exFAT filesystem. (0:Disable or 1:Enable) */


#define FF_FS_NORTC		1
#define FF_NORTC_MON	1
#define FF_NORTC_MDAY	1
#define FF_NORTC_YEAR	2024
/* CHANGED from the vendored default (FF_FS_NORTC 0) -- this project's SD
/  command set never surfaces file timestamps, so a fixed timestamp
/  (rather than a real get_fattime() implementation) is fine, and avoids
/  needing an RTC source at all. */


#define FF_FS_NOFSINFO	0
/* bit0=0: Use free cluster count in the FSINFO if available.
/  bit1=0: Use last allocated cluster number in the FSINFO if available. */


#define FF_FS_LOCK		16
/* File lock function -- how many files/sub-directories can be opened
/  simultaneously under file lock control. EXP_MAX_SD_CHANNELS (16) plus
/  SDLOAD/SDSAVE's own single file comfortably fits. */


#define FF_FS_REENTRANT	0
#define FF_FS_TIMEOUT	1000
/* Not re-entrant -- fine here, since the whole monitor loop (including
/  every FatFs call DoCommand() makes) runs on a single dedicated core,
/  never from more than one thread of execution at a time. */



/*--- End of configuration options ---*/
