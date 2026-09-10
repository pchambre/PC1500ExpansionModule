# Dropping the FXMA108 level shifters on PC1500-RP2350B-BLE — investigation notes

Status: **open investigation, not yet decided**. Nothing on the board has
been changed for this. This file exists to capture the research so it
isn't lost, and to give Piers/Reddit/whoever a concrete writeup of the
question being asked.

## The question

The data bus (U7), and the two address-bus level shifters (U11/U12), are
all FXMA108 parts translating the PC-1500's ~4.7-5V (`VCC`) logic to the
RP2350's 3.3V (`IOVDD`) domain. That's 3 chips and roughly 30
connections/traces. If they can be safely removed (tying the PC-1500 side
directly to RP2350 GPIOs), the board simplifies enough that 4-layer
routing might become viable again.

The LH5801 (PC-1500 CPU) almost certainly can't source more than a small
fraction of a mA on its bus outputs — nowhere near enough to force a
low-impedance node. The question is whether that matters, given the
RP2350's own 5V-tolerance spec is written as a voltage limit, not a
current limit.

## What the RP2350 datasheet actually says

Table 1433 ("Absolute maximum ratings"), `V_PIN_FT`:

| IOVDD | Max pin voltage |
|---|---|
| 3.3V | 5.5V |
| 2.5V | 4.2V |
| 1.8V | 3.63V |
| **0V (unpowered)** | **3.63V** |

Section 14.8.2.1 ("Pin types"), verbatim:

> "Fault Tolerant Digital... these pins are described as Fault Tolerant,
> which in this case means that **very little current flows into the pin
> whilst it is below 3.63V and IOVDD is 0V**. Additionally, they will
> tolerate voltages up to 5.5V, provided IOVDD is powered to 3.3V."

Key point: the "very little current flows" guarantee is *scoped to below
3.63V*. Above that, unpowered, Raspberry Pi doesn't characterize or
publish a current-vs-damage curve at all — it's simply stated as exceeding
the absolute maximum rating. **No public source (datasheet, forums, or
community bug reports — see below) publishes an actual current threshold
for this condition.** That's a real gap, not something we failed to find.

## A2 vs A4 stepping (as asked about directly)

No functional GPIO circuit difference — same pad design on both. What
changed with A4 is that Raspberry Pi ran a full qualification campaign and
officially blessed the 5V-tolerance claim (retroactively covering A2/A3
too, since the silicon is identical). Current datasheet wording: **"5V
tolerant (powered), and 3.3V-failsafe (unpowered)."** An RP2350 forum
thread has an RPi engineer (jamesh) noting an earlier, more permissive
datasheet draft "was inadvertently left in at release so we removed it
asap" — plausible root of some community confusion about how tolerant
these pins really are unpowered.

Also from that same forum thread, asked directly whether there's any way
around needing `IOVDD` powered before 5V is applied: **"no, I can't see
any way round this without additional hardware"** (jamesh, RPi).

## Community evidence, weighed on its merits

**For the "it's probably fine within our exposure" side:** the *One ROM*
project (piers.rocks) runs RP2350 GPIOs 0-23 directly on 5V retro buses
(6502/VIC-II class systems) with no level shifting, and reports first
PCB revision "just worked." Their own measurements: ~15ms from `IOVDD`
present to the RP2350's own code starting to run. The host bus is
electrically active the whole time `IOVDD` is ramping/the chip is in
reset, and it hasn't reportedly killed chips in that project.

**Against:** a contributor on the gusmanb/logicanalyzer GitHub discussion
(#301) claims to have killed an RP2354B this way ("I damaged one of my
RP2354Bs without a level shifter"). **This claim has zero circuit
detail** — no schematic, no statement of what was on the other end of the
pin (a bus signal vs. a low-impedance rail), no series resistor
information, and the poster's own words ("I guess I messed up... I'll
have to look into this a little more") read as an unconfirmed hypothesis,
not a diagnosed failure. The GitHub issue he links as
detail (#308) is unrelated (a stepping/overclocking question). A second
citation in the same thread, offered as evidence of "relatively great
risk" (issue #142), turned out to be about the unrelated E9 **latch-up**
erratum from 2024, not this failure mode at all.

**Bottom line on public evidence:** real people believe they've killed
RP2350/2354 chips this way, but nobody has published a characterized
failure (source impedance, available current, exposure duration). It's
folklore-grade evidence in both directions — enough to take the risk
seriously, not enough to quantify it.

## This board's specific power sequencing (the part that actually matters)

This is a plug-in card; the question isn't "is 5V tolerance real" in the
abstract, it's "on *this* board, is there ever a window where the PC-1500
bus is live and U1's `IOVDD` isn't."

- U1's 3.3V (`IOVDD`) comes from `U4` (AP2112K-3.3 LDO). `U4` pin 3
  (`EN`) ties **directly to `VCC`** — the same rail that powers the
  LH5801. No supervisor, no RC delay.
- `U4` pin 1 (`VIN`) ties to `VGG`, the board's always-on rail (also
  keeps SRAM alive). So `VIN` is already stable and present *before*
  `VCC`/`EN` ever rises — exactly the condition the AP2112 datasheet's
  startup-time spec is measured under.
- AP2112 datasheet (Diodes Inc., DS39724 Rev 2-2), electrical
  characteristics table, `t_S` (Start-up Time, no load): **20µs
  typical.**
- LH5801 reset hold, per the PC-1500 technical reference manual: **≥2ms**
  minimum before the CPU drives the bus.

That's roughly a 100x margin between "IOVDD should be valid" (tens of
µs after `VCC` rises) and "LH5801 might start driving the bus" (≥2ms
after `VCC` rises), using the regulator's typical (not worst-case) spec.
The power-down direction (does `IOVDD` droop before or after the LH5801
stops driving the bus as `VCC` collapses) hasn't been analyzed yet and
is the same class of race in reverse.

## Where this stands

Genuinely promising, not yet proven. "Typical" isn't "guaranteed," and
none of the above has been confirmed on the bench. Next steps before
acting on this:

1. Scope `VCC`, `IOVDD`, and a bus line together across several real
   power-on cycles (including a deliberately slow `VCC` rise, e.g. weak
   batteries) to get the actual margin on this board, not the datasheet's
   typical number.
2. Do the same for power-down.
3. Fold in whatever comes back from Piers/Reddit outreach — real
   fielded-unit experience (positive or negative) is worth more than any
   datasheet table for this specific failure mode.
4. If the margin holds up, plan the actual removal: U7/U11/U12 and their
   ~30 associated nets/traces come out, PC-1500-side data/address nets
   wire straight to the corresponding U1 GPIOs.

## Sources

- [RP2350 Datasheet (Raspberry Pi)](https://pip-assets.raspberrypi.com/categories/1214-rp2350/documents/RP-008373-DS-2-rp2350-datasheet.pdf) — §14.8.2.1 Pin types, §14.9.1 Absolute maximum ratings
- [RP2350 5V tolerance clarification — Raspberry Pi Forums](https://forums.raspberrypi.com/viewtopic.php?t=375118)
- [Raspberry Pi: RP2350 A4, RP2354, and a new Hacking Challenge](https://www.raspberrypi.com/news/rp2350-a4-rp2354-and-a-new-hacking-challenge/)
- [Pico 2/RP235x: A2 vs A4 stepping differences — PiShop.us](https://support.pishop.us/article/170-pico-2-differences-steppings)
- [Retro ROMming with RP2350 5V Tolerant GPIOs — piers.rocks](https://piers.rocks/2025/08/25/rp2350-5v-gpios.html)
- [gusmanb/logicanalyzer discussion #301](https://github.com/gusmanb/logicanalyzer/discussions/301) (ErichZimmer damage claim, unconfirmed root cause)
- [gusmanb/logicanalyzer issue #308](https://github.com/gusmanb/logicanalyzer/issues/308) (referenced as detail, actually unrelated)
- [gusmanb/logicanalyzer issue #142](https://github.com/gusmanb/logicanalyzer/issues/142) (E9 latch-up erratum, unrelated failure mode)
- AP2112 datasheet, Diodes Incorporated, document DS39724 Rev. 2-2, June 2017 — `t_S` Start-up Time spec, page 4
