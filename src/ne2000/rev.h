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

#define DEV_VERSION     2
#define DEV_REVISION    0
#define DEV_NAME        "openpci-rtl8029.device"
#define DEV_IDSTRING    "$VER: openpci-rtl8029.device 2.0 (23.11.2026) (c) 2001-2004 Matay, 2024-2026 Steen Lund\n"
#define DEV_UNITNAME    "openpci-rtl8029.device (x)"
