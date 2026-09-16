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
# itself; pico2_w is just the actual board this is flashed to.
set(PICO_BOARD pico2_w CACHE STRING "Board type")

include(pico_sdk_import.cmake)

project(greenpak_provision C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# This is a standalone, VS-Code-extension-friendly build of the same
# GreenPAK NVM provisioner as ../RP2350/main_provision.c (built there as
# the "greenpak_provision" target inside that project's own multi-target
# CMakeLists.txt). ../RP2350 is built/flashed from CLion, not this
# extension, so it doesn't have the Run/Flash/Serial-Monitor buttons this
# project gets for free. The sources here are hand-kept copies, same
# convention as RP2350B's own greenpak_i2c.c -- see main_provision.c's
# top comment for the tool itself, and keep both copies in sync by hand.
add_executable(greenpak_provision
    main_provision.c
    greenpak_i2c.c
    greenpak_nvm.c
)

pico_set_program_name(greenpak_provision "greenpak_provision")
pico_set_program_version(greenpak_provision "0.1.0")

# USB stdio, not UART -- output is read back over the USB CDC serial
# port (e.g. via the Pico VS Code extension's serial monitor).
pico_enable_stdio_usb(greenpak_provision 1)
pico_enable_stdio_uart(greenpak_provision 0)

target_link_libraries(greenpak_provision
    pico_stdlib
    hardware_gpio
)

# Defaults to dry-run (read-only); build with -DGREENPAK_NVM_DRY_RUN=0 to
# actually write, and -DGREENPAK_NVM_ALLOW_RECOVERY_WRITE=1 to allow
# recovery-mode writes -- both deliberately separate, differently-flagged
# builds, not runtime prompts. Set via the CMake Tools "Configure" cache
# variables UI, or re-run CMake with -D on the command line.
if(DEFINED GREENPAK_NVM_DRY_RUN)
    target_compile_definitions(greenpak_provision PRIVATE GREENPAK_NVM_DRY_RUN=${GREENPAK_NVM_DRY_RUN})
endif()
if(DEFINED GREENPAK_NVM_ALLOW_RECOVERY_WRITE)
    target_compile_definitions(greenpak_provision PRIVATE GREENPAK_NVM_ALLOW_RECOVERY_WRITE=${GREENPAK_NVM_ALLOW_RECOVERY_WRITE})
endif()

pico_add_extra_outputs(greenpak_provision)
