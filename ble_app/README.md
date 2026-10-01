# pc1500_ble

The laptop (and later phone) side of the PC-1500 expansion board's BLE link. It
advertises the "PC-1500 Link" GATT service, and a PC-1500 scans for it and
connects.

What it does for a connected PC-1500 (`RP2350/BLE_PROTOCOL.md`):
- `BLPRINT`/`BLLIST` text appears in the console; `BLCLS` clears it.
- `BLSAVE`/`BLLOAD` files live in `Documents\PC1500-BLE`.
- The plotter (2026-09-30): what the PC-1500's CE-150 commands (`LPRINT`,
  `LLIST`, `LINE`, `RLINE`, `TEST`, ...) draw, on a 58 mm paper roll at its real
  proportions, in the CE-150's four pens (`lib/plot.dart`). The expansion board
  sends the pen's movements (PLOT frames), so the app only draws lines.

The log is also written to `%TEMP%\pc1500_ble.log`. Tests: `flutter test`.

- Link service `c31f0001-92a3-40ab-b63d-7cdb0a37aed0`
  - RX `c31f0002-…`: written by the connecting side (write without response)
  - TX `c31f0003-…`: notify
- Windows advertises under the computer's name, not the requested local name.
  Peers find servers by the service UUID.
- `third_party/ble_peripheral` is a patched copy of `ble_peripheral` 2.4.0,
  because the published Windows code crashes on writes. See its `PATCHES.md`.

Build: `flutter build windows` (needs Developer Mode, and Visual Studio's
C++ desktop workload in a complete install).
