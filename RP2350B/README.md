# RP2350B (bare core dev board + RN4871 BLE) -- in progress

The active RP2350-based redesign, superseding `../RP2350/` (a Pico 2 W
module target, kept for reference/history but no longer developed
against). This directory targets a bare RP2350B chip on a "core dev
board" (WeAct Studio's `weact_studio_rp2350b_core`, a real stock Pico SDK
board definition -- 48-GPIO QFN-80 package, no onboard WiFi/BT module)
plus a Microchip RN4871 BLE module wired over UART, matching the real
KiCad schematic at
`C:\Users\paulc\Documents\PC1500ExpansionBoard\PC1500-RP2350B-BLE`.
**None of this has been run against real hardware yet** -- treat it the
same way `../RP2350/README.md` treats its own port: a carefully-reasoned
starting point for bring-up, not proven-working firmware.

## Why RP2350B instead of the Pico 2 W module

The Pico 2 W module only exposes 26 usable GPIOs (4 of its 30 are wired
on-module to the onboard CYW43439 WiFi/BT chip), which forced `../RP2350/`
into real compromises: a split, non-contiguous data bus field, and the
QMI-CS1 SD-card trick (`../RP2350/lib/qmi_cs1_sdspi/`) to save 3 GPIOs at
the cost of SPI-XIP-flash bus contention during SD transfers. The bare
RP2350B exposes the full 48 GPIOs; this design uses 35 of them with 13
genuinely spare, which removes both compromises: the address bus
(GPIO0-12) and data bus (GPIO13-20) are each a single contiguous field
(`addr = gpio_in & 0x1FFF` with zero shifting), and the SD card gets a
real, uncontended, full 4-pin hardware SPI1 peripheral. BLE is now RN4871
over a real hardware UART instead of the Pico 2 W module's onboard
CYW43439 WiFi/BT combo chip -- a simpler, better-understood transport for
this project's actual need (a BLE link to a phone/host, not WiFi).

See `board_pins.h` for the full pin table and, importantly, its comments
on *why* each pin was checked against the real RP2350 GPIO
function-select table (not just "the schematic says so") -- SPI and UART
pins genuinely need to land on the right hardware peripheral's funcsel,
which isn't the same thing as "the wiring is electrically valid."

## Layout

- `CMakeLists.txt` / `pico_sdk_import.cmake` -- standard Pico SDK project
  files, `PICO_BOARD=weact_studio_rp2350b_core`.
- `board_pins.h` -- the GPIO assignment table, kept in sync by hand with
  the real KiCad schematic (see its own top comment for exactly which
  file is ground truth if they ever disagree).
- `pc_exp.h` -- copy of `../Design01_NonDMA_8K_PV_Swap.cydsn/PC_EXP.h`'s
  wire-protocol constants, unchanged from `../RP2350/pc_exp.h`.
- `ffconf.h` -- this project's FatFs configuration, unchanged from
  `../RP2350/ffconf.h` (`FF_USE_LABEL`/`FF_FS_NORTC` changed from the
  vendored default -- see its own top comment).
- `hw_config.c` -- tells the vendored FatFs SD library which real
  GPIOs/SPI instance the SD card is on (SPI1, GPIO40-43).
- `monitor.c`/`monitor.h` -- the bus-servicing loop and `DoCommand()` SD
  command dispatcher, carried over from `../RP2350/monitor.c` with the
  bus-loop half simplified (single contiguous field for both buses now,
  see the file's own top comment) and `DoCommand()` itself unchanged
  (it's entirely transport-agnostic).
- `ble_rn4871.c`/`.h` -- minimal RN4871 UART-transparent-mode driver
  (init/send/recv/wake-pin control). Doesn't include the RN4871's own
  ASCII command-mode provisioning (putting it into transparent mode in
  the first place) -- that's a one-time setup step via the module's own
  `$$$` escape sequence, out of scope for this driver.
- `greenpak_i2c.c`/`.h` -- bit-banged (software) I2C primitives for the
  two GreenPAK links. Deliberately not using RP2350's hardware I2C
  peripherals -- `board_pins.h`'s GreenPAK SDA/SCL pins don't land on a
  matched pair on the same hardware I2C instance, which is fine since
  this link is low-speed/non-critical (see `board_pins.h`'s own comment).
  The actual GreenPAK command protocol isn't designed yet -- see
  `monitor.c`'s `EXP_COMMAND_ROM_FROM_SRAM`/`ROM_FROM_MCU` cases, carried
  over as placeholders from `../RP2350/monitor.c`.
- `main.c` -- core0: USB stdio, SPI1 SD mount, BLE UART init, both
  GreenPAK I2C buses init, then `multicore_launch_core1(monitor_run)` and
  idle. Core1 runs the whole bus loop/`DoCommand()` exclusively, same
  split as `../RP2350/main.c`.
- `lib/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico/` -- git submodule, same library
  `../RP2350/` uses, pinned to the same commit (`d5e4534`). Unlike
  `../RP2350/`, this project links its own `add_subdirectory`'d
  `no-OS-FatFS-SD-SDIO-SPI-RPi-Pico` INTERFACE target directly (SPI SD
  driver included) instead of hand-picking just the FatFs core files --
  the QMI-CS1-vs-library's-own-SPI-driver symbol collision that forced
  `../RP2350/` to do it the hard way doesn't apply here, since this
  project uses the library's own SPI driver, not a custom one.
- No `lib/qmi_cs1_sdspi/` -- not needed, see "Why RP2350B" above.

## Building

Same toolchain as `../RP2350/` (see that file's own "Building" section
for the full pinned-version table) -- Pico SDK 2.3.0 + Arm GNU Toolchain
15.2.rel1 at `~/.pico-sdk/`, `PICO_SDK_PATH`/`PICO_TOOLCHAIN_PATH` set as
persistent User environment variables. `weact_studio_rp2350b_core` is a
real board definition shipped in Pico SDK 2.3.0's
`src/boards/include/boards/` -- no custom board header needed.

**One gotcha this project hits that `../RP2350/` didn't** (that port
never needed picotool, since it doesn't produce a UF2 via the RP2350
bootloader path the same way -- this one does, via `pico_add_extra_outputs`,
which needs picotool at configure time): a plain `cmake -G Ninja -B build
-S .` fails partway through with unrelated-looking C++ compile errors
(`'to_string' is not a member of 'std'`, `unknown type name '__m128i'`,
etc). That's this machine's stray old `C:\Flex Windows\gcc\bin` earlier in
`PATH` (unrelated to this project) getting picked up by CMake's
`FetchContent`-driven build-picotool-from-source fallback instead of a
working host compiler -- same root cause `../RP2350/README.md` documents
for `pioasm`/`picotool`, but that project's build never actually
triggered this path, so it went undocumented as a required flag until
now. Fix: point `CMAKE_PREFIX_PATH` at the prebuilt binaries so CMake
finds them instead of trying to build from source:

```
cmake -G Ninja -B build -S . -DCMAKE_PREFIX_PATH="C:/Users/paulc/.pico-sdk/toolchain/picotool-2.3.0;C:/Users/paulc/.pico-sdk/toolchain/pico-sdk-tools-2.3.0"
cmake --build build
```

Confirmed working end-to-end 2026-08-25: this produces
`build/pc1500_expansion_rp2350b.uf2` (219KB) from a clean `build/`
directory with zero errors, including the full monitor loop,
`DoCommand()`, `greenpak_i2c`, and `ble_rn4871` modules described above --
ready to drag onto the board in BOOTSEL mode. As with `../RP2350/`, a
clean build is the extent of verification possible without the real
board/GreenPAKs/RN4871/SD card wired up -- see "Pin budget"/board_pins.h
above for what still needs real hardware to confirm.
