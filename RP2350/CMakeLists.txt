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
set(QMI_CS1_SDSPI_DIR lib/qmi_cs1_sdspi)

add_executable(pc1500_expansion_rp2350
    main.c
    monitor.c
    ${FATFS_CORE_DIR}/ff.c
    ${FATFS_CORE_DIR}/ffsystem.c
    ${FATFS_CORE_DIR}/ffunicode.c
    ${QMI_CS1_SDSPI_DIR}/qmi_cs1_spi.c
    ${QMI_CS1_SDSPI_DIR}/qmi_cs1_sd.c
    ${QMI_CS1_SDSPI_DIR}/diskio_qmi_cs1_sd.c
)

target_include_directories(pc1500_expansion_rp2350 BEFORE PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}       # our ffconf.h, ahead of the submodule's own
)
target_include_directories(pc1500_expansion_rp2350 PRIVATE
    ${FATFS_CORE_DIR}               # ff.h, diskio.h
    ${QMI_CS1_SDSPI_DIR}            # qmi_cs1_spi.h, qmi_cs1_sd.h
)

pico_set_program_name(pc1500_expansion_rp2350 "pc1500_expansion_rp2350")
pico_set_program_version(pc1500_expansion_rp2350 "0.1.0")

# Deliberately NOT using the vendored no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
# library's own add_subdirectory/INTERFACE target: that would also pull
# in its sd_driver/ (hardware_spi-based SD driver), which would collide
# at link time with this project's own diskio_qmi_cs1_sd.c -- both
# define disk_initialize()/disk_read()/etc. Only the library's
# protocol-agnostic FatFs core (ff.c/ffsystem.c/ffunicode.c, sourced
# directly above) is used; the SD card itself goes through QMI CS1
# (lib/qmi_cs1_sdspi/), not a normal SPI peripheral -- see that
# directory's own README.md.
#
# pico_cyw43_arch_none (WiFi/BT init, needed for Pico 2 W's onboard LED)
# isn't linked yet -- nothing here uses it; add it back once WiFi work
# starts.
target_link_libraries(pc1500_expansion_rp2350
    pico_stdlib
    pico_multicore
    hardware_gpio
)

pico_add_extra_outputs(pc1500_expansion_rp2350)
