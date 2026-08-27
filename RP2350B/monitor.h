/* monitor.h
 *
 * The ported PSoC5 bus-servicing loop + DoCommand() SD command
 * dispatcher, from Design01_NonDMA_8K_PV_Swap.cydsn/main.c. See the
 * repo's plan history / RP2350/README.md for the full port writeup.
 */
#pragma once

/* Runs forever on whichever core calls it (core1, per main.c). Must be
 * called after the FatFs volume is mounted (main.c's job) since
 * DoCommand()'s SD cases call straight into f_*. */
void monitor_run(void);
