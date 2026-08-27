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
- **`extends` symbols silently break `kicad-cli` connectivity resolution.**
  If a cached `lib_symbols` entry uses `(extends "BaseSymbol")` (e.g.
  `HM62256BLP extends KM62256CLP`, `AP2112K-3.3 extends AP2204K-1.5`), wires
  correctly placed at the inherited pins' real coordinates still show as
  fully disconnected in `kicad-cli sch erc`/`export netlist` -- the pin
  *positions* are right, but the connectivity engine doesn't traverse the
  inheritance. Symptom: a component you just wired shows 0 net memberships
  in the exported netlist despite every coordinate checking out. Fix: flatten
  the symbol into a standalone copy with its own pins inlined (no
  `extends`) -- either copy a working flattened version from elsewhere (the
  non-BLE sibling schematic already had `HM62256BLP` flattened, which is
  why *it* worked and this file didn't), or manually merge the base
  symbol's pin/graphic sub-blocks under the derived symbol's own name.
  `grep '(extends "'` across a schematic's `lib_symbols` block to find every
  instance before trusting any "why is this pin unconnected" investigation.
- **This schematic has a real, recurring bug pattern independent of ERC**:
  multiple components have two adjacent/related pins with their global
  labels swapped or shifted by one position (found and fixed this session:
  J3 pins 1/19 VCC\<->Y2, J3 pins 5/15 S4\<->INHIBIT, J1 pins 3+6/4
  VSS\<->VDD, U5 (flash) pins 4/8 GND\<->VCC, U1's entire IOVDD bank
  mislabeled GND, U1 pins 30/31/33/34/66/67
  XIN/XOUT/SWCLK/SWDIO/USB_DM/USB_DP all cross-wired with each other, U5's
  QSPI pins 1/2/3/5/6/7 largely cross-wired against the real
  DI/DO/WP/HOLD/CLK/CS pin names, U6 (level shifter) pin 8/9 D7/D6
  swapped-with-gap, U8 (BLE) UART_RX/RST wired to the wrong (spare GPIO)
  pins entirely. Also found: U6's B-side (MCU-facing) pins 13/14/15/16/
  17/18/20 against `DATA0_MCU`..`DATA6_MCU` don't follow a consistent
  bit-order either -- **not yet fixed**, needs the intended bit-order
  confirmed (from RP2350 firmware/GPIO assignment) before touching it,
  unlike the others above which were unambiguous from pin *names* alone.
  None of these show up as ERC errors on their own
  (compatible pin types on both ends), only as *wrong* connections -- ERC
  is silent about a wire being right instead of wrong. The only way to
  catch these is to cross-check each pin's **real name** (from the cached
  symbol's own pin table) against what's **actually wired** there (from a
  `kicad-cli sch export netlist --format kicadxml` export), not to trust
  that an already-non-floating pin is correctly connected. Worth doing this
  cross-check systematically (per component) rather than only when ERC or
  the user flags something odd, since this file's history clearly includes
  at least one batch-edit that shifted labels without shifting pins.
- **A single new pin/label addition can silently merge two entire global
  nets** (e.g. GND and +3V3) even when kicad-cli reports no new errors
  immediately -- always diff net *membership counts* via
  `export netlist --format kicadxml` before/after a wiring batch, not just
  the ERC error count, since a bad merge can *reduce* total ERC violations
  (fewer "undriven power pin" errors once GND folds into the already-driven
  +3V3) while being catastrophically wrong. Root cause found once this
  session (`U1` pin 62 `VREG_PGND`, a `length 3.81` pin unlike this
  symbol's other `length 2.54` pins) was never fully explained -- adding a
  same-named global label near it merged GND into +3V3 even after moving
  the wire's pin-side endpoint away to a floating position, so it wasn't a
  simple coordinate collision either. Bisect by re-adding a batch's wires
  one at a time (checking net counts after each) if this happens again
  rather than trying to reason it out statically. Workaround that worked:
  connect the problem pin via a direct real wire to an already-known-good
  same-net pin's exact coordinates instead of an independent stub+label at
  the problem pin's own computed position.
- **`kicad-cli` netlist/ERC output is not a substitute for looking at the
  schematic -- it verifies final connectivity, not layout sanity.** Found
  2026-08-25: R2's GND stub landed 0.16mm from R1's own routed path to S4 --
  netlist-clean (different coordinates, no false connection) but visually
  merged into what looked like one shared junction, which is exactly the
  kind of thing that makes a schematic unworkable to click/edit in the GUI
  even though ERC sees nothing wrong. This class of bug is *invisible* to
  text-based verification and was only found because the user reported it
  after re-checking in the actual KiCad GUI multiple times. **Render and
  visually check any wire routing with more than one segment/waypoint**
  (not simple single-stub pin-to-label connections, which are low-risk) --
  don't trust netlist cleanliness alone as proof the schematic is actually
  editable/correct-looking.
  - How to actually see it (no direct KiCad GUI access, and the `claude-in-
    chrome` tool refuses `file://` URLs): `mcp__kicad__export_schematic_svg`
    to render, then serve the output directory over a plain local HTTP
    server (`python3 -m http.server <port>` from that directory,
    `run_in_background: true`) since the browser needs `http://`, not
    `file://`. Navigate Chrome to `http://localhost:<port>/<name>.svg`,
    then use `javascript_tool` to directly set the root `<svg>` element's
    `viewBox` attribute to the mm-coordinate region of interest (e.g.
    `svg.setAttribute('viewBox', 'x y width height')`) before screenshotting
    -- far more precise and reliable than the `computer` tool's own `zoom`
    action, which occasionally triggers real browser-level zoom instead of
    just a cropped screenshot (produces a garbled tiled/repeated render;
    reload the page to reset it if that happens). Close the tab and kill the
    HTTP server (`pkill -f "http.server <port>"`) when done.
- **Structural label audits (does every label touch a real pin?) do not
  catch missing signals -- only wrong/extra/floating ones.** Found
  2026-08-25: `BLE_UART_RX` was never wired to U1 at all across an entire
  prior session of "comprehensive" cleanup passes, because every audit
  checked "does this label reach a pin" (a property that's vacuously true
  for a label that doesn't exist) rather than diffing the wired signal set
  against the user's authoritative requirement list. **After any wiring
  pass on a component with a known, finite signal budget (e.g. U1's 35
  required nets), export the netlist and diff `set(required_signals)`
  against `set(nets where U1 is a member)` explicitly** -- don't rely on
  ERC/structural checks alone, they're blind to omissions by construction.
- **Two more real, pre-existing miswirings found by this same diff, both
  invisible to ERC (which has no error type for "electrically valid but
  semantically wrong net"):** (1) a stray leftover `BLE_UART_RX` label was
  sitting exactly on a GPIO pin later reassigned to `AD7`, silently
  shorting the two nets together (only caught because the netlist showed
  `AD7` gaining an unexplained extra member after an unrelated edit --
  investigate any *surprise* net-membership change, not just missing
  ones). (2) `BLE_UART_TX` was wired to U8 (RN4871) pin 16 (`P2_0`, a
  generic GPIO) instead of pin 8 (`UART_TX`, the module's actual UART
  output) -- always check the *symbol's own pin names* (grep the
  `lib_symbols` block, e.g. `RN4871_1_1`) against what a label claims to
  connect to, not just whether a wire+label pair is internally
  well-formed.
- **RN4871 UART pin numbers** (from this schematic's `RF_Bluetooth:RN4871`
  symbol, unit `RN4871_1_1`): pin 7 = `UART_RX` (module receives -- wire
  to net `BLE_UART_RX`, i.e. what the MCU transmits), pin 8 = `UART_TX`
  (module transmits -- wire to net `BLE_UART_TX`, i.e. what the MCU
  receives). Net names are from the module's own pin-name perspective, not
  the MCU's -- don't rename them to "fix" the apparent RX/TX inversion.
  Pin 6 = `P1_6`, used for `BLE_WAKE` (user's explicit choice, confirmed
  earlier session -- not a dedicated wake pin, just a spare GPIO).
- **Floating-point-garbage coordinates defeat exact-string coordinate
  matching in hand-written delete/find scripts.** Found 2026-08-25: a wire
  endpoint stored as `123.02000000000001` instead of `123.02` (both
  represent the same grid point) caused a regex looking for the literal
  string `"123.02"` to miss it. When writing scripts to find/delete
  wire+label pairs by tip coordinate, compare with `abs(float(a)-float(b))
  < 0.001`, never exact string equality.

## U1 (RP2350B) GPIO map (as of 2026-08-25, contiguous-bus redesign)

Full 48-GPIO RP2350B (not the 26-usable-pin Pico 2 W module) let every
timing-critical signal land on a single contiguous GPIO range, with the
address bus starting at GPIO0 so firmware can read it with **zero bit
shifting**: `addr = gpio_in & 0x1FFF` directly gives AD0-AD12. Priority
order (per user's explicit instruction: "the timing-critical signals are
write trigger, read trigger, AD0-12, and D0-D7 -- those should get
priority") was address bus, then data bus, then the two bus-strobe
triggers, all placed before the lower-priority I2C/SD/BLE signals:

| GPIO | Pin | Signal | | GPIO | Pin | Signal |
|---|---|---|---|---|---|---|
| 0 | 77 | AD0 | | 18 | 18 | DATA5_MCU |
| 1 | 78 | AD1 | | 19 | 19 | DATA6_MCU |
| 2 | 79 | AD2 | | 20 | 20 | DATA7_MCU |
| 3 | 80 | AD3 | | 21 | 21 | TRIG_RD |
| 4 | 1 | AD4 | | 22 | 22 | TRIG_WR |
| 5 | 2 | AD5 | | 23 | 23 | GREENPAK1_SDA |
| 6 | 3 | AD6 | | 24 | 25 | GREENPAK1_SCL |
| 7 | 4 | AD7 | | 25 | 26 | GREENPAK2_SDA |
| 8 | 6 | AD8 | | 26 | 27 | GREENPAK2_SCL |
| 9 | 7 | AD9 | | 27 | 28 | ROM_SRAM_STATUS |
| 10 | 8 | AD10 | | 28-31 | 36-39 | spare |
| 11 | 9 | AD11 | | 32 | 40 | BLE_UART_RX |
| 12 | 11 | AD12 | | 33 | 42 | BLE_UART_TX |
| 13 | 12 | DATA0_MCU | | 34 | 43 | BLE_WAKE |
| 14 | 13 | DATA1_MCU | | 35-39 | -- | spare |
| 15 | 14 | DATA2_MCU | | 40 | 49 | SD_MISO |
| 16 | 16 | DATA3_MCU | | 41 | 52 | SD_CS |
| 17 | 17 | DATA4_MCU | | 42 | 53 | SD_SCK |
| 18 | 18 | DATA5_MCU | | 43 | 54 | SD_MOSI |
| 19 | 19 | DATA6_MCU | | 44-47 | -- | spare |
| 20 | 20 | DATA7_MCU | | | | |
| 21 | 21 | TRIG_RD | | | | |
| 22 | 22 | TRIG_WR | | | | |
| 23 | 23 | GREENPAK1_SDA | | | | |
| 24 | 25 | GREENPAK1_SCL | | | | |
| 25 | 26 | GREENPAK2_SDA | | | | |
| 26 | 27 | GREENPAK2_SCL | | | | |

Important correction mid-redesign: the address bus is **AD0-AD12** (13
bits, the low-order bits needed to address within the fixed 8K window --
matches the original GPIO-budget spec "13 address pins for 8K window"),
**not** AD2-AD14 as an earlier scattered layout had it. AD13/AD14 remain
wired to J3+U3(GreenPAK1)+U9(GreenPAK2) only -- U1 doesn't need them since
which 8K window is selected is a hardware chip-select decision, not
something firmware reads. GPIO40/ADC0 (pin 49) previously carried two
stray duplicate `RUN` labels (leftover garbage, unrelated to the real RUN
pin at pin 35/+3V3) -- both deleted, net `RUN` no longer touches U1 at all.

**SD SPI pins corrected 2026-08-25, after checking the real RP2350
datasheet GPIO function table** (the doc comment at the top of the Pico
SDK's `hardware/gpio.h`): the original GPIO28-31 assignment for
SD_CS/MISO/MOSI/SCK didn't align with any real SPI peripheral's funcsel
pins (off by one pin each vs. SPI1's real RX/CSn/SCK/TX order), which
would have forced slow bit-banged SPI for SD card I/O. GPIO40-43 is a
genuine, contiguous, correctly-ordered SPI1 block (F1 funcsel:
GPIO40=SPI1 RX/MISO, 41=SPI1 CSn, 42=SPI1 SCK, 43=SPI1 TX/MOSI) -- moved
SD_CS/MISO/MOSI/SCK there so the SD card can use RP2350's hardware SPI1
peripheral directly (CS itself doesn't strictly need the CSn funcsel pin,
since FatFs SD drivers toggle CS as a plain GPIO, but keeping it in the
same contiguous block costs nothing since GPIO41 was spare anyway).
GPIO28-31 became spare instead (total spare count unchanged at 13, just
relocated). **Always check a signal's real GPIO funcsel table before
assigning it to a "for-firmware-convenience" schematic pin** -- this bug
class (net name suggests a peripheral, but the assigned pin doesn't
actually support that peripheral's hardware function) is invisible to
ERC/netlist checks entirely, since electrically the wiring is perfectly
valid -- it just makes the intended firmware approach (hardware SPI)
impossible without a further pin change.

The GreenPAK I2C pins (GPIO23-26) have the same kind of misalignment (they
don't form a matched SDA/SCL pair on the same I2C instance -- GPIO23 is
really I2C1 SCL, GPIO24 is I2C0 SDA, etc.) but were deliberately left
as-is: that link is low-speed/non-critical (occasional mode-control
comms, not the timing-critical bus), so bit-banged (software) I2C is the
intended implementation there, not a bug needing a schematic fix.

BLE UART (GPIO32/33) was checked too and turned out to already be a
correct match: GPIO32=UART0 TX, GPIO33=UART0 RX, exactly right for the
MCU's TX-into-module's-RX (`BLE_UART_RX` net) / MCU's RX-from-module's-TX
(`BLE_UART_TX` net) convention already established -- no change needed.

## J3 (40-pin PC-1500 cartridge slot) pinout

**Authoritative source**: *PC-1500 Technical Reference Manual* §4-3-1
(p.102/p.106), transcribed into
`O:\PC1500-Expansion\docs\expansion-bus-pinout.md` (also covers the 60-pin
connector -- not relevant to this board). That file is the source of truth;
if this table and that file ever disagree, trust the file (or re-derive
from the manual), not this copy.

**History**: an earlier pass this session (2026-08-25) "fixed" pins 1/19 and
5/15 by cross-referencing the schematic's own netlist -- which turned out to
still be substantially wrong for the whole D0-D7 and address-bus range
(reversed/offset, not just two swapped pins). The user caught this by
supplying the real GreenPAK1/GreenPAK2 pinouts (which cite J3 pins directly)
and then the full manual-sourced table above. **Moral: for J3 specifically,
never trust what the schematic's netlist already has as ground truth -- it
has been wrong at multiple different scopes (individual pin swaps, then an
entire bus range) across this one session alone. Always re-derive from the
manual-sourced doc.**

| Pin | Signal | Pin | Signal | Pin | Signal | Pin | Signal |
|-----|--------|-----|--------|-----|--------|-----|--------|
| 1 | VCC | 11 | D3 | 21 | GND | 31 | AD6 |
| 2 | PV | 12 | D2 | 22 | AD15 | 32 | AD5 |
| 3 | PU | 13 | D1 | 23 | AD14 | 33 | AD4 |
| 4 | Y0 | 14 | D0 | 24 | AD13 | 34 | AD3 |
| 5 | S4 | 15 | INHIBIT | 25 | AD12 | 35 | AD2 |
| 6 | DME0 | 16 | S1 | 26 | AD11 | 36 | AD1 |
| 7 | D7 | 17 | S2 | 27 | AD10 | 37 | AD0 |
| 8 | D6 | 18 | S3 | 28 | AD9 | 38 | OD |
| 9 | D5 | 19 | Y2 (chip-select signal, unused on this board) | 29 | AD8 | 39 | R/W |
| 10 | D4 | 20 | VGG | 30 | AD7 | 40 | GND |

Note the address bus is named `AD0`-`AD15` on J3 (and in GreenPAK1/2's own
pinout, see below), **not** `ADDR0`-`ADDR15` -- that was this schematic's
own (wrong) invented convention, now replaced. `AD0` is pin 37, `AD15` is
pin 22 -- i.e. bit number *decreases* as pin number increases, opposite of
what the old wrong labeling assumed.

**AD-bit-number ambiguity**: "A12"/"AD12"/"ADDR12" always means bit 12 of
the address, but *which* address -- the raw one from J3/the LH5801, or the
one actually presented to the 62256 SRAM's own A12 pin (which can differ,
since GreenPAK1 mediates address bits 11-14 for the SRAM, see below) --
depends on context the user has to supply and I have to track. In this
schematic: raw/LH5801-side bits use `AD0`-`AD15` as their net name; the
SRAM-side (post-GreenPAK) versions of bits 11-14 use `SRAM_A11`-`SRAM_A14`
specifically so the two are never confused by a shared net name.

Pin 5 (S4) is otherwise unconnected on this board -- it briefly fed a
resistor divider (R1 pull-up to VGG, R2 pull-down to GND, midpoint tied to
J3 pin 5 *and* U3/GreenPAK1 pin 2/IO13) meant as an automatic PC-1500 vs
PC-1500A sense circuit, but the user decided (2026-08-26) that relying on
S4's real behavior across both variants was unreliable, and that whoever
installs the module already knows which base unit they have. R1/R2 were
removed and J3 pin 5 was left N/C; **R21**, a manual jumper on U3 pin 2
(IO13) itself, replaces it -- see the "Spare-pin flexibility additions"
section below. Before removal, this divider had its own history of pin
mixups: R1's pull-up briefly went to `+3V3` instead of `VGG`, and the whole
divider was briefly wired to pin 15 instead of pin 5 (pin 5/15 labels were
swapped); U3 pin 2 had also briefly been on U3 pin 12 (the dedicated SDA
pin, wrong) before GreenPAK1's full real pinout came in.

## GreenPAK1 (U3) and GreenPAK2 (U9) pinout

Given directly by the user (from the real board's design docs, not derived
from the schematic) on 2026-08-25, after finding the schematic's existing
GreenPAK wiring was extensively wrong (SDA/SCL routed to generic IO4/IO5
instead of the dedicated pins 12/13, an entire IO0-IO3 sequence reversed,
SRAM address/control signals on the wrong IO pins entirely). SLG46826G pin
numbers: 1=IO14, 2=IO13, 3=IO12, 4=IO11, 5=IO10, 6=IO9, 7=VDD2, 8=IO8,
9=IO7, 10=GND, 11=IO6, 12=SDA, 13=SCL, 14=IO5, 15=IO4, 16=IO3, 17=IO2,
18=IO1, 19=IO0, 20=VDD.

| Pin | GreenPAK1 (U3) | GreenPAK2 (U9) |
|-----|-----------------|-----------------|
| 20 (VDD) | VGG | VGG |
| 19 (IO0) | AD11 (J3 pin 26) | AD11 (J3 pin 26) |
| 18 (IO1) | AD12 (J3 pin 25) | AD12 (J3 pin 25) |
| 17 (IO2) | AD13 (J3 pin 24) | AD13 (J3 pin 24) |
| 16 (IO3) | AD14 (J3 pin 23) | AD14 (J3 pin 23) |
| 15 (IO4) | AD15 (J3 pin 22) | AD15 (J3 pin 22) |
| 14 (IO5) | RW (J3 pin 39) | RW (J3 pin 39) |
| 13 (SCL) | to MCU (`GREENPAK1_SCL`) | to MCU (`GREENPAK2_SCL`) |
| 12 (SDA) | to MCU (`GREENPAK1_SDA`) | to MCU (`GREENPAK2_SDA`) |
| 11 (IO6) | SRAM_A11 (AD11 to 62256) | read trigger to MCU (`TRIG_RD`) |
| 10 (GND) | GND | GND |
| 9 (IO7) | SRAM_A12 (AD12 to 62256) | write trigger to MCU (`TRIG_WR`) |
| 8 (IO8) | SRAM_A13 (AD13 to 62256) | DME0 (J3 pin 6) |
| 7 (VDD2) | VGG | VGG |
| 6 (IO9) | SRAM_A14 (AD14 to 62256) | OD (J3 pin 38) |
| 5 (IO10) | SRAM_WE (R/W to 62256) | SRAM_CS (CS to 62256) |
| 4 (IO11) | PV (J3 pin 2) | PV (J3 pin 2) |
| 3 (IO12) | `PRE_DME0_CS` -- to GreenPAK2 | `PRE_DME0_CS` -- from GreenPAK1 |
| 2 (IO13) | `PC1500_SENSE` -- R21 jumper to GND (bridged=PC-1500, open=PC-1500A; needs GreenPAK Designer internal pull-up config for the open state, see below) | spare |
| 1 (IO14) | `ROM_SRAM_STATUS` -- SRAM/ROM toggle status to MCU | spare |

(Historical note, resolved 2026-08-25: `GREENPAK2_SCL` was briefly missing
its U1 side earlier this session, during the scattered pre-contiguity
layout. The GPIO0-34 contiguity remap wired it properly to GPIO26/pin27 --
see the U1 GPIO map above.)

**Known past mistakes**: pin 1 (VCC) and pin 19 (Y2) were swapped in the
schematic -- a Schottky diode (D1) meant to feed VCC from USB VBUS, plus
U4's regulator EN pin, had both been wired to pin 19 instead of pin 1. Found
by tracing a `multiple_net_names` ERC violation back to its real components
rather than trusting the existing labels; fixed by swapping the two
`global_label` texts at pins 1 and 19, not by moving any wires. Pins 5 (S4)
and 15 (INHIBIT) were separately swapped the same way -- the user caught
this one directly (not from an ERC violation), since a mislabeled control
pin with a plausible-looking existing connection doesn't trip any ERC rule.
If a future
session finds pin numbers here disagreeing with the manuals again, trust the
manuals and re-verify against the schematic, not the other way around.

## Spare-pin flexibility additions (2026-08-25, after J3 footprint/outline work)

Three small schematic additions, all aimed at preserving future flexibility
rather than fixing anything broken:

- **R16** (`Jumper:Jumper_2_Bridged`, footprint
  `Jumper:SolderJumper-2_P1.3mm_Bridged_RoundedPad1.0x1.5mm`) bridges `PU`
  (J3 pin 3, previously wired to J3 only and otherwise dangling) to
  `GREENPAK2_IO14` (U9/GreenPAK2 pin 1, previously spare). The jumper is
  the *only* link between those two nets -- cutting its solder bridge
  frees IO14 for something else later without a rework. This is a new
  component-type pattern for this schematic (no prior jumper existed);
  reuse `Jumper:Jumper_2_Bridged` for any future cuttable-jumper need
  rather than a 0Ω resistor -- the user explicitly chose solder-jumper
  pads over a resistor for easier disconnection (hobby knife, not a
  soldering iron).
- **AD13/AD14/AD15** (already reaching J3 + both GreenPAKs, but not U1)
  now also reach U1 GPIO28/29/30 respectively -- global_label stubs
  added at those 3 previously-spare U1 pins, auto-joining the existing
  nets by name. Firmware can read these later if a design ever needs
  more than the 8K window's 13 address bits.
- **J5** (`Connector_Generic:Conn_01x10`, `PinHeader_2.54mm:PinHeader_1x10_P2.54mm_Vertical`,
  Value `SPARE_GPIO_U1`) breaks out U1's remaining 10 spare GPIOs
  (31, 35-39, 44-47) to header pins, net-named plainly `GPIO31`/`GPIO35`/
  etc (no `/ADC*` suffix, to sidestep the `{slash}`-escaping quirk noted
  above).
- **J6** (`Connector_Generic:Conn_01x01`, `PinHeader_2.54mm:PinHeader_1x01_P2.54mm_Vertical`,
  Value `SPARE_GPIO_U9`) breaks out U9's one remaining spare pin
  (IO13/pin2) as `GREENPAK2_IO13`.
- **R21** (`Jumper:Jumper_2_Bridged`, same footprint/pattern as R16, added
  2026-08-26) bridges `PC1500_SENSE` (U3/GreenPAK1 pin 2, IO13) to `GND`,
  reusing the board footprint freed by removing R1/R2's old S4 divider
  (see above). Bridged (soldered closed) = PC-1500; left open = PC-1500A.
  **Open dependency, not resolvable from the schematic alone**: a floating
  digital input is bad practice, so IO13 needs its pin-type set to
  "digital input with internal pull-up" in the GreenPAK Designer project
  so the open/PC-1500A state reads a clean HIGH instead of an undefined
  floating level -- no GreenPAK Designer project file exists in this repo
  tree to make that change in yet (same class of gap as the ROM/SRAM-mux
  register-sequence dependency noted elsewhere).
- **GreenPAK1 (U3) has zero spare *unused* pins** -- confirmed via full
  pin-by-pin inventory; all 20 pins are wired to something (IO13 above is
  "spare" only in the sense that its function was repurposed, not that
  it's unconnected).
- Both new components (`Jumper:Jumper_2_Bridged`,
  `Connector_Generic:Conn_01x10`, `Connector_Generic:Conn_01x01`) needed
  their `lib_symbols` definitions copied in from the installed KiCad 10
  stock libraries (`Jumper.kicad_sym`, `Connector_Generic.kicad_sym`) --
  they didn't exist in this schematic before. When copying a stock
  symbol in this way: **only the outermost `(symbol "Name" ...)` gets
  renamed to `"Library:Name"`; nested `_1_1`-suffixed sub-symbol names
  stay as-is** -- confirmed against the existing Conn_01x03 entry
  already in this file before copying the new ones.
- **Bug caught during this pass, worth remembering for any future
  hand-written component-instance generator script**: a Python string-%
  formatting call with many positional `%s` placeholders is easy to get
  subtly wrong by miscounting the tuple -- caught here only because a
  reference number (e.g. "R16") showed up in the *Value* field instead
  of the intended value string, for all three new parts at once. It was
  electrically harmless (Value doesn't affect connectivity, so ERC/
  netlist checks didn't catch it) but wrong in the visible schematic --
  only caught by actually reading text content back out of the rendered
  SVG DOM (`document.querySelectorAll('text')`), not by trusting the
  generator script's own success message. **Netlist/ERC verification
  proves connectivity; it does NOT prove properties/labels are correct
  -- always also spot-check rendered text for anything with cosmetic-only
  fields (Value, Description) that no automated check covers.**
  Fixed with direct string-replace edits once found.
- Placed in previously-empty space below y=250 (R16 at (50,320), J5 at
  (150,320), J6 at (300,320)) -- confirmed via full component-position
  inventory that nothing existed below y≈250 anywhere on the sheet
  before placing these.

## Component placement done (2026-08-25/26, direct-file-edit)

All 33 real components (32 + J3) are now placed in `PC1500-RP2350B-BLE.kicad_pcb`
with real footprint geometry (copied wholesale from the actual KiCad 10 stock
library files, not hand-abbreviated) and real per-pad net assignments (from
`kicad-cli sch export netlist`). This was done entirely by direct file edit
(no GUI access), using the same pattern as everything else this session but
at much larger scale -- worth understanding before touching this file again:

- **Floor plan**: top side (F.Cu) has U1 (near J3, for short bus traces),
  J1 (microSD) and U8 (RN4871 BLE, `rot180` so its antenna keepout points
  away from J3) per the user's explicit top/bottom + antenna-orientation
  spec, plus a few small passives and J4. Bottom side (B.Cu) has everything
  else: U2/U3/U9/U6/U5, J2 (USB-C), J5, J6, SW1, and all remaining passives.
  **Correction (2026-08-26): J2 originally protruded ~2mm past the Y=100.1
  board edge and I initially called that "correct for an edge connector" --
  the user explicitly rejected this ("having the frame of the USB connector
  protrude past the edge of the board in any direction is unacceptable").
  There is no real-world scenario on THIS board where an edge overhang is
  fine; that reasoning only applies to J3, whose contacts are SUPPOSED to be
  flush with a card-edge slot. Fixed by tightening the U3/U9/U6/U5 column
  spacing (~1mm gaps down to 0.3mm) to free vertical room, then moving J2
  fully within bounds to `(160, 95.1, rot=0, side='B')`. Don't reintroduce
  edge overhang for any connector without being told to.** J5 (a 10-pin
  2.54mm header, 25.6mm long)
  got its own dedicated left-edge strip since it alone needs more linear
  space than most other single parts on this tiny 31.8x43mm board -- if
  redesigning this area, that length constraint is the first thing to
  re-check.
- **Real bugs found and fixed just from checking real footprint sizes**
  (independent of placement): J1's footprint was a **full-size SD card**
  slot (`SD_Card_Device_16mm_SlotDepth`, 24x32mm -- nearly the whole board
  by itself) despite the project always having said "J1=microSD"; fixed to
  `microSD_HC_Molex_104031-0811` (13.68x16.25mm, pad count/pinout still
  compatible with the schematic's generic 9-pin `SD_Card_Device` symbol).
  L2's footprint name (`L_0806_2016Metric`) didn't exist in `Inductor_SMD`
  at all (a stray digit -- real size is `L_0805_2012Metric`), matching a
  `footprint_link_issues` ERC warning that had been sitting unfixed since
  the very first plan of this multi-session project. **Lesson: "the
  footprint field has some string in it" is not the same as "the footprint
  is right" -- always compute/sanity-check real physical size before
  placement, not just trust whatever name is already there.**
- **Through-hole (PTH) parts collide across F/B sides, unlike SMD.** J4/J5/J6
  (2.54mm pin headers) are through-hole -- their drilled holes span the
  full board thickness, so a "top-side" header and a "bottom-side" header
  at overlapping X/Y genuinely collide even though SMD parts on opposite
  sides never would. First placement pass put J4 right on top of J5's
  column for exactly this reason (caught by DRC `shorting_items`, not by
  a same-side-only AABB pre-check). **When placing any through-hole part,
  check it against every other through-hole part regardless of side.**
- **Confirmed via testing, not assumption: KiCad does NOT store a separate
  mirror flag for back-placed footprints** -- the actual geometry (every
  pad `(at ...)`, every `fp_line`/`fp_poly` coordinate) must be mirrored
  (negate local X) by whoever generates the placed instance, and all
  `F.*`/`B.*` layer names must be swapped. Verified against a real
  KiCad-10-authored back-side footprint in the installed demo library
  (`cm5_minima/CM5_MINIMA_3.kicad_pcb`) before trusting this, the same way
  the footprint-rotation convention was verified earlier this session.
  **Text specifically needs one more thing beyond coordinate mirroring:**
  an explicit `(justify mirror)` inside its `(effects ...)` block, or KiCad
  flags `nonmirrored_text_on_back_layer` even though the position is
  correct -- also confirmed against that same real demo file.
- **A custom footprint mixing the legacy `(fp_text reference/value ...)`
  form with a newer `(property "Reference"/"Value" ...)` form (e.g. from
  adding one without removing the other) causes DRC to badly misattribute
  pads** -- our own hand-authored `SHARP-PC-CARTRIDGE.kicad_mod` had
  exactly this (a leftover `fp_text reference "REF**"` from when it was
  first hand-converted from KiCad 5 format, plus a newer `property`
  version added later) and DRC reported every pad on it as belonging to
  a nonexistent component called `REF**`, cascading into bogus
  `shorting_items` reports. Fixed by converting the footprint file itself
  to consistently use the modern `property` form -- **any footprint
  generator/placer script should defensively strip BOTH old and new forms
  before re-adding a fresh one**, which this project's placer script now
  does.
- **Modern KiCad (10) does not use `(net_class ...)` in the `.kicad_pcb`
  file at all** -- confirmed absent from every installed stock demo/
  template file. Don't add one "to fix a DRC clearance issue" based on
  older KiCad knowledge; it won't be read. Clearance defaults for a bare
  hand-built board (like this project's own `PC1500-RP2350B-BLE.kicad_pcb`,
  which has no explicit design-rules file) come from KiCad's own built-in
  fallback, not anything embedded in the board text itself.
- **CORRECTED 2026-08-26 -- the "140 shorting_items + 140 solder_mask_bridge
  is inherent to card-edge connectors" conclusion above was WRONG.** The
  user asked "the breakout cartridge pcb shows the 1.27mm pitch contacts on
  J3, while this pcb does not -- pretty sure these are needed," which
  prompted a real re-investigation instead of re-asserting the prior
  (wrong) conclusion. **Root cause: every one of J3's 40 pads had local pad
  rotation `180deg` when it needed to be `90deg`.** Pad size was `(5.6 0.8)`
  (or `6.6 0.8` for the two end pads) -- correct raw dimensions -- but at
  `rot=180` the rendered/DRC'd shape stayed 5.6mm wide x 0.8mm tall in
  board space (no axis swap), when the design needs 0.8mm wide (matching
  the 1.27mm pitch, leaving a real 0.47mm gap) x 5.6mm tall (along the
  insertion direction). Result: all 40 "fingers" were 5.6mm-wide blobs
  overlapping their neighbors by ~4.3mm each -- genuinely shorted copper,
  not just tight-but-legal clearance. This is also why the isolation test
  (J3 alone on an empty board, same 140+140 count) "confirmed" the wrong
  theory: isolating it correctly ruled out *other components* as the
  cause, but I stopped one level too early and didn't question whether
  J3's own pad geometry was internally correct.
  **KiCad pad-rotation gotcha found while root-causing this (verified with
  isolated single-pad test boards, not assumed): a pad's own `(at x y rot)`
  rotation does NOT visibly compose with its parent footprint's placement
  rotation for shape/bbox purposes in kicad-cli's plotter/DRC -- swap
  parity (does width/height swap in board space) depends only on the
  pad's own stored `rot` value mod 180 (90/270 swaps, 0/180 doesn't),
  regardless of the footprint's rotation.** This contradicts the naive
  "angles just add" assumption and cost real time to pin down -- confirmed
  via minimal hand-built test `.kicad_pcb` files with a single pad at
  known footprint-rotation/pad-rotation combinations, reading back
  `getBBox()` in a browser, not by re-deriving trig by hand again.
  **Fixed**: changed all 40 pads' rotation from `180` to `90` in both
  `Sharp-PC-Footprints.pretty/SHARP-PC-CARTRIDGE.kicad_mod` (the library
  footprint) and the already-placed J3 instance in
  `PC1500-RP2350B-BLE.kicad_pcb` (backups kept alongside both:
  `.bak_before_rotfix` / `.bak_before_j3padrotfix`). DRC violations
  dropped **404 -> 103** immediately; `shorting_items` and
  `copper_edge_clearance` on J3 dropped to 0, `solder_mask_bridge` dropped
  from 140 to 2 (both now on X1/J4, unrelated to J3). Verified visually
  too: a zoomed SVG render now shows 40 distinct 0.8mm fingers with real
  gaps, matching the reference breakout board's look, where before it was
  one solid red/blue block.
  **Lesson: when a DRC "explanation" survives one isolation test, that
  doesn't mean the explanation is right -- it can mean the isolation test
  answered a narrower question ("is it something else?") than the one
  actually being asked ("is this component's own geometry correct?").
  When a user's direct visual read contradicts a prior conclusion, treat
  that as a strong signal to re-open the investigation, not defend the
  earlier finding.**
- After this fix, the remaining 103 DRC violations are unrelated to J3:
  `silk_over_copper` (42), `lib_footprint_mismatch` (23), `clearance` (10,
  small passives/X1/J4 spacing), `courtyards_overlap` (10), `silk_overlap`
  (9), `silk_edge_clearance` (5), `pth_inside_courtyard` (2),
  `solder_mask_bridge` (2, X1/J4). None of these are J3-related -- ordinary
  small-component-crowding polish, verified via `kicad-cli pcb drc` (never
  trust the Python self-checker's AABB approximation alone -- it caught
  most issues but missed the PTH cross-side case entirely, and would never
  have caught this pad-rotation bug either since it doesn't render shapes).
- Remaining DRC categories not yet worked through in this pass (silk vs
  copper/silk overlap, `lib_footprint_mismatch`, `copper_edge_clearance`,
  `courtyards_overlap` down from 12 to 9, `pth_inside_courtyard`) are
  smaller-scale polish items better suited to interactive GUI review
  (click a violation, see it highlighted) than further blind script
  iteration -- flagged to the user rather than ground through by hand.

- **U2's SRAM footprint was wrong — fixed 2026-08-26, same class of bug as
  J1/L2 above.** Real part is **Alliance Memory `AS6C62256-55SIN`** (330-mil
  SOP, industrial temp -- confirmed from Alliance's own datasheet PDF,
  fetched and read directly rather than trusting search snippets: body
  ~8.4mm, lead-tip span ~11.5-12.1mm, D=18.49mm max, pitch 1.27mm, same
  standard pin order as Hitachi's `HM62256B` -- A14 on pin 1 through Vcc
  on pin 28, no remapping needed since this isn't the TSOP case). The
  schematic's `Value` field had already been corrected to
  `AS6C62256-55SIN` in an earlier session (the user had told me this
  before), but **the `Footprint` field was never updated to match** -- it
  still pointed at `Package_SO:SOIC-28W_7.5x18.7mm_P1.27mm`, a 300-mil
  body about 1mm too narrow for the real part. **Lesson: a correct Value
  field is not evidence the Footprint field is also correct -- check both
  independently, every time, the same discipline as checking real
  physical size before trusting any footprint name.** Fixed by switching
  to KiCad's own stock `Package_SO:SOP-28_8.4x18.16mm_P1.27mm` (its
  `descr` field cites an ISSI 62-65C256AL datasheet -- same 330-mil SOP
  package family as the Alliance part, found by searching the stock
  library rather than hand-building a custom footprint). Re-placed via
  `replace_components.py`, same position/rotation/side as before; DRC
  violation count and category breakdown were unchanged after the swap
  (103, same categories), confirming the ~0.9mm-wider body still clears
  its neighbors with no new collisions.

## Known open items (as of the RP2350 port + KiCad sessions)

- PSoC5 board: decoupling capacitors on power-supply connections, and the
  VBAT/VBOOST/IND/VSSB pin treatment (boost circuit not needed on this
  board) -- raised but not resolved; the user redirected to other work
  before this was finished.
- (Resolved 2026-08-25: a real schematic exists now --
  `PC1500-RP2350B-BLE.kicad_sch` -- and `RP2350B/board_pins.h`'s GPIO
  numbers are settled/final, checked against both the schematic and the
  real RP2350 GPIO funcsel table. `../RP2350/` (Pico 2 W) is kept for
  reference only, no longer developed against.)
- RP2350B board: the GreenPAK-comms protocol over the 2-wire I2C-style link
  isn't designed yet -- `monitor.c`'s `EXP_COMMAND_ROM_FROM_SRAM`/
  `ROM_FROM_MCU` cases are placeholders pending that.
- RP2350B board: if the address bus ever needs to narrow below 13 bits
  (e.g. to free more pins), the 6K ROM region's boot-load path needs a
  fundamental rethink -- it currently reaches SRAM by routing through
  this same MCU-facing bus (`ROM_FROM_MCU`), which requires the full
  8K/13-bit address range to be visible to the MCU during that copy.
- J3 footprint (Resolved 2026-08-25, corrected same day -- see below):
  the schematic referenced `Sharp-PC-Footprints:SHARP-PC-CARTRIDGE`, a
  placeholder name with no actual footprint or library behind it
  anywhere -- would have blocked "Update PCB from Schematic" entirely.
  The user pointed at
  https://github.com/kaibader/Sharp-PC-Breakout-Boards (CC BY-SA 4.0,
  Kai C. Bader) as the real reference. **That repo has two different
  board designs, both with a J3** -- `Cartridge-Breakout-Board/` (a
  40-pin **through-hole** connector, real part `KLS1-503-40-S`) and
  `Breakout-Cartridge/` (a 40-pin **SMD card-edge/gold-finger**
  connector, literally named `SHARP-PC-CARTRIDGE` in its own footprint
  library reference). First pass used the wrong one
  (`Cartridge-Breakout-Board`) purely because it was the first directory
  found -- the user then corrected this explicitly: `Breakout-Cartridge`
  is "the actual board footprint and card edge layout." **This isn't
  just a wrong-footprint mistake, it's a materially different physical
  connection method**: the real PC-1500 cartridge slot is a card-edge
  slot, and this board's J3 end is meant to physically insert into it
  via exposed edge-finger pads -- there's no separate discrete connector
  part to place. Converted the real footprint (40 SMD rect pads split
  F.Cu/B.Cu for front/back fingers, plus `fp_poly` solder-mask-relief
  shapes and `fp_line` Edge.Cuts geometry defining a keyed insertion-tab
  notch) from KiCad 5's `(module ...)` syntax to KiCad 10's
  `(footprint ...)` syntax, saved to
  `PC1500-RP2350B-BLE\Sharp-PC-Footprints.pretty\SHARP-PC-CARTRIDGE.kicad_mod`,
  registered in a new project-level `fp-lib-table` (didn't exist
  before), J3's schematic footprint field left as
  `Sharp-PC-Footprints:SHARP-PC-CARTRIDGE` (the original placeholder
  name turned out to already be correct once a real footprint backed
  it -- no rename needed after all). Verified via `kicad-cli sch erc`:
  J3 resolves cleanly (only L2's separate, unrelated `Inductor_SMD` name
  mismatch remains in `footprint_link_issues`). Pad numbers 1-40 match
  the connector's real pin numbers directly, which already match this
  schematic's J3 pin numbers (per the authoritative manual-sourced
  pinout table above) -- no pin-remapping needed.
  **Real board dimensions given directly by the user 2026-08-25**
  (superseding the reference board's own smaller size, confirmed via an
  AskUserQuestion preview sketch before drawing anything -- a wrong
  mechanical outline is a costly hardware mistake, worth the extra
  confirmation step): tab is 26.3mm wide x 7mm deep; the rest of the
  board is 31.8mm wide x 43mm long, sharing a centerline with the tab.
  Footprint's own tab outline was redrawn to 26.3 x **7.1**mm (0.1mm
  added so it fully contains the real pad geometry, which reaches
  exactly to that edge -- correct for an edge connector, contacts are
  supposed to be flush with the board edge). Dropped the reference's own
  `fp_poly` solder-mask-relief shapes and `fp_text user` dimension notes
  (not recreated -- individual per-pad mask openings are sufficient;
  revisit only if manufacturing review flags soldermask slivers between
  the 1.27mm-pitch pads). The wider 31.8x43mm body is drawn separately as
  PCB-level `gr_line` Edge.Cuts in `PC1500-RP2350B-BLE.kicad_pcb` (not
  baked into the footprint, since that part is this board's own custom
  shape) -- **J3 must be placed at absolute (150, 50), rotation
  270deg** for its own embedded tab outline to close the body outline
  exactly (`kicad-cli pcb drc` confirms 0 `invalid_outline` violations
  at that placement; documented in a `gr_text` note on the PCB's own
  Cmts.User layer too). J3 was deliberately NOT pre-placed in the file
  itself (a manually-placed footprint's UUID likely won't match what
  "Update PCB from Schematic" expects, risking a duplicate/orphan) --
  place it via those exact coordinates once that command has run.
  **Gotcha, confirmed empirically while getting this placement right:
  KiCad footprint rotation (`at X Y ANGLE`) is CLOCKWISE for positive
  angles, not the standard CCW math convention.** First attempt used
  90deg reasoning as CCW and got a ~9mm displacement in the wrong
  direction -- caught by rendering an SVG and reading actual coordinates
  back out via JS, not by re-deriving the formula again. 270deg
  (equivalently -90deg) was correct. Don't re-derive this by hand next
  time; remember rotation is clockwise if placing any other footprint by
  direct file edit.
  **Component-side placement constraints given by the user, not yet
  acted on:** pins 1-20 of J3 (F.Cu) = top side of the board, by project
  convention. SD card slot and the RN4871 BLE module both go on top; the
  BLE module's antenna end points toward the back edge (farthest from
  J3). Everything else can go on either side, except avoid placing
  components underneath the BLE antenna on the bottom side.
