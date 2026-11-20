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

