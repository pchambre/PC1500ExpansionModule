/* lwipopts.h -- lwIP's settings (2026-10-06, Wi-Fi: wifi_link.h).
 *
 * NO_SYS: lwIP runs in the CYW43's background async context on core0
 * (pico_cyw43_arch_lwip_threadsafe_background), the same one BTstack uses;
 * other code reaches it with async_context_execute_sync(). The raw API
 * only -- no sockets, no netconn. IPv4, DHCP and DNS for the coming
 * network keywords (PING, SSH), with buffers for one TCP connection at a
 * time at a modest window -- RAM is plentiful (about 54 KB of 520 KB was
 * in use before Wi-Fi). Based on the pico-examples' lwipopts. */
#pragma once

#define NO_SYS 1
#define LWIP_SOCKET 0
#define LWIP_NETCONN 0
#define MEM_LIBC_MALLOC 0
#define MEM_ALIGNMENT 4
#define MEM_SIZE 16000
#define MEMP_NUM_TCP_SEG 32
#define MEMP_NUM_ARP_QUEUE 10
#define PBUF_POOL_SIZE 24

#define LWIP_IPV4 1
#define LWIP_IPV6 0
#define LWIP_ARP 1
#define LWIP_ETHERNET 1
#define LWIP_ICMP 1
#define LWIP_RAW 1 /* PING */
#define LWIP_TCP 1
#define LWIP_UDP 1
#define LWIP_DNS 1
#define LWIP_DHCP 1
#define DHCP_DOES_ARP_CHECK 0
#define LWIP_DHCP_DOES_ACD_CHECK 0
#define LWIP_TCP_KEEPALIVE 1

#define TCP_MSS 1460
#define TCP_WND (8 * TCP_MSS)
#define TCP_SND_BUF (8 * TCP_MSS)
#define TCP_SND_QUEUELEN ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))

#define LWIP_NETIF_STATUS_CALLBACK 1
#define LWIP_NETIF_LINK_CALLBACK 1
#define LWIP_NETIF_HOSTNAME 1 /* MCONF HOSTNAME, for DHCP */
#define LWIP_NETIF_TX_SINGLE_PBUF 1
#define LWIP_CHKSUM_ALGORITHM 3

#define MEM_STATS 0
#define SYS_STATS 0
#define MEMP_STATS 0
#define LINK_STATS 0
#define LWIP_STATS 0
#define LWIP_STATS_DISPLAY 0
#define LWIP_DEBUG 0
