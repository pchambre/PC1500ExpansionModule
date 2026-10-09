/* board_pins.h -- the GPIO map of the board this build is for (2026-10-09).
 *
 * PC1500_TARGET=dongle (the default, CMakeLists.txt): the Pico 2 W dongle,
 * board_pins_dongle.h. PC1500_TARGET=card defines PC1500_TARGET_CARD: the
 * RP2354B internal card, board_pins_card.h. Builds that define neither
 * (pc1500emu's ExpansionMock compiles keywords.c) get the dongle's. */
#pragma once

#ifdef PC1500_TARGET_CARD
#include "board_pins_card.h"
#else
#include "board_pins_dongle.h"
#endif

/* The bus PIO programs (read_serve.pio, write_serve.pio) run on pio0/pio1
 * at their default GPIO base 0, so each sees GPIO0-31 only: the address
 * bus, the data bus and both triggers must all be in there. The card's
 * schematic once wasn't (D3-D7 on GPIO35-39). */
_Static_assert(ADDR_PIN_BASE + ADDR_PIN_COUNT <= 32 && DATA_PIN_BASE + DATA_PIN_COUNT <= 32
               && PIN_TRIG_RD < 32 && PIN_TRIG_WR < 32,
               "the bus pins must be in the bus PIOs' GPIO0-31 window");
