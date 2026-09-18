/* monitor.h
 *
 * The ported PSoC5 bus-servicing loop + DoCommand() SD command
 * dispatcher, from Design01_NonDMA_8K_PV_Swap.cydsn/main.c. See the
 * repo's plan history / RP2350/README.md for the full port writeup.
 */
#pragma once

/* Zeroes the shared 8K data window and loads the expansion ROM image
 * into its ROM region (pages 8-31). Must be called from main.c BEFORE
 * monitor_run() -- monitor_run() starts reading this buffer immediately
 * once its bus loop is live, with no further synchronization. */
void monitor_init_buffer(void);

/* Runs forever on core0: the tight bus-servicing loop only -- never calls
 * DoCommand() directly. On a write to the instruction address, it stamps
 * EXP_STATUS_BUSY into the shared buffer itself (so the very next read
 * anywhere reflects it immediately, no special handling needed) and hands
 * the command byte to core1 over the SIO mailbox FIFO, then keeps
 * servicing bus cycles without ever blocking. See main.c's own comment
 * for why this replaced the original single-core design: core0 blocking
 * inside DoCommand() left the bus floating (pulled-down, per InitGpio()'s
 * comment) for however long an SD command took, and 0x00 is
 * EXP_STATUS_READY's own value -- indistinguishable from "already done"
 * to the LH5801's own busy-poll loops, causing real, confirmed data
 * corruption (2026-09-17, SDLS producing different garbage-triggered
 * crashes on every attempt). */
void monitor_run(void);

/* Runs forever on core1: pops a command byte pushed by monitor_run() and
 * calls DoCommand() with it, blocking core1 (never core0) for the
 * command's full duration -- including whatever real SD/I2C-bridge work
 * it does. DoCommand() itself still re-stamps EXP_STATUS_BUSY as its own
 * first action (redundant with core0's own stamp just before the handoff,
 * but harmless, and keeps DoCommand() correct if ever called any other
 * way) and writes the real final status last, which core0's already-
 * running bus loop picks up and serves on the very next read with no
 * extra synchronization needed -- ordinary shared-SRAM visibility between
 * RP2350's two cores, no cache-coherency concerns on this chip. Must be
 * launched (multicore_launch_core1) before monitor_run() starts servicing
 * writes, same buffer-init ordering requirement as monitor_run() itself. */
void monitor_command_worker(void);
