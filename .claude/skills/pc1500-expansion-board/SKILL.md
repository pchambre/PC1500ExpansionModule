---
name: pc1500-expansion-board
description: Use for any work on the PC-1500 SD-card expansion board hardware -- the PSoC5-based firmware in this repo (PC1500-PSOC5), the RP2350 (Pico 2 W) redesign in RP2350/, or the KiCad schematic in the separate PC1500ExpansionBoard repo. Covers the DoCommand()/monitor-loop firmware architecture, the EXP_COMMAND_* wire protocol shared with rom.asm, the RP2350 pin budget and QMI-CS1 SD driver, and the direct-file-edit KiCad workflow (no GUI access).
---

# PC-1500 expansion board (PSoC5, RP2350, and the KiCad schematic)

This covers the real-hardware companion project to `pc1500emu`: an SD-card
expansion board for the Sharp PC-1500, currently shipping on a PSoC5-based
design (`Design01_NonDMA_8K_PV_Swap.cydsn`, this repo), with an in-progress
RP2350 (Pico 2 W) redesign (`RP2350/`, this repo) and a KiCad schematic for
the PSoC5 board in a **separate repo**,
`C:\Users\paulc\Documents\PC1500ExpansionBoard` (no git remote confirmed as
of this writing -- local-only history via KiCad's own `.history/` Local
History feature).

The PC-1500-side counterpart to all of this is `pc1500emu`'s `rom/rom.asm`
(SD*/DIR commands) and `docs/pc1500_hardware_reference.md` -- see that
repo's own `pc1500-dev` skill. The wire protocol below (`PC_EXP.h`) is the
literal contract between the two sides; changing it means changing both.

## Firmware architecture (PSoC5, `Design01_NonDMA_8K_PV_Swap.cydsn/main.c`)

- **No BRQ line** -- the board can never halt the LH5801 to take over the
  bus. `main()`'s `for(;;)` loop reacts to bus cycles it doesn't control
  the timing of: on CS asserted, latches page/address/data, drives the
  data pins from `buffer[page][laddress]`; on a write to
  `EXP_INSTRUCTION_PAGE`/`EXP_INSTRUCTION_ADDRESS`, calls `DoCommand()`
  inline instead of just storing the byte.
- **`DoCommand()` is a synchronous, blocking, ~30-case switch** over
  `EXP_COMMAND_*`, built on SEGGER emFile's `FS_*` API. It sets
  `EXP_STATUS_BUSY` into the shared buffer *before* starting SD work, then
  blocks for the whole operation (can be milliseconds) before setting
  `EXP_STATUS_SUCCESS`/`ERROR`/etc. The main loop does **not** resume
  servicing other bus cycles while `DoCommand()` runs -- any bus read
  during that window sees whatever was last physically latched onto the
  data pins before the call. This is by design: the LH5801 side
  (rom.asm) only ever polls the status byte in a tight loop after issuing
  a command, and only observes a fresh answer once `DoCommand()` returns
  and the main loop resumes. Don't try to "fix" this without also
  auditing rom.asm's polling loop -- it's an existing, working protocol
  property, not a bug.
- **`buffer[32][256]`** is the whole 8K LH5801 window (0x8000-0x9FFF):
  pages 0-7 are the 2K live data window (`EXP_BUFFER_START_PAGE`,
  `EXP_INSTRUCTION_PAGE`=7), pages 8-31 are the 6K ROM region -- pages
  8-31 only actually route through this buffer (and `DoCommand()`) while
  `Control_Mode_Control`/GreenPAK state selects `ROM_FROM_MCU`, during the
  boot-time copy (`EXP_COMMAND_ROM_FROM_MCU`/`ROM_FROM_SRAM`). Once
  `ROM_FROM_SRAM` is selected, ROM reads are answered directly by the SRAM
  in hardware, no MCU involvement.

## Wire protocol (`PC_EXP.h`, mirrored in `RP2350/pc_exp.h`)

Kept in sync by hand across three places: `PC_EXP.h` (PSoC5),
`RP2350/pc_exp.h` (RP2350 port), and `pc1500emu`'s `ExpansionMock` (C++
test double) -- check all three before assuming a constant is unused.

- `EXP_COMMAND_*` -- the command byte written to the instruction address.
  SD file ops (open/read/write/close/remove), dir listing/cd/mkdir/rmdir/
  pwd, cp/mv (+ dest-exists check), SDOPEN-style channel I/O
  (open/close/write/read/skip a value, up to `EXP_MAX_SD_CHANNELS`=16),
  name validation, and `ROM_FROM_SRAM`/`ROM_FROM_MCU`.
- `EXP_STATUS_*` -- `BUSY`/`READY`/`SUCCESS`/`ERROR`/`NOT_IMPLEMENTED`/
  `EOF`. `EOF` is distinct from `ERROR` specifically for
  `SD_READ_VALUE`'s "legitimately ran out of stored values" case (not an
  error -- SDINPUT# fills remaining variables with 0/blank), vs.
  `SD_SKIP_VALUES` which uses `ERROR` for the same "ran out" condition
  since SDSKIP# raises a real ERROR 40 for that instead.
- **Every SD command's name argument is a full path**, not just a bare
  filename: a plain filename, a relative path (`.`/`..`/multiple
  components), or an absolute one from the SD root (`/SUB/FILE.BAS`).
  `'/'` is this project's own separator convention (not emFile's/FatFs's
  native one) specifically because it's an unshifted PC-1500 key.
  `'+'` stands in for a real FAT short name's `'~'` for the same reason
  (no `~` key) -- translated at the filesystem boundary
  (`ConvertPlusToTilde`/`ConvertTildeToPlus`), never earlier.
- **`EXP_COMMAND_LIST_SD_DIR`'s bulk format**: 2-byte BE count, then
  fixed-width `EXP_DIR_RECORD_SIZE`-byte records (name +
  pre-rendered size text + 4-byte binary size), then a free-text summary
  line. Fixed-width and pre-rendered specifically because the ROM has no
  binary-to-decimal conversion of its own and no need to parse
  variable-length records with no multiply instruction.
- **SDCP/SDMV's two-name wire layout**: two fixed
  `EXP_TWO_NAME_SLOT_LEN`-byte slots back-to-back, not
  length-derived-offset -- deliberately, so the LH5801 (no multiply
  instruction) never computes the second slot's address from the first
  name's length.
- **SDOPEN-family channels are variable-oriented, not filename-oriented**:
  rom.asm resolves a BASIC variable's address itself (base ROM's D461H)
  and sends/receives a self-describing chunk (`['N']`+8 raw bytes for
  numeric, `['S']`+length+bytes for string) -- the MCU side never sees a
  variable name or value, just opaque chunk bytes to move to/from a
  channel's file.

## RP2350 (Pico 2 W) redesign (`RP2350/`)

Not yet the active design -- see `RP2350/README.md` for full status. Two
real architectural facts worth knowing before touching this:

- **Pin budget is exact and fully allocated**: a Pico 2 W module exposes
  26 usable GPIOs (GP23/24/25/29 are hard-wired on the module to the
  onboard CYW43439). This design uses all 26: 13-bit flat address bus
  (8K window), 2 GreenPAK-combined trigger lines (`CS+R+OE` "drive data
  now" / `CS+W` "latch data now" -- doubling as POWMAN `PWRUP0`/`PWRUP1`
  wake sources, no dedicated wake pin), 8 data bits, a 2-wire I2C-style
  GreenPAK link (comms + `Control_Mode_Control`-equivalent mode select),
  1 SD card pin. See `RP2350/board_pins.h` and its own README section
  "Pin budget" for the full table -- there is zero slack, so adding any
  new signal means removing one of these first.
- **SD card rides QMI CS1, not a normal SPI peripheral** --
  `RP2350/lib/qmi_cs1_sdspi/`, a from-scratch driver (no existing one
  found) using RP2350's QMI peripheral's second chip-select (shared
  SCLK/SD0-3 with the onboard flash's own CS0 -- the same mechanism
  normally used for XIP PSRAM) instead of 4 dedicated SPI GPIOs. Real
  consequence: flash XIP stalls chip-wide (both cores) while a CS1
  transaction is in flight, so every function that runs between
  `qmi_cs1_spi_transaction_begin/end` is marked `__not_in_flash_func` --
  see that module's own README and `qmi_cs1_sd.c`'s top comment for why,
  and don't add a new function to that call chain without the same
  treatment. `monitor.c`'s own loop and `DoCommand()` deliberately do
  *not* need this treatment (see that file's own top comment for why --
  the existing async BUSY-poll protocol already tolerates the main loop
  being "away" during a command, same as the PSoC5 original).
- FatFs is wired in via just the vendored `no-OS-FatFS-SD-SDIO-SPI-RPi-Pico`
  submodule's protocol-agnostic core (`ff15/source/ff.c`/`ffsystem.c`/
  `ffunicode.c`) -- its own SD driver (`sd_driver/`, built on
  `hardware_spi`) is unused/not linked, since it has no pluggable
  low-level-driver seam and would collide with `qmi_cs1_sdspi`'s own
  `diskio_*` functions at link time. `RP2350/ffconf.h` is a full copy of
  the vendored library's default (FatFs has no per-value override
  mechanism) with `FF_USE_LABEL`/`FF_FS_NORTC` changed -- see its own top
  comment before changing anything else there.
- Pico SDK 2.3.0 + ARM toolchain 15.2.rel1 installed manually at
  `~/.pico-sdk/` (no GUI access to the VS Code extension's own installer)
  -- see `RP2350/README.md`'s "Building" section for exact versions/
  locations and the working `cmake -DCMAKE_PREFIX_PATH=...` invocation
  (needed to find prebuilt `picotool`/`pioasm` and avoid a stray old GCC
  on this machine's `PATH` breaking a from-source picotool build).
- **Nothing here has touched real hardware** -- no RP2350 board, SD card,
  GreenPAK, or schematic exists yet. A clean build is the only
  verification that's been possible; say so plainly rather than
  overclaiming when discussing this code.

## KiCad workflow (`PC1500ExpansionBoard` repo, PSoC5 board schematic)

No GUI access to KiCad -- schematic work happens by editing
`PC1500ExpansionBoard.kicad_sch` directly (it's a plain S-expression text
format) and verifying via `kicad-cli`, not by driving the KiCad UI.

- **Connectivity is net-name matching, not the bus graphic.** A `(bus ...)`
  block is purely visual annotation; two pins are actually connected only
  if they share the same net label text (or a direct wire). When asked to
  "remove the bus" while preserving connectivity, remove the bus graphic
  and its label but leave (or add) matching net labels on the actual pins.
- **Stored label text escapes `/` as `{slash}`** -- e.g. `R/W` is stored as
  `R{slash}W`. When cross-checking a label against a netlist export (which
  uses the real, unescaped name), account for this or every such label
  will falsely look orphaned.
- **Pin/pad coordinate transform**: at symbol rotation 0, a pin's absolute
  Y position is `symbol_y - local_pin_y` (empirically validated this
  session, not from a documented formula) -- needed when computing wire
  endpoints to attach to a specific pin by coordinates.
- **Verification loop**: `kicad-cli sch erc` after any wiring change;
  `kicad-cli sch export netlist --format kicadxml` to cross-check for
  orphaned labels (a label with only one connection) or unintended nets.
  Run both after any nontrivial edit -- don't assume a hand-edited
  S-expression is correct without them.
- **Placing a net label via the KiCad UI** (for the user's own reference,
  not something I do): click to place, type the name in the popup, then
  click the *same* pin again to actually commit it -- this really is the
  expected two-click flow, not a UI glitch.
- **Power pin research before wiring, not from memory**: e.g. the PSoC5's
  `VCCA`/`VCCD` pins are internal-regulator *outputs* in the correctly-
  configured "internally regulated" mode and must never be tied to
  external VCC (would backfeed the regulator) -- unlike `VDDD`/`VDDA`/
  `VDDIO0-3`, which should be. Confirmed via the actual CY8C58LP datasheet
  (§6.2) before wiring, not assumed from the pin name looking similar.
  Apply the same discipline to any other power-pin question on this
  board -- check the real datasheet, don't pattern-match from pin names.

## Known open items (as of the RP2350 port + KiCad sessions)

- PSoC5 board: decoupling capacitors on power-supply connections, and the
  VBAT/VBOOST/IND/VSSB pin treatment (boost circuit not needed on this
  board) -- raised but not resolved; the user redirected to other work
  before this was finished.
- RP2350 board: no schematic exists yet, so `board_pins.h`'s GPIO
  *numbers* (not the pin *count/roles*, which are settled -- see above)
  are still provisional.
- RP2350 board: the GreenPAK-comms protocol over the 2-wire I2C-style link
  isn't designed yet -- `monitor.c`'s `EXP_COMMAND_ROM_FROM_SRAM`/
  `ROM_FROM_MCU` cases are placeholders pending that.
- RP2350 board: if the address bus ever needs to narrow below 13 bits
  (e.g. to free more pins), the 6K ROM region's boot-load path needs a
  fundamental rethink -- it currently reaches SRAM by routing through
  this same MCU-facing bus (`ROM_FROM_MCU`), which requires the full
  8K/13-bit address range to be visible to the MCU during that copy.
