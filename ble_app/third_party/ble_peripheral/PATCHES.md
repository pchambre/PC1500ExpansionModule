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
