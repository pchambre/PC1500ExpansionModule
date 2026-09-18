cmake_minimum_required(VERSION 3.13)

# Pico 2 W specifically (RP2350 + CYW43439 WiFi/BT) -- must be set before
# pico_sdk_import.cmake is included, matching every official pico-examples
# CMakeLists.txt that targets a *_w board.
set(PICO_BOARD pico2_w CACHE STRING "Board type")

include(pico_sdk_import.cmake)

project(pc1500_expansion_rp2350 C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# FatFs itself has no per-value ffconf.h override mechanism -- the whole
# file has to be replaced (see ffconf.h's own top comment). This project
# provides its own copy; BEFORE puts it ahead of the submodule's copy on
# the include path so ff.c's own #include "ffconf.h" resolves to ours.
set(FATFS_CORE_DIR lib/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico/src/ff15/source)

# lib/qmi_cs1_sdspi/ is NOT used on this board -- it assumes a
# QMI-CS1-driven SD card on a fixed GPIO, which doesn't match
# PC1500-Pico2W-Dongle.kicad_sch. The real SD socket goes through U5 (an
# SC18IS602B I2C-to-SPI bridge chip) sharing the GreenPAKs' own I2C bus
# instead -- see board_pins.h's own SD comment, sc18is602b.h/.c (the
# bridge primitive), and diskio_sd_bridge.c (the FatFs disk_*() driver
# built on it, implementing the standard SD-over-SPI command set).
# qmi_cs1_spi_init() previously hijacked GPIO19, which is really this
# board's D6 data-bus line, breaking the bus outright -- don't add
# lib/qmi_cs1_sdspi/ back here.
add_executable(pc1500_expansion_rp2350
    main.c
    monitor.c
    greenpak_i2c.c
    greenpak_virtual_io.c
    sc18is602b.c
    diskio_sd_bridge.c
    ${FATFS_CORE_DIR}/ff.c
    ${FATFS_CORE_DIR}/ffsystem.c
    ${FATFS_CORE_DIR}/ffunicode.c
)

target_include_directories(pc1500_expansion_rp2350 BEFORE PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}       # our ffconf.h, ahead of the submodule's own
)
target_include_directories(pc1500_expansion_rp2350 PRIVATE
    ${FATFS_CORE_DIR}               # ff.h, diskio.h
    ${CMAKE_CURRENT_LIST_DIR}/../Design01_NonDMA_8K_PV_Swap.cydsn/rom  # rom_image.h -- see monitor.c's own comment for why this isn't hand-copied
)

pico_set_program_name(pc1500_expansion_rp2350 "pc1500_expansion_rp2350")
pico_set_program_version(pc1500_expansion_rp2350 "0.1.0")

# Deliberately NOT using the vendored no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
# library's own add_subdirectory/INTERFACE target: that would also pull
# in its sd_driver/ (hardware_spi-based SD driver), which would collide
# at link time with this project's own diskio_sd_bridge.c -- both define
# disk_initialize()/disk_read()/etc. Only the library's protocol-agnostic
# FatFs core (ff.c/ffsystem.c/ffunicode.c, sourced directly above) is
# used.
# pico_cyw43_arch_none: the onboard LED is wired through the CYW43439,
# not a plain GPIO. Currently used from monitor_run() as a
# read/write-trigger activity indicator for a stripped-down diagnostic
# build of the bus loop (DriveData()/DoCommand() calls commented out --
# see monitor.c) -- not the normal firmware. Bringing up the CYW43439 is
# real SPI-like traffic and real current draw, a plausible noise-coupling
# source in its own right, which is part of what this diagnostic build is
# for: isolating whether that's contributing to the bus interference
# independent of any actual data-bus drive.
#
# pico_multicore -- core0 runs monitor_run()'s bus loop, core1 runs
# monitor_command_worker()'s DoCommand() dispatch (see monitor.c's own
# "WHY TWO CORES" comment: a confirmed data-corruption bug in the
# original single-core design, not a performance tweak). Linking this
# makes pico_cyw43_arch_none's underlying async_context enable its
# cross-core (semaphore + IRQ) support (gated on LIB_PICO_MULTICORE) --
# harmless extra overhead, since cyw43_arch is still only ever called
# from core0 (main.c), never core1.
target_link_libraries(pc1500_expansion_rp2350
    pico_stdlib
    hardware_gpio
    pico_cyw43_arch_none
    pico_multicore
)

pico_add_extra_outputs(pc1500_expansion_rp2350)

# ---- Standalone SD-bridge bring-up/test firmware -- separate image,
# deliberately independent of the PC-1500 bus loop (no board_pins.h bus
# GPIOs beyond the I2C pair, no monitor.c at all). Flash this alone, with
# the dongle NOT connected to the PC-1500, to test the SC18IS602B/SD
# driver over USB serial in isolation -- see main_sd_test.c's own top
# comment for why (SDLS and, separately, ECVER both stopped responding
# on real hardware, suggesting the single-core firmware hangs somewhere
# in the SD path rather than the SD command failing cleanly). ----
add_executable(sd_bridge_test
    main_sd_test.c
    greenpak_i2c.c
    sc18is602b.c
    diskio_sd_bridge.c
    ${FATFS_CORE_DIR}/ff.c
    ${FATFS_CORE_DIR}/ffsystem.c
    ${FATFS_CORE_DIR}/ffunicode.c
)

target_include_directories(sd_bridge_test BEFORE PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}       # our ffconf.h, ahead of the submodule's own
)
target_include_directories(sd_bridge_test PRIVATE
    ${FATFS_CORE_DIR}               # ff.h, diskio.h
)

pico_set_program_name(sd_bridge_test "sd_bridge_test")
pico_set_program_version(sd_bridge_test "0.1.0")

pico_enable_stdio_usb(sd_bridge_test 1)
pico_enable_stdio_uart(sd_bridge_test 0)

target_link_libraries(sd_bridge_test
    pico_stdlib
    hardware_gpio
)

pico_add_extra_outputs(sd_bridge_test)
