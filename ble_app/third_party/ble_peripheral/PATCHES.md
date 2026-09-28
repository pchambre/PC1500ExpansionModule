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

## Windows: notification results logged (2026-09-27, temporary diagnostic)

`UpdateCharacteristic` fires `NotifyValueAsync` and forgets it, so the Dart
side can't tell whether a notification was ever sent. While bringing up the
PC-1500 link it appends each result (clients, status, bytes sent) to
`%TEMP%\pc1500_ble_native.log`; that showed Windows holding notifications
(Unreachable, 0 bytes) until the PC-1500 had a GATT server of its own.
To be reduced to failures only.
