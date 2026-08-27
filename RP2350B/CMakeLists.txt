# Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
# Version 2.0 -- see LICENSE.

cmake_minimum_required(VERSION 3.13)

# Bare RP2350B "core dev board" -- a real, stock Pico SDK board definition
# (WeAct Studio's RP2350B Core board: 48-GPIO QFN-80 package, no onboard
# WiFi/BT module, unlike ../RP2350/'s Pico 2 W target). Its board header
# sets PICO_RP2350A=0 (i.e. "this is the B variant") and PICO_PLATFORM=
# rp2350 automatically -- must be set before pico_sdk_import.cmake is
# included, matching every official pico-examples CMakeLists.txt.
set(PICO_BOARD weact_studio_rp2350b_core CACHE STRING "Board type")

include(pico_sdk_import.cmake)

project(pc1500_expansion_rp2350b C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_subdirectory(lib/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico/src)

add_executable(pc1500_expansion_rp2350b
    main.c
    monitor.c
    ble_rn4871.c
    greenpak_i2c.c
    hw_config.c
)

# FatFs itself has no per-value ffconf.h override mechanism -- the whole
# file has to be replaced (see ffconf.h's own top comment). BEFORE puts
# our copy ahead of the submodule's own on the include path so ff.c's
# #include "ffconf.h" resolves to ours.
target_include_directories(pc1500_expansion_rp2350b BEFORE PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)

pico_set_program_name(pc1500_expansion_rp2350b "pc1500_expansion_rp2350b")
pico_set_program_version(pc1500_expansion_rp2350b "0.1.0")

# USB stdio, not UART stdio -- every GPIO here (including the Pico SDK's
# usual UART0 default pins) is already committed to the PC-1500 bus or a
# peripheral (see board_pins.h/main.c's own comment). USB costs no GPIOs.
pico_enable_stdio_usb(pc1500_expansion_rp2350b 1)
pico_enable_stdio_uart(pc1500_expansion_rp2350b 0)

# Unlike ../RP2350/ (Pico 2 W, QMI-CS1 SD trick to save 3 GPIOs), this
# board's full 48-GPIO budget has room for a real hardware SPI1
# peripheral for the SD card -- so the vendored library's own
# no-OS-FatFS-SD-SDIO-SPI-RPi-Pico INTERFACE target (SPI-based SD driver
# included) is used directly, no custom diskio needed. GreenPAK I2C is
# bit-banged (see greenpak_i2c.h for why), so no hardware_i2c link is
# needed either. BLE UART uses hardware_uart via ble_rn4871.c.
target_link_libraries(pc1500_expansion_rp2350b
    pico_stdlib
    pico_multicore
    hardware_gpio
    hardware_uart
    no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
)

pico_add_extra_outputs(pc1500_expansion_rp2350b)
