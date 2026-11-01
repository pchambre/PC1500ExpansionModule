# Design decisions

Rationale behind non-obvious hardware/firmware choices across this repo,
kept at repo root because it's referenced from multiple sub-projects
(`GreenPak_Provision/main_provision.c`, `RP2350B/main_provision.c`, and
others). Both dongle and internal-card boards have **running fabricated
prototype hardware** as of this writing -- entries here are confirmed,
working design intent from that hardware, captured during revision work
for the next fabrication round, not theoretical/undecided.

This file replaces an earlier version of itself that existed at some
point (referenced by the "repo-level plan doc" comments in both copies
of `main_provision.c`) but was never committed to git and has been lost.
Committing this one for real this time so that doesn't happen again.

## Why 4.7V is the PC-1500's native logic rail (Vcc/Vgg)

Sharp's own choice for this CMOS-based pocket computer, not something
either expansion board invented:

- **Easier to regulate down from a 4x1.5V (6V) alkaline/NiCd battery
  pack** than 5V -- less regulator dropout margin needed.
- **Lower power than 5V**, which matters for battery life on CMOS logic
  (power scales with V²).
- **Still close enough to 5V for TTL-style interfacing** -- the PC-1500
  Technical Reference Manual specifies the bus's minimum logic-high
  (`V_IH`) as a fixed **2.4V**, not a percentage of the bus's own supply
  (CMOS-style), so 4.7V clears standard TTL thresholds comfortably. This
  fixed-threshold fact is also what makes direct (un-level-shifted) GPIO
  connection to a 3.3V MCU work -- see
  `PC1500-RP2350B-BLE/no_level_shifters_investigation.md` for the full
  GPIO-tolerance/timing analysis that rests on it.

**Vcc vs Vgg, on the PC-1500 itself:** both are nominally ~4.7V, but
functionally different rails -- `Vcc` is the switched "On" power (dies
when the calculator powers off), `Vgg` is the **always-on** 4.7V rail
that keeps things like static RAM powered through power-off.

**On the expansion boards:** each board has its own onboard regulator
(`U7`, AP1117-ADJ, feedback-set via `R1`/`R2`) deliberately configured to
output **4.7V**, named `VGG` on these boards -- chosen specifically to
match the PC-1500's own native voltage domain, not an arbitrary pick.
This is *not* the same node as the PC-1500's own `+4V7` connector pins
(the PC-1500's own internal Vgg) -- see `CONNECTOR_PINOUT.md`'s note on
this; don't tie them together without checking whether the PC-1500 side
can source/sink current from an external board driving its own +4V7
pins.

## GreenPAK dual-chip provisioning (both boards: GP1/GP2)

Each board carries two Renesas SLG46826G GreenPAKs (GP1, GP2). Brand-new,
unprogrammed GreenPAKs all ship at the **same factory-default I2C slave
address (0x08)**, so both chips can't coexist live on one bus until at
least one has been reassigned a unique address. `GreenPak_Provision`
(standalone Pico 2 W firmware, separate image from normal board
operation) handles this:

- **Target addresses once provisioned:** GP1 -> `0x18`, GP2 -> `0x10`.
- On the **dongle board**, GP2's isolation from the shared bus is a
  physical jumper (SDA1/SCL1 -- see the dongle-specific section below)
  that must be disconnected for GP1-alone provisioning and reconnected
  for GP2's. The **internal card** does the same with two jumpers on
  J7 (see the card-specific section below). Its target addresses differ:
  GP1 -> `0x20`, GP2 -> `0x28` (control codes 4 and 5;
  `RP2350/greenpak_i2c.h`).
- **Every provisioning run checks target addresses first, not just the
  factory default** -- this is what makes a re-run idempotent/safe: any
  chip already answering at its own target address gets read back and
  compared against the expected image, and is only actually
  reprogrammed if the content doesn't already match (NVM write endurance
  is finite; don't re-erase/re-write a chip that's already correct just
  because the tool ran again, e.g. during repeated power-cycling in
  bring-up).
- Fallback logic handles all four real states (both provisioned already;
  neither provisioned yet; GP1 done/GP2 pending; GP2 done/GP1 pending)
  plus a recovery mode (full 7-bit address scan) if nothing answers at
  any known address. Recovery mode never writes unless built with
  `-DGREENPAK_NVM_ALLOW_RECOVERY_WRITE=1` -- a deliberately different
  build, not a runtime prompt.
- All real NVM writes are gated by `GREENPAK_NVM_DRY_RUN` (default 1).
- Full state-machine detail lives in `GreenPak_Provision/main_provision.c`'s
  own header comment -- this section is the "why," that file is the
  "exactly how."

### Dongle-specific: SDA1/SCL1 jumpers exist for this reason

On `PC1500-Pico2W-Dongle`, `U3`=GP1, `U4`=GP2, bridged via `U8`
(TCA9406 level shifter) to the MCU. `SDA1`/`SCL1` (formerly `JP1`/`JP2`)
jumper `SDA_GP`/`SCL_GP` (GP1's segment, always live) across to
`SDA_GP_U4`/`SCL_GP_U4` (GP2's segment, isolated when open):

1. **Jumpers open** -- GP2 electrically isolated. MCU programs GP1 alone
   at the factory default, reassigns it `0x18`.
2. **Jumpers closed** -- GP2 joins the bus. No collision (GP1's moved
   off default). MCU programs GP2, reassigns it `0x10`.
3. Jumpers **stay closed permanently** after this one-time step for
   normal operation. Always the same on-board MCU on both sides of this
   sequence -- never an external programmer.

**Pull-up placement consequence:** `R11`/`R12` (2K, added for I2C
fast-mode -- see below) sit on `VGG` -> `SDA_GP`/`SCL_GP` only. This
covers GP1 unconditionally and GP2 once the jumpers bridge the segments
together; GP2's isolated segment doesn't need its own pull-up since
nothing communicates with it while isolated.

### Internal card: J7's SDA/SCL jumpers exist for the same reason

On `PC1500-RP2354B-BLE-more-RAM`, `U3`=GP1, `U9`=GP2, bridged via `U10`
(TCA9406) to the MCU (GPIO28/29). GP1 sits on `SDA_GP`/`SCL_GP`, always
live; GP2's own `SDA_GP_U9`/`SCL_GP_U9` reach only header `J7`
(`SWD_I2C_HEADER`), whose pins pair them up for two jumpers:

- J7 pin 1 `SCL_GP` -- pin 2 `SCL_GP_U9`
- J7 pin 3 `SDA_GP` -- pin 4 `SDA_GP_U9`

Every new GreenPAK answers at the factory default, so GP2 must stay off
the bus until GP1 has been reprogrammed to its own address: jumpers off,
program GP1; jumpers on, program GP2; jumpers stay on from then on. This is
deliberate -- not a missing connection -- even though it leaves GP2's I2C
pins on a net with no other part. `R20`/`R21` (2K to `VGG`) pull up GP1's
side only, which covers GP2 once the jumpers are on (as on the dongle).

Found 2026-10-18: the `SCL_GP`/`SDA_GP` labels drawn for J7 pins 1 and 3
sat 2.54 mm short of the pin ends -- dangling (ERC `label_dangling`), so
the jumpers would have joined GP2 to nothing. Check those two before
building the board.

## The 3.3V regulator's enable comparator (internal card)

On `PC1500-RP2354B-BLE-more-RAM`, `U5` (MCP6541 comparator, powered from
`VGG`) compares the card edge's `VCC` (+ input) against `R22`/`R23`'s
divider of `VGG` (- input) and drives `U4`'s (AP2112K-3.3) `EN`. It's there
so a ghost voltage left on the rails with the PC-1500 off can't keep the
3.3V regulator -- and so the MCU -- from shutting off.

Neither `U5` nor `U10` (TCA9406) has its own bypass capacitor, by choice
(2026-10-18): the comparator only gates the regulator's enable, and the
TCA9406 circuit is the one already proven on the dongle. Don't flag these
two in a decoupling review.

## Decoupling on VGG and VREG_AVDD (internal card)

The GreenPAKs (`U3`, `U9`) each get one 100nF cap, at `VDD` (pin 1), and
none at `VDD2` (pin 14), by choice (2026-10-19). The SLG46826 datasheet asks
only for "Capacitor Value at VDD" of at least 0.1uF (Recommended Operating
Conditions) and says nothing about a capacitor on `VDD2`. On this board
`VDD2` is tied to `VDD` by a very short trace. The SRAM (`U2`) gets two
(`C30`, `C32`); `C28` was dropped.

`R29` (33R) with `C27` (4.7uF) filters `VREG_AVDD` from `+3V3`, kept by
choice (2026-10-19). Raspberry Pi's "Hardware design with RP2350" (Power
section) calls `VREG_AVDD` very sensitive to noise and recommends that RC
filter; it draws about 200uA, so `R29` drops about 7mV. The RP2350B dev
board used for prototyping has only 2.2uF to ground and works, but the card
follows the reference design. `FLG5` marks `VREG_AVDD` as driven for ERC
because it's fed only through `R29`.

## VREG_LX inductor (L2) orientation dot (internal card)

`L2`'s footprint (`Sharp-PC-Footprints:L_Abracon_AOTA-B201610`) marks pad 1
with a silkscreen dot, and on these boards pad 1 is `DVDD` (the regulator's
output). That's the side Raspberry Pi marks: in their
`RPI-RP2350A-MINIMAL_R4-S1` KiCad design, L1 (AOTA-B201610S3R3-101-T) has its
dot on pad 1 = `+1V1`, with pad 2 = `VREG_LX`. The RP2350 datasheet agrees
(Figure 28: dot on the "-" end, where the current leaves; Figure 26: the dotted
terminal runs to C_OUT/V_OUT). Checked 2026-10-26. Don't move it to
`VREG_LX`.

The board is being built with an unmarked substitute, Changjiang
FTC252010D3R3MBCA (JLC has no Abracon part), on the existing pads, by choice.
With no marking, orientation can't be controlled. The user's working RP2350B
dev board uses an unmarked inductor too.

## microSD socket: symbol numbering and shell (both boards)

Both boards use KiCad's `Connector:SD_Card_Device` (a full-size SD card
symbol) on the Molex 104031-0811 microSD footprint. KiCad joins them by
number, so the symbol's pin *names* are full-size SD names, while the nets
follow the real microSD pads: pad 1 DAT2 (own pull-up), pad 2 CD/DAT3 = CS,
pad 3 CMD = MOSI, 4 VDD, 5 CLK, 6 VSS, 7 DAT0 = MISO, 8 DAT1; pads 9/10 are
the card-detect switch. This wiring is proven on the dongle (after the
pad 1/2 rework, 2026-09-18). Trust the pad identity, not the symbol's pin
names ("VSS" on SD_MOSI is expected); ERC's power-pin warning on that pin is
the same quirk. Don't "fix" it by swapping symbols without checking every
pad.

The socket's shell (`SH` pads) is meant to be grounded, and it is: GND vias
sit in the `SH` pads. No symbol and footprint pair could be found that
shows the shield's ground connection correctly, so the `SH` pads stay
without a net and DRC will always flag those vias. On the More-RAM card
that's a clearance, a hole-clearance and a solder-mask-bridge error for each
of the three `SH` pads (2026-10-19). These are expected: don't "fix" them by
moving the vias off the shell, and don't count them as new problems in a
DRC review.

The vias are placed by hand after every autoroute (Freerouting can't know
the shell is ground). On the More-RAM card and its TSSOP-GreenPAK fork
(`PC1500-RP2354B-BLE-more-RAM-SOICGP`) the full expected DRC signature is:
`shorting_items` "nets GND and <no net>" (GND via vs J1 pad `SH`),
`hole_clearance` with actual 0 mm or nearly 0 mm, `solder_mask_bridge` and
`clearance` on the same via/pad pairs, and a `via_dangling` for a shell via
that touches no copper other than the shell. The vias sit at about
(155.32, 78.99), (155.58, 78.2) and (161.3, 79.0). Don't report any of these,
and don't suggest giving `SH` a net: it was tried and no symbol/footprint
pair does it. Some `SH` pads may also be given GND on the board by hand, which
shows up as schematic-parity "No corresponding pin found in schematic" for
J1 `SH`. That's expected too, and so is the fact that Update PCB from Schematic
clears it. Don't report it.

## BGM220S unconnected pins (internal card)

Per the BGM220S datasheet (Rev. 1.4, pin definitions): `DECOUPLE` (pin 14)
is internally decoupled and must be left disconnected; `VREG` (pin 21) is
the module's internal regulated supply, not for external circuitry -- also
left open. `ANT_OUT` (pin 40) drives the antenna loop that is part of the
project's `Silabs_BGM220S` footprint (a second copper pad numbered 40), so
in the schematic its net reaches only the module pin. All three are correct
as drawn.

## I2C pull-up values (dongle, added 2026-09-20 revision)

Goal: run I2C fast mode for reasonable throughput over the level-shifted
bridge to the GreenPAKs.

- **2K** on `VGG` (4.7V) -> `SDA_GP`/`SCL_GP` (`R11`/`R12`) -- the
  GreenPAK/level-shifter-B-side segment.
- **1.5K** on `3V3` -> `SDA`/`SCL` (`R13`/`R14`, at `U5`
  SC18IS602B pins 7/8) -- the MCU-side segment (`U1`, `U5`, `U8`'s
  A-side).

The internal card (RP2350B-BLE) may stay on slower I2C, and might split
out separate per-GreenPAK I2C connections instead of sharing one bridged
bus -- under consideration, not yet decided/implemented as of this
writing; revisit before assuming the dongle's jumper-bridge approach
applies there too.

## SRAM bank switching: the LH5801 decides (internal card)

The internal card's extra 64K of SRAM is reached through bus addresses
`0000H`-`3FFFH`. Card GP1 drives the high SRAM address lines (`U3`
IO7/8/9 -> `SRAM_A14`/`A15`/`A16`), so a bus access in that range lands
in the selected bank. Three GP1 virtual inputs select the bank
(`GP1_VI_SRAM_V16`/`V15`/`V14`, `RP2350/greenpak_i2c.h`):

- `000`: the base 16K
- `1bb`: bank `bb`
- `011`: the staging window onto the `C000H` copy

The MCU sets them over I2C. Card GP1's .gp6 is built and passes its bench
vectors and the full sweep (2026-10-16).

The MCU only switches banks; the LH5801 program decides when and which.
A program asks for a bank with a module command, and the MCU never
switches on its own. The reason: the bank is part of the LH5801's memory
map. Whatever runs in `0000H`-`3FFFH`, or keeps data or a stack there,
only works if that range doesn't change under it.

For a program that switches banks, this means its switching code, its
stack and anything it needs across a switch must live outside
`0000H`-`3FFFH`.
