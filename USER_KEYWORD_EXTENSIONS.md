# User keyword extensions: design ideas

Status: ideas only (2026-09-25), nothing implemented. Written so the work can be picked up later.

Goal: someone who has an LH5801 routine and a keyword name for it (already checked for conflicts)
can add it to the expansion ROM without learning its internals.

## What makes it hard today

- **The keyword table is edited by hand** (`rom/rom.asm`), and its layout rules were found the hard
  way (see `Documents/PC1500/PC1500_BASIC_Keyword_Extension_Mechanism.md` §1-§4):
  - one first-letter index;
  - every marker that isn't first for its letter needs bit 4 clear;
  - a name that is a prefix of another must come after the longer one (SDRMDIR before SDRM);
  - a terminator right after the last entry of each letter chain;
  - a unique E1xx code per keyword.
- **The calling convention isn't obvious:** on entry, Y points just past the keyword's token (at
  its arguments); the routine ends with `VEJ E2` with Y at the end of its statement, not `RTN`.
  That form works both at the prompt and inside a running program (§12 of the same doc).
- **Rebuilding means two toolchains:** the ROM is assembled with the sdcc-pc1500 toolchain, and its
  image is compiled into the RP2350 firmware (`rom_image.h`), which then has to be rebuilt and
  reflashed with the Pico SDK.

## Tier 1: build-time extensions (for people with the toolchain)

A drop-in folder, `rom/extensions/`. A user adds `mycmd.asm` and a manifest entry:

```
keyword  LINEPLOT
code     E1A3        ; optional -- assigned automatically otherwise, then kept
entry    LINEPLOT_ENTRY
```

A generator script run by `build.ps1`:

- builds the first-letter index and the keyword table, with ordering, markers and terminators
  handled automatically;
- checks for clashes:
  - a name that is a prefix of an existing keyword, or has one as its prefix;
  - a name containing a BASIC keyword, which BASIC tokenizes (FORMAT contains FOR);
- assembles the ROM and produces the firmware image as today.

Codes must never change once assigned: saved BASIC programs store the E1xx code, so a renumbered
keyword breaks every program that uses it. User codes would come from E1A0 upward, clear of the
built-in ones (E180-E19A).

### A stable API for user code

A fixed jump table at the top of the ROM (for example 0x9F00) that user code can `SJP` into, and
that never moves between releases even as the rest of the ROM does:

- `EC_WAKE` / `EC_DONE` (wake the MCU / let it sleep again);
- evaluate a BASIC expression into a value;
- raise a BASIC error;
- show a line and wait for a key; blank the line; browse a listing;
- a small SD file API: open, read block, write block, close;
- `KEYWORD_RETURN`.

A routine that doesn't need any of these just ends with `VEJ E2`. A template `.asm` would show the
entry and exit convention.

Estimated effort: about a day, mostly the generator and its clash checks.

## Tier 2: no toolchain, no firmware rebuild (extension packs on the SD card)

The ROM region is RAM inside the MCU, so the firmware can assemble the final ROM image itself at
boot:

- **Packs on the SD card:** the firmware reads `/EXT/*.EXT`. A pack holds keyword names, codes,
  entry offsets, the code bytes and a relocation list.
- **Loading:** the firmware places each pack's code into the free ROM space (about 4.7K as of
  2026-09-25) and generates the keyword table and index itself, so the table rules live in C and
  users never see them.
- **Making a pack:** the user assembles the routine with any LH5801 assembler, then runs a small
  pack tool (or a web page) that wraps it.
- **Relocation:** LH5801 code uses absolute jumps, so each pack either targets a fixed slot address
  or carries a list of offsets for the loader to patch as it places the code. The relocation list
  is friendlier (several packs can coexist); sdas's relocatable output can supply it.
- **Managing packs:** an `EXT` keyword to list loaded packs, plus `EXT LOAD` / `EXT OFF`.
- **Program compatibility:** keyword codes still have to stay fixed per keyword, as in Tier 1.
- **STAGE RAM needs one change.** Staging itself already works: it copies whatever is in the ROM
  region, and its checksum is computed from the same image. But at boot the firmware trusts an
  already-staged SRAM copy (`monitor_init_greenpak()`). With packs, the image can change between
  boots, so the firmware must store a hash of the image it staged and re-stage on a mismatch.
  Otherwise it keeps running a stale ROM.

Estimated effort: a few days — the pack format, loader, relocation, table generation in C, the
`EXT` keyword and the pack tool. Tier 2 can reuse Tier 1's API table unchanged.

## Limits of either approach

- **One ROM page:** everything shares the 8800-9FFF PV-low page, with about 4.7K free today and at
  most 256 codes (E100-E1FF), of which the built-ins use E180-E19A.
- **Standalone routines only:** user keywords are LH5801 routines. Anything that needs MCU-side code
  (BLE, Wi-Fi, SD work beyond the API) needs C in the firmware; a later step could add an MCU plugin
  hook, or a generic "run this MCU command" helper.
- **Program use depends on the user's code:** a keyword works inside programs only if the routine
  reads its arguments at Y and ends with `VEJ E2`.

## Suggested order

Build Tier 1 first; it's cheap and fits the current workflow. Design its API table so Tier 2 can
reuse it as is. Tier 2 is what turns this into a feature for users without a toolchain.
