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

# BGM220S provisioner for the internal card (2026-11-16): a separate
# RP2354B image, flashed deliberately like GreenPak_Provision, that programs
# the card's BGM220S (U8) over SWD from GPIO38/39 with the module firmware
# embedded at build time. Flash the card firmware back afterwards.
cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pc1500_card_rp2354b CACHE STRING "Board type")
set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/../RP2350/boards)

include(pico_sdk_import.cmake)

project(bgm220_provision C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# The module image: the card configuration of ../BGM220 (slc generate --with
# BGM220SC22WGA, see ../BGM220/README.md), linked at flash address 0, no
# bootloader. Embedded with .incbin; the build re-runs when the .bin changes.
set(MODULE_IMAGE "C:/Users/paulc/silabs/work/mod_card/cmake_gcc/build/base/pc1500_module.bin"
    CACHE FILEPATH "BGM220S firmware image (.bin, linked at 0x0)")
if(NOT EXISTS ${MODULE_IMAGE})
    message(FATAL_ERROR "MODULE_IMAGE not found: ${MODULE_IMAGE}")
endif()
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/module_image.S
"    .section .rodata.module_image, \"a\"
    .balign 4
    .global module_image, module_image_end
module_image:
    .incbin \"${MODULE_IMAGE}\"
    .balign 4, 0xFF
module_image_end:
")
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/module_image.S
    PROPERTIES OBJECT_DEPENDS ${MODULE_IMAGE})

add_executable(bgm220_provision
    main.c
    swd.c
    ${CMAKE_CURRENT_BINARY_DIR}/module_image.S
)
target_include_directories(bgm220_provision PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../RP2350 # board_pins_card.h
)

pico_set_program_name(bgm220_provision "bgm220_provision")
pico_set_program_version(bgm220_provision "0.1.0")
pico_enable_stdio_usb(bgm220_provision 1)
pico_enable_stdio_uart(bgm220_provision 0)

target_link_libraries(bgm220_provision pico_stdlib hardware_gpio)

# Read-only unless built with -DBGM220_DRY_RUN=0. Even then the module is
# only written when its flash differs from the image.
if(DEFINED BGM220_DRY_RUN)
    target_compile_definitions(bgm220_provision PRIVATE BGM220_DRY_RUN=${BGM220_DRY_RUN})
endif()

# -DBGM220_CHECK_ONLY=1: attach to the running module and read its crystal
# calibration only -- no reset, no halt, nothing written.
if(BGM220_CHECK_ONLY)
    target_compile_definitions(bgm220_provision PRIVATE BGM220_CHECK_ONLY=1)
endif()
# How long the check lets the module run before attaching (ms, default 500).
if(DEFINED BGM220_CHECK_SETTLE_MS)
    target_compile_definitions(bgm220_provision PRIVATE BGM220_CHECK_SETTLE_MS=${BGM220_CHECK_SETTLE_MS})
endif()

pico_add_extra_outputs(bgm220_provision)
