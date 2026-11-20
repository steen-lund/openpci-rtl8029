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

/*--------------------------------------------------------------------------*/
/* Resident header written for openpci-rtl8029.device                    */
/* $VER: header.s 2.0 (28.09.2024)                                          */
/* 2.0                                                                      */
/*     - Renamed to openpci-rtl8029.device                                  */
/*                                                                          */
/*--------------------------------------------------------------------------*/

#include "rev.h"

            .global     _DevInit
            .global     _romtag
            .global     _IdString
            .global     _CopyB

/* Empty function protecting against running library as executable. */

            MOVEQ       #-1,d0
            RTS

/* Resident table. */

_romtag:    .short      0x4AFC
            .long       _romtag
            .long       _tagend
            .byte       0,1,3,0
            .long       _devname
            .long       _IdString
            .long       _DevInit


_tagend:
            .align      2
_devname:   .string     "openpci-rtl8029.device"

