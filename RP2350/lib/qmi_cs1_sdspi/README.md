# qmi_cs1_sdspi

An SD-card-over-SPI driver for RP2350 that puts the card on the QMI
(QSPI Memory Interface) peripheral's second chip select, **CS1**, instead
of a normal `spi0`/`spi1` peripheral -- sharing SCLK/SD0/SD1 with the
chip's onboard flash (which uses CS0), rather than needing 4 dedicated
GPIOs of its own.

## Why

On the RP2350A package (Pico 2 / Pico 2 W), only 26 of the chip's 30
GPIOs are exposed, and a design with a wide parallel bus (this project
needs 13 address bits + 2 status lines + 8 data bits for an LH5801
expansion-bus interface) can run out of pins fast. QMI's CS1 is a real,
documented RP2350 feature -- normally used for XIP PSRAM -- that lets a
second SPI-mode device share the flash's own SCLK/SD0-3 lines, distinguished
purely by which chip-select is asserted. Using it for an SD card instead
costs **1 GPIO** (the CS1 pin itself) instead of the normal **4**
(SCK/MOSI/MISO/CS).

CS1 is only available on specific pins: on RP2350A, GPIO 0, 8, or 19 (the
`GPIO_FUNC_XIP_CS1` alternate function). RP2350B's GPIO47 doesn't exist
on this package.

We found no existing driver doing this -- CS1 is documented and used for
PSRAM, but not, as far as we could find, for an SD card. This module is
that driver, split out on its own in case it's useful for other RP2350
projects with the same pin-budget problem.

## How it works

QMI has a "direct mode" (`QMI_DIRECT_CSR.EN`) that lets firmware push
raw bytes through a small TX/RX FIFO pair and clock them out over
whichever chip-select is asserted (`ASSERT_CS0N` for the flash,
`ASSERT_CS1N` for the second device), independent of XIP's normal
memory-mapped read/write windows. This module:

- `qmi_cs1_spi.h/.c` -- generic byte-transfer primitives over QMI direct
  mode + CS1: init, set baud rate, begin/end a transaction (assert/
  deassert CS1), transfer one byte full-duplex. No SD-specific knowledge
  at all -- usable for any SPI-mode device that only needs SCLK/MOSI/MISO
  plus its own CS.
- `qmi_cs1_sd.h/.c` -- the SD card SPI-mode protocol on top of that:
  CMD0 (GO_IDLE_STATE), CMD8 (interface condition check, distinguishes
  SDv2/SDHC/SDXC from SDv1), CMD55+ACMD41 (init polling), CMD58 (OCR,
  checks CCS/high-capacity), CMD17/CMD24 (single-block read/write), CRC7
  for command frames, R1/R7 response parsing, data-token/CRC16 handling
  for block transfers.
- `diskio_qmi_cs1_sd.c` -- the thin glue implementing FatFs's `diskio.h`
  interface (`disk_status`/`disk_initialize`/`disk_read`/`disk_write`/
  `disk_ioctl`) in terms of the above, so any FatFs port (this project's
  or another's) can mount it with `f_mount()` like any other block
  device.

## The one real constraint: it's a single shared bus

QMI can only be doing one thing at a time -- servicing XIP flash fetches
(instruction/rodata reads via CS0) or running a direct-mode transaction
(CS1, this driver). While a transaction is in progress, **flash XIP
fetches stall on both cores**, chip-wide, until the transaction ends. This
is architecturally the same situation the Pico SDK already handles around
flash erase/program operations: code that must keep running during a
flash-busy window has to live in RAM, not be fetched from flash. Use
`__not_in_flash_func` (from `pico/platform.h`) on any function that must
stay responsive while an SD transfer is in flight, and make sure
everything *it* calls is RAM-resident too -- the low-level transfer
functions here (`qmi_cs1_spi_transaction_begin/end`,
`qmi_cs1_spi_transfer_byte`) are already marked this way; callers with
their own latency requirements need to extend that discipline to their own
code.

## Status

This was written by working through the RP2350 datasheet and the Pico
SDK's own register headers (`hardware/structs/qmi.h`,
`hardware/regs/qmi.h`) carefully, but **it has not been run against real
hardware** -- no RP2350 board, SD card, or scope was available in the
session that wrote it. Treat it as a well-reasoned starting point for
bring-up, not as verified-working code. The first things worth checking
with a scope/logic analyzer on real hardware: that CS1 actually toggles
independently of CS0 as expected, that the SCK/MOSI/MISO waveforms look
like valid SPI mode 0, and that the SD card's own R1 response byte
(0x01 after CMD0) comes back at all before trusting anything built on top
of it.
