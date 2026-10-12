# BGM220S module firmware (internal card)

This is the BLE module firmware for the PC-1500 internal card (U8, BGM220SC22WGA2). The module handles the radio. The RP2354B keeps the Link protocol and talks to the module over a UART, as described in [../RP2350/MODULE_PROTOCOL.md](../RP2350/MODULE_PROTOCOL.md).

**Status (2026-10-12):** the advertiser side works on the BGM220 Explorer Kit (BRD4314A), and `tools/host_test.py` passes:
- HELLO, SET_NAME, ADV_START, POLL;
- a peer connects;
- `LINK_UP`, `LINK_RX`, `LINK_SEND` and `LINK_DOWN`.

**Still to do:**
- the connector side: SCAN_*, CONNECT, discovery and subscribe;
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
