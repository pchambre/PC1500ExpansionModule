# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.1)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.1)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
# Copyright (c) 2026 Paul Chambre. Licensed under the Apache License,
# Version 2.0 -- see LICENSE.

cmake_minimum_required(VERSION 3.13)

# Pico 2 W specifically (RP2350 + CYW43439 WiFi/BT) -- must be set before
# pico_sdk_import.cmake is included, matching every official pico-examples
# CMakeLists.txt that targets a *_w board. This project doesn't use WiFi/BT
# itself; pico2_w is just the actual board on the breadboard.
set(PICO_BOARD pico2_w CACHE STRING "Board type")

include(pico_sdk_import.cmake)

project(greenpak_tester C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(greenpak_tester
    main.c
    greenpak_pins.c
    greenpak_gpio.c
    greenpak_i2c.c
    greenpak_virtual_io.c
    selftest.c
)

pico_set_program_name(greenpak_tester "greenpak_tester")
pico_set_program_version(greenpak_tester "0.1.0")

# USB stdio, not UART -- matches RP2350B's convention. printf output is
# read back over the USB CDC serial port (e.g. via the Pico VS Code
# extension's serial monitor).
pico_enable_stdio_usb(greenpak_tester 1)
pico_enable_stdio_uart(greenpak_tester 0)

# GP0/GP1 land on a real matched RP2350 hardware I2C0 SDA/SCL pair here,
# so this uses the hardware_i2c peripheral directly -- unlike RP2350B's
# greenpak_i2c.c, which bit-bangs because its GreenPAK link didn't land
# on a matched pair.
target_link_libraries(greenpak_tester
    pico_stdlib
    hardware_gpio
    hardware_i2c
)

pico_add_extra_outputs(greenpak_tester)
