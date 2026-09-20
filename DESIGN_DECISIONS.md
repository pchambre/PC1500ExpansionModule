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
  for GP2's. On the **internal card**, the equivalent isolation is
  whatever this board's own schematic implements (check that board's
  schematic before assuming the dongle's jumper mechanism applies
  identically).
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
