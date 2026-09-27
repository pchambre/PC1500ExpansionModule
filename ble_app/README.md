# pc1500_ble

The laptop (and later phone) side of the PC-1500 expansion board's BLE link. It
advertises the "PC-1500 Link" GATT service, and a PC-1500 scans for it and
connects.

Status (2026-09-27): step-0 spike. It advertises the Link service, logs every
write to RX and echoes it back on TX. Tested on Windows with nRF Connect as the
central. The log is also written to `%TEMP%\pc1500_ble.log`.

- Link service `c31f0001-92a3-40ab-b63d-7cdb0a37aed0`
  - RX `c31f0002-…`: written by the connecting side (write without response)
  - TX `c31f0003-…`: notify
- Windows advertises under the computer's name, not the requested local name.
  Peers find servers by the service UUID.
- `third_party/ble_peripheral` is a patched copy of `ble_peripheral` 2.4.0,
  because the published Windows code crashes on writes. See its `PATCHES.md`.

Build: `flutter build windows` (needs Developer Mode, and Visual Studio's
C++ desktop workload in a complete install).
