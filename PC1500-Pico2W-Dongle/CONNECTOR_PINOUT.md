# Hirose HIF6A-60PA-1.27DS-71 pinout (Sharp PC-1500/1600 60-pin connector)

Extracted directly from the `Sharp_PC_Connector:HIF6A-60PA-1.27DS` footprint's
pad net names in Kai Bader's `Sharp-PC-Breakout-Boards` repo
(`breakout-board.kicad_pcb`), not re-derived — see
`C:\Users\paulc\Downloads\Sharp-PC-Breakout-Boards-main\Sharp-PC-Breakout-Boards-main\breakout-board.kicad_pcb`.

| Pin | Signal | Pin | Signal | Pin | Signal |
|-----|--------|-----|--------|-----|--------|
| 1 | AD7 | 21 | D3 | 41 | +4V7 |
| 2 | AD6 | 22 | D2 | 42 | +4V7 |
| 3 | AD5 | 23 | D1 | 43 | FGND |
| 4 | AD4 | 24 | D0 | 44 | FGND |
| 5 | AD3 | 25 | ~INHIBIT | 45 | +BATT |
| 6 | AD2 | 26 | ~WEX | 46 | +BATT |
| 7 | AD1 | 27 | ~CMTIN | 47 | +BATT |
| 8 | AD0 | 28 | W1 | 48 | +BATT |
| 9 | PB0 | 29 | CMTOUT | 49 | NC4 |
| 10 | PC7 | 30 | INT | 50 | BFO |
| 11 | +4V7 | 31 | AD8 | 51 | \u03a6OS |
| 12 | +4V7 | 32 | AD9 | 52 | GND |
| 13 | NC1 | 33 | AD10 | 53 | GND |
| 14 | NC2 | 34 | AD11 | 54 | GND |
| 15 | PV | 35 | AD12 | 55 | GND |
| 16 | PU | 36 | AD13 | 56 | DME0 |
| 17 | D7 | 37 | AD14 | 57 | RW |
| 18 | D6 | 38 | AD15 | 58 | DME1 |
| 19 | D5 | 39 | PB1 | 59 | ME1 |
| 20 | D4 | 40 | NC3 | 60 | OD |

Notes:
- `+BATT` (pins 45-48, 4 pins ganged) is the raw, unregulated battery
  supply this board's own 4.7V/600mA regulator feeds from.
- `+4V7` (pins 11/12/41/42) is the PC-1500's own internally-regulated
  4.7V rail (Vgg on the internal RP2350B board's own connector) --
  **not** the same node as this dongle's own locally-regulated `Vgg`,
  though both happen to be ~4.7V. Don't tie them together without
  checking whether the real PC-1500 side can source/sink current from an
  external board driving its own +4V7 pins.
- `FGND` (pins 43/44) vs `GND` (pins 52-55) are kept as separate nets on
  the original board -- confirmed distinct in the source footprint, not
  a naming accident. Don't merge them without understanding why the
  original design kept them apart.
- `~INHIBIT`, `~WEX`, `~CMTIN` -- tilde prefix denotes active-low in the
  source project's own convention.
- Only 13 of the 16 AD lines (AD0-AD12) are needed for this board's own
  8K address-decode window per the approved plan; AD13-AD15 still route
  through the GreenPAKs for SRAM addressing as on the internal board.
