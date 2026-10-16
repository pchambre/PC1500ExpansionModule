/* Host-test stand-in for the pico-sdk header selftest.c includes. */
#pragma once

#include <stdint.h>

static inline void sleep_us(uint64_t us) { (void)us; }
