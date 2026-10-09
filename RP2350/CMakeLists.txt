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
cmake_minimum_required(VERSION 3.13)

# Which board (2026-10-09). One source tree, two boards -- each in its own
# build directory, since PICO_BOARD is cached:
#   dongle (default): the Pico 2 W dongle. RP2350 + CYW43439 (Wi-Fi,
#           BTstack BLE + classic), SD through the SC18IS602B bridge.
#           build/, firmware pc1500_expansion_rp2350.
#   card:   the RP2354B internal card (PC1500-RP2354B-BLE-more-RAM). No
#           Wi-Fi, SD on SPI1, its BGM220S BLE module not driven yet (no
#           stack chosen; no BTstack linked). boards/pc1500_card_rp2354b.h,
#           board_pins_card.h.
#           cmake -B build-card -DPC1500_TARGET=card; pc1500_card_rp2354b.
# The board must be set before pico_sdk_import.cmake is included, matching
# every official pico-examples CMakeLists.txt that targets a *_w board.
set(PC1500_TARGET dongle CACHE STRING "Board to build for: dongle (Pico 2 W) or card (RP2354B internal card)")
set_property(CACHE PC1500_TARGET PROPERTY STRINGS dongle card)
if(PC1500_TARGET STREQUAL "card")
    set(PICO_BOARD pc1500_card_rp2354b CACHE STRING "Board type")
    set(PICO_BOARD_HEADER_DIRS ${CMAKE_CURRENT_LIST_DIR}/boards)
    set(FW_TARGET pc1500_card_rp2354b)
elseif(PC1500_TARGET STREQUAL "dongle")
    set(PICO_BOARD pico2_w CACHE STRING "Board type")
    set(FW_TARGET pc1500_expansion_rp2350)
else()
    message(FATAL_ERROR "PC1500_TARGET must be dongle or card, not '${PC1500_TARGET}'")
endif()
if((PC1500_TARGET STREQUAL "card") AND NOT (PICO_BOARD STREQUAL "pc1500_card_rp2354b")
   OR (PC1500_TARGET STREQUAL "dongle") AND NOT (PICO_BOARD STREQUAL "pico2_w"))
    message(FATAL_ERROR "PICO_BOARD '${PICO_BOARD}' is cached for another target -- use a separate build directory per PC1500_TARGET")
endif()

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
# bridge primitive), and diskio_sd.c (the FatFs disk_*() driver
# built on it, implementing the standard SD-over-SPI command set).
# qmi_cs1_spi_init() previously hijacked GPIO19, which is really this
# board's D6 data-bus line, breaking the bus outright -- don't add
# lib/qmi_cs1_sdspi/ back here.
#
# Both boards' firmware (2026-10-09): the sources they share, then each
# board's own.
set(FW_COMMON_SOURCES
    main.c
    monitor.c
    keywords.c
    greenpak_i2c.c
    greenpak_virtual_io.c
    diskio_sd.c         # the SD card's FatFs driver, over sd_bus.h (the board's own, below)
    mcu_log.c
    mcu_config.c
    mcu_store.c
    plotter.c
    basic_xlate.c
    link_secure.c       # the Link's authentication (2026-10-03, BLE_PROTOCOL.md sec.7)
    link_store.c
    kbd_seq.c           # the external keyboard (2026-10-04): its key, and its driver's wait loop
    time_sync.c         # the PC-1500's clock from WFCON/BLCON (2026-10-08)
    time_zone.c         #   MCONF TZ: portable, as the next
    tz_table.c          #   generated: tools/gen_tz_table.py
    cmd_history.c       # BASIC's command history (2026-10-07, MCONF HISTORY): portable
    history_session.c   #   and on the dongle: flash, the commands
    third_party/monocypher/monocypher.c
    third_party/monocypher/monocypher-ed25519.c
    plot_text.c
    hershey_simplex.c
    ${FATFS_CORE_DIR}/ff.c
    ${FATFS_CORE_DIR}/ffsystem.c
    ${FATFS_CORE_DIR}/ffunicode.c
)
set(FW_DONGLE_SOURCES
    sc18is602b.c        # the SD card behind the I2C-to-SPI bridge
    sd_bus_bridge.c     #   as sd_bus.h
    ble_link.c
    kbd_host.c          # the external keyboard's Bluetooth side (classic HID host)
    bt_store.c          #   and its bonds
    wifi_link.c         # Wi-Fi station mode (2026-10-06): the WF* keywords
    wifi_store.c        #   and the remembered networks
    ssh_session.c       # SSH (2026-10-07): the SSH keyword's session
    ssh_client.c        #   the protocol (portable, as the next three)
    ssh_term.c          #   the 26-character view of an 80-column terminal
    ssh_keys.c          #   the PC-1500's keys as a terminal's
    sha256.c
    ssh_store.c         #   the device key and known hosts
    net_ping.c          # WFPING (2026-10-07), and the host lookup SSH shares
    net_time.c          # SNTP, for Wi-Fi
)
set(FW_CARD_SOURCES
    sd_bus_spi.c        # the SD card on SPI1, as sd_bus.h
    no_radio.c          # Wi-Fi/SSH/BLE stand-ins: no radio stack yet
)

if(PC1500_TARGET STREQUAL "card")
    add_executable(${FW_TARGET} ${FW_COMMON_SOURCES} ${FW_CARD_SOURCES})
else()
    add_executable(${FW_TARGET} ${FW_COMMON_SOURCES} ${FW_DONGLE_SOURCES})
endif()

       # our ffconf.h, ahead of the submodule's own
target_include_directories(${FW_TARGET} BEFORE PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)

message(STATUS "DEBUG: Looking for rom_image.h at: ${CMAKE_CURRENT_LIST_DIR}/../Design01_NonDMA_8K_PV_Swap.cydsn/rom")


get_filename_component(ROM_IMAGE_DIR 
    "${CMAKE_CURRENT_LIST_DIR}/../Design01_NonDMA_8K_PV_Swap.cydsn/rom" 
    ABSOLUTE
)
  # rom_image.h -- see monitor.c's own comment for why this isn't hand-copied
               # ff.h, diskio.h
target_include_directories(${FW_TARGET} PRIVATE
    ${FATFS_CORE_DIR}
    ${ROM_IMAGE_DIR}
)

# PIO-driven read-trigger path (2026-09-18) -- see read_serve.pio's own
# header comment for the design and its OneROM/MIT-licensed lineage.
#pico_generate_pio_header(pc1500_expansion_rp2350 ${CMAKE_CURRENT_LIST_DIR}/read_serve.pio)

# PIO-driven write-trigger path (2026-09-19) -- see write_serve.pio's own
# header comment for the design.
#pico_generate_pio_header(pc1500_expansion_rp2350 ${CMAKE_CURRENT_LIST_DIR}/write_serve.pio)

# PIO-driven read-trigger path
pico_generate_pio_header(${FW_TARGET} 
    ${CMAKE_CURRENT_LIST_DIR}/read_serve.pio
    OUTPUT_DIR ${CMAKE_CURRENT_LIST_DIR}  # <--- FORCES it into your source tree root
)

# PIO-driven write-trigger path
pico_generate_pio_header(${FW_TARGET} 
    ${CMAKE_CURRENT_LIST_DIR}/write_serve.pio
    OUTPUT_DIR ${CMAKE_CURRENT_LIST_DIR}  # <--- FORCES it into your source tree root
)

pico_set_program_name(${FW_TARGET} "${FW_TARGET}")
pico_set_program_version(${FW_TARGET} "0.1.0")

# TEMPORARY DIAGNOSTIC (2026-09-18): switched from UART stdio to USB CDC
# for the PIO/DMA read-path bring-up -- this board's UART0 pins (GPIO0/1)
# are the SAME physical pins as address bus lines A0/A1 (see board_pins.h),
# so UART stdio was never actually reachable once InitGpio() reclaims
# those pins for the address bus, only nominally present in the build.
# USB CDC rides the same cable already used for picotool flashing, no
# extra hardware needed. Revert to UART (or drop both) once this round of
# PIO/DMA debugging is done, matching the original build's intent.
pico_enable_stdio_usb(${FW_TARGET} 1)
pico_enable_stdio_uart(${FW_TARGET} 0)

# Deliberately NOT using the vendored no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
# library's own add_subdirectory/INTERFACE target: that would also pull
# in its sd_driver/ (hardware_spi-based SD driver), which would collide
# at link time with this project's own diskio_sd.c -- both define
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
target_link_libraries(${FW_TARGET}
    pico_stdlib
    hardware_gpio
    hardware_i2c
    hardware_pio
    hardware_dma
    hardware_flash
    pico_flash
    pico_multicore
    pico_low_power      # DORMANT between commands in STAGE RAM mode -- see monitor.c
    pico_rand           # the Link's keys and nonces (link_secure.c)
)

if(PC1500_TARGET STREQUAL "card")
    target_link_libraries(${FW_TARGET}
        hardware_spi    # the SD card on SPI1 (sd_bus_spi.c)
    )
    # What the card lacks, for the shared sources (board_pins.h, main.c,
    # monitor.c, keywords.c, net_ping.h). Each is a default-on guard
    # (#ifndef PC1500_NO_...), so the dongle -- and pc1500emu's
    # ExpansionMock, which compiles keywords.c -- needs no define at all.
    target_compile_definitions(${FW_TARGET} PRIVATE
        PC1500_TARGET_CARD  # board_pins_card.h
        PC1500_NO_WIFI      # WF*/SSH/WFPING turned away (keywords.c), no_radio.c
        PC1500_NO_CYW43     # no CYW43: no radio power, no LED (main.c, monitor.c)
        PC1500_SD_SPI       # SD on SPI1, not the SC18IS602B bridge (main.c)
        PC1500_NO_BLE       # the BGM220S isn't driven yet: no_radio.c's BLE stand-ins
    )
else()
    target_link_libraries(${FW_TARGET}
        pico_cyw43_arch_lwip_threadsafe_background # Wi-Fi (2026-10-06, wifi_link.h): was
                            # pico_cyw43_arch_none. lwIP runs NO_SYS in the same
                            # background async context BTstack uses; lwipopts.h is ours
        pico_btstack_ble    # BLE (2026-09-27) -- see ble_link.h; btstack_config.h is ours
        pico_btstack_classic # classic, for the external keyboard (2026-10-04, kbd_host.h)
        pico_btstack_cyw43  #   (also switches cyw43_arch_init() to bring BTstack up and
                            #   the CYW43 firmware to the WiFi+Bluetooth image)
    )
    # BTstack's flash store goes where flash_layout.h puts it, not at the SDK's
    # default spot (which overlaps MCONF/STSAVE) -- ble_link.c defines it.
    target_compile_definitions(${FW_TARGET} PRIVATE
        pico_flash_bank_get_storage_offset_func=ble_flash_bank_offset
    )
endif()

pico_add_extra_outputs(${FW_TARGET})

# ---- Standalone SD-bridge bring-up/test firmware -- separate image,
# deliberately independent of the PC-1500 bus loop (no board_pins.h bus
# GPIOs beyond the I2C pair, no monitor.c at all). Flash this alone, with
# the dongle NOT connected to the PC-1500, to test the SC18IS602B/SD
# driver over USB serial in isolation -- see main_sd_test.c's own top
# comment for why (SDLS and, separately, ECVER both stopped responding
# on real hardware, suggesting the single-core firmware hangs somewhere
# in the SD path rather than the SD command failing cleanly). The dongle's
# bridge only (2026-10-09). ----
if(PC1500_TARGET STREQUAL "dongle")
add_executable(sd_bridge_test
    main_sd_test.c
    sd_test_stubs.c     # monitor.c's two symbols, not the bus loop itself
    greenpak_i2c.c
    sc18is602b.c
    sd_bus_bridge.c
    diskio_sd.c
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
    pico_cyw43_arch_none
    hardware_gpio
    hardware_i2c
    hardware_pio
    hardware_dma
)

pico_add_extra_outputs(sd_bridge_test)
endif()
