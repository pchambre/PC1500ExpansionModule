/* monitor.h
 *
 * The ported PSoC5 bus-servicing loop + DoCommand() SD command
 * dispatcher, from Design01_NonDMA_8K_PV_Swap.cydsn/main.c. See the
 * repo's plan history / RP2350/README.md for the full port writeup.
 */
#pragma once

#include <stdbool.h>

/* Set (true) by greenpak_i2c_write()/greenpak_i2c_read() (core1, or
 * whichever core happens to call them -- currently always core1, inside
 * DoCommand()) immediately before issuing a real I2C transaction on the
 * shared GreenPAK/SD-bridge bus. monitor_run() (core0) polls this AFTER
 * its own watchdog check and write-dispatch forwarding each iteration --
 * deliberately last, so a real drive-activity LED update can never delay
 * either of those -- and, if set, clears it and turns the LED ON itself
 * (cyw43_arch_gpio_put() may only ever be called from core0, see main.c's
 * own comment). Paired with g_command_done_pending below for the OFF
 * side. 2026-09-22: replaces the previous BUSY-status-plus-timer blink,
 * which (a) only reflected "some command is in flight," not real bus
 * activity, and (b) sat BEFORE the dispatch check in the loop, so a
 * blocking cyw43_arch_gpio_put() call could delay noticing a new
 * dispatch -- moving it after the dispatch check removes that risk
 * regardless of how long any individual toggle call takes. */
extern volatile bool g_i2c_activity_pending;

/* Set (true) by WriteStatus() (monitor.c) whenever it writes anything
 * other than EXP_STATUS_BUSY -- i.e. exactly when a command's status
 * stops being BUSY, board owner's own request ("DoCommand() to reset it
 * back to off when the command is done, or when status busy is
 * cleared"). Checked by monitor_run() alongside g_i2c_activity_pending,
 * same ordering/rationale -- if both are set in the same iteration,
 * "done" is checked after "activity" so the command-just-finished OFF
 * state wins, matching it being the more current, authoritative one. */
extern volatile bool g_command_done_pending;

/* Zeroes the shared 8K data window and loads the expansion ROM image
 * into its ROM region (pages 8-31). Must be called from main.c BEFORE
 * monitor_run() -- monitor_run() starts reading this buffer immediately
 * once its bus loop is live, with no further synchronization. */
/* True while the CYW43 is initialized -- set by main.c after a successful
 * cyw43_arch_init(), cleared while STAGE RAM sleep has it powered down and
 * set again once it's re-initialized on wake (monitor.c's "STAGE RAM sleep"
 * section). Every activity-LED call checks it first. */
extern bool g_cyw43_up;

/* Called by diskio_sd_bridge.c's disk_status() when the SD card has been
 * removed or swapped: forgets monitor.c's open-file and SDOPEN-channel
 * bookkeeping, whose FatFs handles belong to the old card. Core1 only (it
 * runs inside a FatFs call from DoCommand()). */
void monitor_sd_card_changed(void);

void monitor_init_buffer(void);

/* Boot-time GreenPAK check. The GreenPAKs and the SRAM stay powered from
 * VGG while the Pico is off, so a finished STAGE survives a power cycle:
 * if both Remap bits are set and GP1's write-enable is clear, the staged
 * ROM is kept (RAM mode, no re-copy). Otherwise -- Remap bits that
 * disagree, write-enable still set from an interrupted copy, or a failed
 * read -- all three virtual inputs are forced back to ROM_FROM_MCU, the
 * recovery this function has always provided: a stuck Remap bit over
 * half-written SRAM would otherwise survive every RP2350 reboot/reflash.
 * Call once from main(), before monitor_run() starts serving bus reads. */
void monitor_init_greenpak(void);

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
