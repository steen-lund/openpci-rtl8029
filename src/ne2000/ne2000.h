/*
 * openpci-rtl8029.device - SANA-II driver for RTL8029 PCI Ethernet cards via openpci.library
 *
 * Copyright (C) 2001-2004 Matay; written by Grzegorz Kraszewski as prm-rtl8029.device
 * Copyright (C) 2024-2026 Steen Lund
 *
 * Modified by Steen Lund from 2024: ported to openpci.library and largely rewritten. The git
 * history records each change and its date.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Matay's original release also forbids including the driver in a commercial product; see
 * README.md and licenses/matay-prm-rtl8029.txt.
 */

/* header file for NE2000 driver for OpenCPI Library */
/* $Id: ne2000.h 0.3 2001/06/09 05:04:52 Krashan Exp Krashan $ */

#include <exec/types.h>

#ifndef NE2000_H
#define NE2000_H

/* some register constans */

#define COMMAND_PAGE0   0x00
#define COMMAND_PAGE1   0x40
#define COMMAND_PAGE2   0x80
#define COMMAND_READ    0x08
#define COMMAND_WRITE   0x10
#define COMMAND_SEND    0x18
#define COMMAND_ABORT   0x20
#define COMMAND_UNUSED2 0x10  // Not used in the driver
#define COMMAND_UNUSED1 0x08  // Not used in the driver
#define COMMAND_TXP     0x04
#define COMMAND_START   0x02
#define COMMAND_STOP    0x01

#define TXCFG_COLOFFSET 0x10
#define TXCFG_ATDIS     0x08
#define TXCFG_LOOP_MASK 0x06
#define TXCFG_LOOP_NONE 0x00
#define TXCFG_LOOP_INT  0x02
#define TXCFG_LOOP_EXT  0x04
#define TXCFG_CRC       0x01

#define RXCFG_MONITOR   0x20
#define RXCFG_PROM      0x10
#define RXCFG_MCAST     0x08
#define RXCFG_BCAST     0x04
#define RXCFG_SHORTPKT  0x02
#define RXCFG_RXERR     0x01

#define DTCFG_FIFOTRSH  0x60
#define DTCFG_FIFO_8    0x40
#define DTCFG_AUTORM    0x10
#define DTCFG_LOOPSEL   0x08
#define DTCFG_LAS       0x04
#define DTCFG_BYTESWAP  0x02
#define DTCFG_WIDE      0x01

#define INT_RESET       0x80
#define INT_DMAFINISH   0x40
#define INT_COUNTEROVF  0x20
#define INT_OVERFLOW    0x10
#define INT_TXERROR     0x08
#define INT_RXERROR     0x04
#define INT_TXPACKET    0x02
#define INT_RXPACKET    0x01

#define TXS_TRANSMITTED_OK    0x01
#define TXS_COLLIDED          0x04   /* collided at least once (retried) */
#define TXS_ABORTED           0x08   /* gave up after 16 collisions */
#define TXS_CARRIER_LOST      0x10   /* carrier lost during transmission */
#define TXS_LATE_COLLISION    0x80   /* collision after the first 64 bytes */

#define NE2000_COMMAND             0
#define NE2000_PAGE_START          1
#define NE2000_PHYSICAL_ADDR0      1      /* register page 1 */
#define NE2000_PAGE_STOP           2
#define NE2000_BOUNDARY            3
#define NE2000_TX_PAGE_START       4      /* write */
#define NE2000_TX_STATUS           4      /* read */
#define NE2000_TX_COUNTER0         5
#define NE2000_COLLISIONS          5      /* read: collisions of the last transmit */
#define NE2000_RX_STATUS          12      /* read: status of the last received frame */
#define NE2000_CNT_ALIGN_ERRORS   13      /* read: tally counters, cleared by reading */
#define NE2000_CNT_CRC_ERRORS     14
#define NE2000_CNT_MISSED         15
#define COMMAND_PAGE3   0xC0              /* RTL8029 configuration registers */
#define RTL8029_CONFIG0            3      /* register page 3 */
#define RTL8029_CONFIG2            5
#define RTL8029_CONFIG3            6
#define NE2000_TX_COUNTER1         6
#define NE2000_CURRENT_PAGE        7      /* register page 1 */
#define NE2000_INT_STATUS          7
#define NE2000_DMA_START_ADDR0     8
#define NE2000_DMA_START_ADDR1     9
#define NE2000_DMA_COUNTER0       10
#define NE2000_DMA_COUNTER1       11
#define NE2000_RX_CONFIG          12
#define NE2000_TX_CONFIG          13
#define NE2000_DATA_CONFIG        14
#define NE2000_INT_MASK           15

struct Ne2000
 {
  UBYTE regs[16];
  union                 /* data port: 16-bit, or 32-bit on cards that allow it */
   {
    UWORD DMAPort;
    ULONG DMAPortLong;
   };
  UWORD pad1;
  UWORD pad2;
  UWORD ResetPort;
 };


/* Ethernet and IP. BSD names where they exist (netinet/if_ether.h is not usable here: it  */
/* pulls in the socket API). Frame sizes exclude the 4-byte CRC, which the card adds and    */
/* strips; BSD's ETHER_MIN_LEN/ETHER_MAX_LEN (64/1518) include it, hence different names.   */

#define ETHER_ADDR_LEN   6                                /* bytes in a station address */
#define ETHER_HDR_LEN    (2 * ETHER_ADDR_LEN + 2)         /* destination, source, type  */
#define ETHERMTU         1500                             /* longest payload            */
#define ETHER_MIN_FRAME  60                               /* shortest frame on the wire */
#define ETHER_MAX_FRAME  (ETHER_HDR_LEN + ETHERMTU)       /* longest frame: 1514        */
#define ETHERTYPE_IP     0x0800

#define IP_MIN_HDR_LEN   20                               /* IPv4 header without options */
#define IPPROTO_ICMP     1
#define IPPROTO_TCP      6
#define IPPROTO_UDP      17
#define ICMP_ECHO        8                                /* echo request (ping)        */

struct EthFrame
 {
  UBYTE ef_DestAddr[ETHER_ADDR_LEN];
  UBYTE ef_SrcAddr[ETHER_ADDR_LEN];
  UWORD ef_Type;
  UBYTE ef_Data[ETHER_MIN_FRAME - ETHER_HDR_LEN];   /* shortest possible */
 };

#endif
