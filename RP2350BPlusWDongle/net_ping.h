/* net_ping.h -- PING (2026-10-07): ICMP echo over Wi-Fi, a round at a time
 * (EXP_COMMAND_PING_*, pc_exp.h), for seeing whether the dongle reaches a
 * host at all. Also the host name lookup PING and SSH share.
 *
 * Threading, as ssh_session.h: lwIP on core0 (a raw ICMP pcb whose callback
 * notes the reply), everything else on core1 in the commands. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifndef PC1500_NO_WIFI
#include "lwip/ip_addr.h"
#endif

/* core1: an EXP_COMMAND_PING_* command. On the card (PC1500_NO_WIFI),
 * no_radio.c's, which answers ERROR. */
uint8_t net_ping_command(uint8_t command, uint8_t *window);

#ifndef PC1500_NO_WIFI
/* core1: `host` (a dotted address, or a name for DNS) to an address, within
 * `timeout_ms`. False if it doesn't resolve. */
bool net_resolve(const char *host, ip_addr_t *out, uint32_t timeout_ms);
#endif
