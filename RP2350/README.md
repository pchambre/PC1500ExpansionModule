# RP2350 (Pico 2 W) architecture -- in progress

An exploration of rearchitecting this board around the RP2350 (Pico 2 W,
with its CYW43439 WiFi/BT chip) instead of the current PSoC 5 + ESP32-S3
combination. **This is not yet the active design** -- `Design01_NonDMA_8K_PV_Swap.cydsn`
(see the top-level README) is what actually ships and is developed
against. This directory tracks a real rewrite of the firmware, not a
sketch: the bus-servicing loop and `DoCommand()` SD command dispatcher
are now ported from the PSoC5 side (see "Layout" below), and the GPIO
pin budget for a Pico 2 W module is fully worked out (see "Pin budget"
below) -- but **none of it has been run against real hardware**: there is
no RP2350 board wired to a PC-1500, no GreenPAK, and no schematic for
this board yet. Treat everything here as a carefully-reasoned starting
point for bring-up, not as proven-working firmware.

## Why RP2350

Same fundamental constraint as the PSoC5 design: the LH5801 has no
bus-request (BRQ) line, so the board can never halt the CPU to take over
the bus -- it must react to bus cycles it doesn't control the timing of.
RP2350's PIO blocks are a better fit for the reactive parts of that
problem than PSoC5's software polling loop: independent state machines
that react to GPIO transitions in a handful of deterministic clock
cycles, immune to whatever else the CPU cores are doing. Concretely:

- PIO has no general memory access -- it can capture an address/status
  atomically and drive output pins the instant it's handed a value, but
  it can't do an arbitrary "look up byte at this address" lookup itself
  (confirmed against how the Pico SDK's own `cyw43_bus_pio_spi.c` driver
  uses PIO+DMA: that's a CPU-orchestrated, sequential, fixed-length
  transfer, not a random-access slave response -- a genuinely different
  problem from what this board needs for its live data window).
- The reactive part of the protocol (the 2K "live" MCU<->LH5801 data
  window -- status/params/command byte, matching `EXP_INSTRUCTION_PAGE`/
  `EXP_BUFFER_START_PAGE` in the current PSoC firmware's `PC_EXP.h`)
  needs a real memory lookup per access, so it needs a dedicated CPU
  core running a tight loop against its own on-chip SRAM -- comfortably
  within a ~2us budget at 150MHz (on the order of 10-30 cycles of actual
  work against a ~300-cycle budget), but that core must be fully
  exclusive (no interrupts, no other work) while doing it, since there's
  no BRQ fallback if it's ever late.
- That core is only needed while actively servicing an LH5801 request,
  not perpetually -- most of the time (whenever no command is in
  flight), the MCU can be fully asleep. RP2350's pad isolation latches
  (datasheet SS9.7) freeze every GPIO's output state automatically on
  sleep entry and hold it through the entire sleep period and the early
  part of wake, with zero MCU participation -- so a status byte
  (sleeping/busy/ready) set just before sleeping stays correctly visible
  without needing a separate hardware latch for the *value*. Waking
  itself uses up to 4 dedicated GPIO wake sources (`PWRUP0`-`PWRUP3`),
  monitored by the always-on POWMAN domain even with the core fully
  powered down.
- The 6K ROM region and the 62256's remaining ~26K of general expansion
  RAM are **single-master** -- only the LH5801 ever drives that SRAM's
  address/data pins. The ROM image gets there via the LH5801 itself
  running a memcpy loop (the existing `_PV_Swap` firmware's own
  mechanism: PV toggled between the read, from the MCU's data path, and
  the write, to the SRAM, of each byte), not the MCU writing the SRAM
  directly -- so there's no two-master contention to arbitrate on that
  chip. GreenPAK-equivalent glue logic (still needed, likely still an
  actual small CPLD rather than something RP2350 absorbs) does address
  decode/chip-select for the 26K region and muxes whether the 6K ROM
  window's reads are answered by the SRAM directly (`ROM_FROM_SRAM`,
  hardware-only, MCU asleep) or by the MCU (`ROM_FROM_MCU`, during the
  boot-time copy) -- plus, separately, latching the one thing that
  genuinely still needs a CPLD-side flip-flop: the wake *request* itself
  (a bus write to the instruction address while asleep), since that has
  to survive as a stable level until POWMAN gets around to noticing it,
  not just a bus-cycle-width pulse.

## Pin budget

A Pico 2 W module exposes 26 usable GPIOs (GP23/24/25/29 are wired on the
module itself to the onboard CYW43439 wifi chip). This design uses all
26, with zero spare:

| Signal | Pins | Notes |
|---|---|---|
| Address bus A0-A12 | 13 | Flat 8K window (0x8000-0x9FFF) |
| Trigger lines (read-trigger, write-trigger) | 2 | A GreenPAK combines CS+R/W+OE into two edges ("read requested, drive data now" / "write requested, latch data now") instead of RP2350 computing CS&&RW&&OE itself. These double as POWMAN wake sources (`PWRUP0`/`PWRUP1`) -- no separate wake pin. |
| Data bus D0-D7 | 8 | |
| I2C-style link to the GreenPAK(s) | 2 | Comms + the `Control_Mode_Control`-equivalent ROM-source mux select |
| SD card | 1 | Via QMI CS1, not a normal SPI peripheral -- see below |

SRAM chip-select and MCU chip-select are both handled by the GreenPAK(s)
directly, not wired to RP2350 GPIOs. See `board_pins.h` for the exact
GPIO assignment (SD_CS1 is fixed by silicon to GPIO 0/8/19 on RP2350A;
everything else is a free choice, not yet validated against a real
schematic).

### SD card: QMI CS1, not a normal SPI peripheral

A normal SPI peripheral (4 dedicated GPIOs) would have blown this
budget by 3. RP2350's QMI (QSPI Memory Interface) peripheral -- the same
one that does XIP flash fetches -- supports a second chip-select, CS1,
sharing SCLK/SD0-3 with the flash's own CS0 (a real, documented feature,
normally used for XIP PSRAM). Using it for the SD card instead costs 1
GPIO instead of 4. The driver for this lives in its own module,
`lib/qmi_cs1_sdspi/` -- **see that directory's own README.md** for the
full explanation, the real constraint it comes with (QMI is a single
physical bus, so flash XIP stalls chip-wide while an SD transaction is
in flight), and the explicit "unverified against real hardware" caveat.
It's split out on its own because we found no existing driver doing
this, and it may be useful for other RP2350 projects with the same
pin-budget problem.

## Layout

- `CMakeLists.txt` / `pico_sdk_import.cmake` -- standard Pico SDK
  project files, `PICO_BOARD=pico2_w`.
- `board_pins.h` -- the GPIO assignment table above.
- `pc_exp.h` -- copy of `../Design01_NonDMA_8K_PV_Swap.cydsn/PC_EXP.h`'s
  wire-protocol constants, kept in sync by hand (same pattern this
  project already uses elsewhere, e.g. pc1500emu's `ExpansionMock`).
- `ffconf.h` -- this project's FatFs configuration (copy of the vendored
  library's default with `FF_USE_LABEL`/`FF_FS_NORTC` changed -- see its
  own top comment).
- `monitor.c`/`monitor.h` -- the ported bus-servicing loop and
  `DoCommand()` SD command dispatcher, from
  `../Design01_NonDMA_8K_PV_Swap.cydsn/main.c`. Translated case-by-case
  from SEGGER emFile's `FS_*` API to FatFs's `f_*` API; see `monitor.c`'s
  own top comment for the port's structural reasoning (including why it
  deliberately does *not* need the RAM-residency treatment
  `qmi_cs1_sdspi` does).
- `lib/qmi_cs1_sdspi/` -- the QMI-CS1 SD-over-SPI driver described
  above.
- `lib/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico/` -- git submodule; only its
  protocol-agnostic FatFs core (`ff15/source/ff.c`/`ffsystem.c`/
  `ffunicode.c`) is actually compiled in -- its own SD driver
  (`sd_driver/`) is unused, since the SD card goes through
  `qmi_cs1_sdspi` instead.
- `main.c` -- core0: `stdio_init_all`, QMI-CS1 init, `f_mount`, then
  `multicore_launch_core1(monitor_run)` and idle. Core1 runs the whole
  bus loop / `DoCommand()` exclusively.

## Building

Requires the Pico SDK (2.0.0+, for RP2350 support) and its toolchain --
the [official Raspberry Pi Pico VS Code
extension](https://marketplace.visualstudio.com/items?itemName=raspberry-pi.raspberry-pi-pico)
normally manages this installation automatically, but that installer is
GUI-driven (no CLI equivalent for its specific managed-SDK download), so
this machine's copy was set up manually instead -- an independently
documented, fully standard way to develop for the Pico SDK, not a
workaround:

| Component | Version | Location |
|---|---|---|
| Pico SDK (+ submodules: tinyusb, cyw43-driver, lwip, mbedtls, btstack) | 2.3.0 | `~/.pico-sdk/sdk/2.3.0` |
| Arm GNU Toolchain (arm-none-eabi) | 15.2.rel1 | `~/.pico-sdk/toolchain/15.2.rel1` |
| Ninja | 1.13.2 | `~/.pico-sdk/ninja/1.13.2` |
| `picotool` (prebuilt, from `raspberrypi/pico-sdk-tools`) | 2.3.0 | `~/.pico-sdk/toolchain/picotool-2.3.0` |
| `pioasm` (prebuilt, from `raspberrypi/pico-sdk-tools`) | 2.3.0 | `~/.pico-sdk/toolchain/pico-sdk-tools-2.3.0` |

The SDK/toolchain versions are pinned to specific releases (not
"latest") for reproducibility -- bump deliberately, not by accident.
`picotool`/`pioasm` are prebuilt binaries from `pico-sdk-tools`'
releases rather than built from source: this machine has a stray, very
old GCC earlier in `PATH` (`C:\Flex Windows\gcc\bin`, unrelated to this
project) that CMake's `FetchContent`-driven host-tool sub-builds picked
up instead of a working host compiler, and it's new enough that this
predates C99 support -- using the prebuilt binaries sidesteps needing a
working *host* compiler entirely (separate from the `arm-none-eabi`
*target* cross-compiler above, which is unaffected by this).

`PICO_SDK_PATH` and `PICO_TOOLCHAIN_PATH` are set as persistent User
environment variables pointing at the paths above (standard Pico SDK
convention, read directly by its CMake files); Ninja and the toolchain's
`bin/` are also on the User `PATH`. `.vscode/settings.json` at the repo
root points the Pico extension's own `cmakePath`/`ninjaPath`/
`python3Path` settings at the same install. A new terminal (or VS Code
restart) is needed to pick up environment changes made after it was
opened.

Confirmed working end-to-end: `cmake -G Ninja -B build -S .` then
`cmake --build build` produces `build/pc1500_expansion_rp2350.uf2`,
ready to drag onto the board in BOOTSEL mode -- including the full
monitor loop, `DoCommand()`, and `qmi_cs1_sdspi` module described above
(a clean build was the extent of verification possible in the session
that wrote them; see "Pin budget" above for what still needs real
hardware to confirm).

Unlike the PSoC5 side, this is a plain CMake + C project -- no
proprietary schematic-only files, everything here is directly
readable/editable text.
