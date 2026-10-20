# Local patches to ble_peripheral 2.4.0

This is a copy of `ble_peripheral` 2.4.0 from pub.dev
(https://github.com/rohitsangwan01/ble_peripheral), used by `../../pubspec.yaml`
as a path dependency because the published Windows code crashes.

## Windows: coroutine parameters taken by reference (2026-09-27)

`AddServiceAsync`, `SubscribedClientsChanged`, `ReadRequestedAsync` and
`WriteRequestedAsync` are `winrt::fire_and_forget` coroutines that took their
first parameter by reference. After the first `co_await` that reference
dangles, and using it crashed the app with an access violation in
`ble_peripheral_plugin.dll` as soon as a central wrote to a characteristic
(upstream issue #26; the unmerged PR #27 fixed only the write path by copying
the UUID early). Here all four take the parameter by value, which copies the
reference-counted WinRT object / the BleService, so it stays valid across
`co_await`. Changed: `windows/ble_peripheral_plugin.h`, `.cpp`.

## Windows: responding to a write without response (2026-09-27)

`WriteRequestedAsync` called `request.Respond()` for every write. For a
write *without* response (what the PC-1500 sends, BLE_PROTOCOL.md) Windows
has nothing to respond to and throws; the exception left a callback posted
to the UI thread and ended the app (fail-fast 0xC0000409, in the same second
as the PC-1500's first write). It now responds only to
`GattWriteOption::WriteWithResponse`, and nothing in that callback can throw
out of it. Changed: `windows/ble_peripheral_plugin.cpp`.

## Windows: undelivered notifications logged (2026-09-27)

`UpdateCharacteristic` fires `NotifyValueAsync` and forgets it, so the Dart
side can't tell whether a notification was ever sent. A notification Windows
didn't deliver (an operation that failed, a client it couldn't reach, or no
client subscribed) is appended to `%TEMP%\pc1500_ble_native.log`. While the
PC-1500 link was being brought up, logging every result showed Windows
holding notifications (Unreachable, 0 bytes) until the PC-1500 had a GATT
server of its own.

## Android: advertisement too large (2026-10-20)

`startAdvertising` put the device name in both the advertisement and the
scan response. Flags (3) + the 128-bit Link service UUID (18) + "PC1500-SRV"
(12) is 33 bytes, over the 31-byte legacy advertisement, so Android refused
it (ADVERTISE_FAILED_DATA_TOO_LARGE, "data too large"). The name now goes in
the scan response only, and only if it fits there. The plugin also renamed
the phone's Bluetooth adapter to `localName`, which renames the phone for
good; it no longer does, so the phone advertises under its own name, as
Windows does. Changed: `android/src/main/kotlin/.../BlePeripheralPlugin.kt`.

## Android: nothing reached the central (2026-10-20)

The PC-1500 connected and sent HELLO, but never got the app's HELLO back
("Our HELLO was refused (7)"). Three things, all in `BlePeripheralPlugin.kt`:
- On connect the plugin called `createBond()` on a central that wasn't
  bonded, and added it to the connected devices only once bonded. The
  PC-1500 pairs at the link level and never bonds, so `updateCharacteristic`
  had no device to send to. Centrals are now added on connect, unbonded.
- Every send was an indication (`notifyCharacteristicChanged(..., true)`),
  for a characteristic that only notifies. Indications are now used only for
  a characteristic without the notify property.
- Android takes one notification at a time and refuses the next until
  `onNotificationSent`; the app's ACK and its own HELLO go out back to back,
  so the HELLO was lost. Notifications are now queued and sent in order, each
  with its own copy of the value (API 33+'s value-taking call).
