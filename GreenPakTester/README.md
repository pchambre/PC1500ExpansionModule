# GreenPakTester

Pico 2 W bench-test firmware for a Renesas GreenPAK SLG46826V (GreenPAK6,
20-pin) on a breadboard. Drives/reads the GreenPAK's IO pins to verify
its programmed logic, and talks to it over I2C. This is a standalone
bring-up tool, not part of the PC-1500 expansion board firmware itself,
though the chip under test ("GP1") is the same GreenPAK design used on
the real board (`../../PC1500ExpansionBoard/GreenPak/GP1-SRAM.gp6` --
address-decode glue logic for the expansion SRAM/ROM).

## Wiring (all at 3.3V -- GreenPAK powered from the Pico's own rail, no level shifters needed)

| Signal | Pico GPIO | GreenPAK pin | Function |
|---|---|---|---|
| I2C0 SDA | GP0 | 9 | SDA |
| I2C0 SCL | GP1 | 8 | SCL |
| IO0 | GP2 | 2 | IO0 |
| IO1 | GP3 | 3 | IO1 |
| IO2 | GP4 | 4 | IO2 |
| IO3 | GP5 | 5 | IO3 |
| IO4 | GP6 | 6 | IO4 |
| IO5 | GP7 | 7 | IO5 ("R/W from bus") |
| IO6 | GP10 | 10 | IO6 |
| IO7 | GP12 | 12 | IO7 |
| IO8 | GP13 | 13 | IO8 |
| IO9 | GP15 | 15 | IO9 |
| IO10 | GP16 | 16 | IO10 ("R/W to SRAM") |
| IO11 | GP17 | 17 | IO11 |
| IO12 | GP18 | 18 | IO12 ("SRAM CS") |
| IO13 | GP19 | 19 | IO13 |
| IO14 | GP20 | 20 | IO14 |
| VDD | 3V3 | 1 | power |
| GND | GND | 11 | ground |
| VDD2 | 3V3 | 14 | power |

Encoded in `greenpak_pins.c` -- edit there if the wiring changes.

**Add a real discrete external pull-down resistor on any pin the bench
drives as an input to the GreenPAK (address bus, DME0, PV, R/W-from-bus,
OD, 1500A-sense).** Confirmed on real hardware 2026-09-16: after such a
pin has been actively driven and then released to Hi-Z, the RP2350 lets
it bleed a residual "ghost" voltage back onto the line (measured ~1.9V
resting between periodic 3.3V pulses on IO14/DME0, GPIO20) that the
firmware's own internal weak pull-down (`greenpak_gpio_release_pulled_down()`,
~50-80k ohm) does not fully suppress -- this cost real debugging time
before being traced to the RP2350 rather than to test-vector logic or
breadboard noise. A real external pull-down, well below that internal
value, resolved it. GreenPAK output pins being read (CS, R/W to SRAM,
GP2's read/write triggers) don't need this.

## Building

Same pattern as `../RP2350`:

```
mkdir build && cd build
cmake -G Ninja -DCMAKE_PREFIX_PATH="C:/Users/paulc/.pico-sdk/toolchain/picotool-2.3.0;C:/Users/paulc/.pico-sdk/toolchain/pico-sdk-tools-2.3.0" ..
ninja
```

Requires `PICO_SDK_PATH` set (already configured on this machine, pointing
at `~/.pico-sdk/sdk/2.3.0`). The `CMAKE_PREFIX_PATH` above points CMake at
the pre-installed `picotool`/`pioasm` that ship with the pico-sdk installer
-- without it, CMake tries to build picotool from source using whatever
host `cc`/`c++` it finds first on PATH, which on this machine resolves to
an ancient MinGW GCC 4.7.2 (`C:\Flex Windows\gcc\bin`) too old to compile
picotool's C99/C++11 sources, and the build fails deep in mbedtls/picotool
internals rather than in this project's own code. `../RP2350`/`../RP2350B`
already have this baked into their cached `build/CMakeCache.txt`, which is
why a bare `cmake ..` works for them without the flag once already
configured -- a truly fresh `cmake ..` there would hit the same failure.

Flash `greenpak_tester.uf2` via BOOTSEL mode. View debug output over the
USB CDC serial port (e.g. the Pico VS Code extension's serial monitor).

## Bring-up sequence

Work through these in order rather than trusting the full self-test
immediately -- several steps deliberately come before the GreenPAK is
even wired in, to isolate Pico-side bugs from chip-side ones:

1. Build+flash the firmware as-is and confirm it enumerates as a USB CDC
   device and prints the startup banner. Validates toolchain/board
   id/CMake before anything hardware-specific.
2. **No GreenPAK attached yet:** blink one mapped GPIO (e.g. GP2/IO0)
   and confirm with a multimeter/LED that it toggles -- validates the
   pin table against the physical breadboard wiring.
3. **Still no GreenPAK:** jumper two mapped GPIOs together (e.g.
   GP2<->GP3) and confirm `greenpak_gpio_drive`/`greenpak_gpio_read` see
   each other -- exercises the exact code path the self-test uses, with
   zero risk to the chip.
4. Power up the GreenPAK (VDD/VDD2/GND only, no signal GPIO yet) and
   confirm with a meter it sees a clean 3.3V and draws sane idle current.
5. Wire GP0/GP1 to GreenPAK pins 9/8. `greenpak_i2c_detect_dut()` (see
   `greenpak_i2c.h`) does exactly this bus scan automatically at boot --
   confirmed working against real hardware: GP1-SRAM.gp6's compiled
   control code is `0011`, giving I2C address `0x18`
   (`GREENPAK_GP1_I2C_ADDR`); GP2's design is compiled with `0010`
   (`0x10`, `GREENPAK_GP2_I2C_ADDR`).
6. Wire and test one IO pin (e.g. GP2/IO0) against the live chip before
   wiring the rest.
7. Wire the remaining 14 IO connections, re-checking each pin
   individually (not just in aggregate) to localize any wiring mistakes.
8. Run the placeholder self-test end-to-end with full wiring in place --
   validates harness plumbing; the PASS/FAIL result itself is
   meaningless until real vectors replace the placeholder.
9. Once real GP1 truth-table data is available (GreenPAK Designer
   truth-table export, or decoded LUT data), replace `example_vectors`
   in `selftest.c` with real ones for a first meaningful run.

## Known open items

- GP1's I2C address (`0x18`) is confirmed against real hardware. GP1's
  vectors were re-derived 2026-09-16 after finding DME0 (J3 pin 6) had
  been treated as active-low when it's actually active-high -- the LUT
  programming for both GreenPAKs was corrected, and every DME0-gated
  expected result in `test_vectors`/`test_vectors_gp2` was flipped to
  match (see `selftest.c`'s per-vector comments). Not yet re-run against
  real hardware since the fix -- treat prior "N/N passing" hardware
  confirmations as stale until it is. GP2's address (`0x10`) matches its
  compiled control code but has no real vectors yet -- `test_vectors_gp2`
  in `selftest.c` is a placeholder pending GP2's actual design.
- `selftest.c`'s `example_vectors` is explicitly fake, for exercising the
  harness only -- not real GP1 behavior.
- Pull-up/pull-down needs on individual IO pins during test depend on
  whether GP1's design configures a given output push-pull or
  open-drain, which isn't decoded from the LUT -- left as a per-call
  choice (`greenpak_gpio_release` applies no pull) rather than hardcoded.
- No interactive re-trigger (serial command) or generic NVM read/write
  beyond the register helpers in `greenpak_i2c.h` -- natural follow-on
  once the register protocol/address is confirmed.
