# BGM220S module firmware (internal card)

This is the BLE module firmware for the PC-1500 internal card (U8, BGM220SC22WGA2). The module handles the radio. The RP2354B keeps the Link protocol and talks to the module over a UART, as described in [../RP2350/MODULE_PROTOCOL.md](../RP2350/MODULE_PROTOCOL.md).

**Status (2026-10-12):** both Link roles work on the BGM220 Explorer Kit (BRD4314A):
- **The advertiser side:** `tools/host_test.py` passes. HELLO, SET_NAME, ADV_START, POLL; a peer connects; `LINK_UP`, `LINK_RX`, `LINK_SEND` and `LINK_DOWN`.
- **The connector side:** `tools/connector_test.py` passes. SCAN_START finds a Link advertiser by UUID; CONNECT discovers the service and subscribes; `LINK_UP` comes as role 0 with MTU 247; frames go both ways; DISCONNECT. The PC plays the advertiser with Windows' own GATT server, through the winrt packages that bleak installs.

**Still to do:**
- the keyboard (KBD_*);
- SET_TX_POWER;
- testing the wake lines on the card.

## Tools

These are installed by Silicon Labs' `SimplicityInstaller` with the Bluetooth components, and managed by its `slt.exe`:

| | Where |
|---|---|
| Simplicity SDK 2026.6.1 | `%USERPROFILE%\.silabs\slt\installs\conan\p\simpleca33d691c539\p` |
| slc-cli 6.0.25 (`slt install slc-cli`) | `%USERPROFILE%\.silabs\slt\installs\archive\slc-cli-v6.0.25\slc_cli\slc.bat` |
| Arm GCC 14.2 | `%USERPROFILE%\.silabs\slt\installs\conan\p\gcc-a999d2e027337f\p` |
| cmake, ninja | `...\conan\p\cmakefa35ab0687064\p\bin`, `...\conan\p\ninja1a38fc85adcf7\p` |
| Commander | `%USERPROFILE%\.silabs\slt\installs\archive\Simplicity Commander\commander.exe` |

The kit's J-Link needs SEGGER's legacy driver: run `.silabs\slt\installs\archive\segger-legacy-jlink-drivers\InstDrivers.exe` once.

Before the first `slc generate`, run once:
- `slc configuration --sdk <SDK> --gcc-toolchain <GCC>`
- `slc signature trust --sdk <SDK>`

## Build and flash

Generate **outside the repo, in a short path.** CMake gives up on object paths over 250 characters, and generating out of tree also keeps the repo free of generated files.

```
slc generate -p pc1500_module.slcp -d C:\Users\paulc\silabs\work\mod_kit --with brd4314a -o cmake
cd C:\Users\paulc\silabs\work\mod_kit\cmake_gcc
cmake --preset project
cmake --build --preset default_config
commander flash build\base\pc1500_module.s37 --serialno 440198798
```

For the card's module, use `--with bgm220sc22wga2` instead of `--with brd4314a`. That build isn't tested yet.

Generation prints `## ERROR ## Unknown peripheral None` from the pin tool. It's harmless: the UART configuration it generates is correct (USART1 on PA05–PA08, RTS/CTS).

## The Explorer Kit wired to an RP2354B (hand-wired card prototype)

The protocol UART moves to the kit's mikroBUS socket, so the J-Link's USB serial port stays free. Generate with `--configuration PC1500_HOST_MIKROBUS:1` (see `config/sl_iostream_usart_host_config.h`):

| Signal | RP2354B | Module (kit) | mikroBUS |
|---|---|---|---|
| UART MCU → module | GPIO32 | PB02 | RX |
| UART module → MCU | GPIO33 | PB01 | TX |
| CTS ← module RTS | GPIO34 | PC01 | MISO |
| RTS → module CTS | GPIO35 | PC00 | MOSI |
| HOST_WAKE | GPIO37 | PB04 (and LED0, PA04) | PWM |
| MODULE_WAKE | GPIO27 | PB03 | INT |
| GND | GND | GND | GND |

Leave RESETn and SWD unconnected: the kit's J-Link does those. Both boards run at 3.3 V.

**Check it:** type `m` on the card's USB console (card firmware, `module_link.c`). It raises MODULE_WAKE, sends HELLO, POLLs the events and prints them. **Working 2026-10-13:** HELLO answered, the module's boot LOG event collected, and HOST_WAKE went from 1 to 0 as the queue emptied.

## On the Explorer Kit

- **The kit's USB serial port is the module's protocol UART.** VCOM is USART1 on PA05 (TX), PA06 (RX), PA07 (RTS) and PA08 (CTS), the same pins as on the card. So `tools/host_test.py COM3` can play the RP2354B.
- **LED0 is HOST_WAKE (PA04).** It's lit while events are queued.
- **MODULE_WAKE (PB03) is pulled up,** so on the kit a session is always open.

Test (needs `pyserial` and `bleak`; the PC's Bluetooth plays the peer):

```
python tools/host_test.py COM3
```

## Licence

The SDK's example and application code is zlib-licensed. Check Silicon Labs' licence terms for the stack libraries before cards are sold.
