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

/// includes and defines

// ignore that STRPTR is unsigned char *, but all string literals are char *
#pragma GCC diagnostic ignored "-Wpointer-sign"

#define __NOLIBBASE__

#include <proto/exec.h>
#include <proto/utility.h>
#include <proto/dos.h>
#include <proto/openpci.h>
#include <proto/timer.h>
#include <exec/execbase.h>
#include <exec/libraries.h>
#include <exec/resident.h>
#include <exec/memory.h>
#include <exec/interrupts.h>
#include <devices/sana2.h>
#include <devices/newstyle.h>
#include <dos/dostags.h>
#include <libraries/pcitags.h>
#include <utility/tagitem.h>
#include <libraries/openpci.h>

#include "rev.h"
#include "ne2000.h"

/* mmu.library, see hw_is_cache_inhibited() */
#include <proto/mmu.h>
#include <mmu/context.h>

/* Card memory, in 256 byte pages: two transmit buffers of one full frame each, so that  */
/* the next frame can be uploaded while one is on the wire, then the receive ring.         */
#define TX_BUFFER   0x40
#define TX_PAGES    6
#define RX_BUFFER   (TX_BUFFER + 2 * TX_PAGES)
#define BUFFER_END  0x80
#define INTMASK     (INT_RXPACKET | INT_TXPACKET | INT_TXERROR | INT_OVERFLOW)

/* The RTL8029 accepts 32-bit accesses to its data port, which halves the number of bus    */
/* cycles per packet (the bus cycles are what packet transfer time consists of). Set to 0  */
/* to move packet data 16 bits at a time.                                                  */
#define USE_LONG_DMA 1

/* Frames are kept in memory starting 2 bytes past a longword boundary, so that the data   */
/* after the 14 byte Ethernet header is longword aligned. The stack copies that data to or */
/* from its own (aligned) buffers, and a copy between differently aligned addresses can be */
/* several times slower. Linux does the same (NET_IP_ALIGN).                                */
#define FRAME_ALIGN 2

/* m68k compilers only guarantee word alignment, even for longword arrays: round up first */
#define FRAME_START(space) ((UBYTE *)((((ULONG)(space)) + 3) & ~3UL) + FRAME_ALIGN)
#define DMAPORT_LONG(ne) ((ne)->DMAPortLong)

/* Word offsets into a received frame in ud_RxBuffer, which is indexed in 16-bit words */
#define RXW_DST   0                          /* destination address */
#define RXW_SRC   (ETHER_ADDR_LEN / 2)       /* source address      */
#define RXW_TYPE  (2 * ETHER_ADDR_LEN / 2)   /* EtherType           */
#define RXW_DATA  (ETHER_HDR_LEN / 2)        /* payload             */

/* PCI card ID's */

#define PCI_VENDOR_REALTEK 0x10EC
#define PCI_DEVICE_RTL8029 0x8029

/* Units 0 to MAX_UNITS-1, one card each. The unit number is one digit of DEV_UNITNAME   */
/* (rev.h), the name of the unit's task, port and interrupt server.                        */
#define MAX_UNITS        4
#define UNIT_DIGIT       (sizeof(DEV_UNITNAME) - 3)   /* the 'x' before the closing ')' */
_Static_assert(MAX_UNITS <= 10, "unit names have room for one digit");

// Unit flags.

#define UF_CONFIGURED   0x01
#define UF_ONLINE       0x02
#define UF_PROMISCUOUS  0x04
#define UF_EXCLUSIVE    0x08    /* opened with SANA2OPF_MINE */

/* Private OpenDevice() flag for diagnostic tools (s2stat): open the unit for queries only. */
/* Such an open is allowed next to an exclusive opener, gets no buffer management and may   */
/* not read or write packets, so it cannot take anything away from the exclusive opener.    */
#define OPENF_MONITOR   0x80000000

// Macro for registerized parameters (used in some OS functions).

#define reg(a) __asm(#a)

// Macro for declaring local libraries bases.

#define USE(a) struct Library *a = dd->dd_##a;
#define USE_U(a) struct Library *a = ud->ud_##a;

// Debug output to the serial port, only in a PDEBUG build. Never per packet: serial output
// takes far longer than a packet.

#if PDEBUG

#define USE_D(a) struct Library* a = dd->dd_##a;
#define USE_UD(a) struct Library* a = ud->ud_##a;

#define DBG(a) KPrintF(a "\n")
#define DBG_NONEWLINE(a) KPrintF(a)
#define DBG_U(a) KPrintF(a "\n")
#define DBG_T(a) KPrintF(a "\n")
#define DBG1(a, b) KPrintF(a "\n", (LONG)b)
#define DBG1_W_NONEWLINE(a, b) KPrintF(a , (WORD)b)
#define DBG1_U(a, b) KPrintF(a "\n", (LONG)b)
#define DBG1_T(a, b) KPrintF(a "\n", (LONG)b)
#define DBG2(a, b, c) KPrintF(a "\n", (LONG)b, (LONG)c)
#define DBG2_U(a, b, c) KPrintF(a "\n", (LONG)b, (LONG)c)
#define DBG2_T(a, b, c) KPrintF(a "\n", (LONG)b, (LONG)c)

#else
#define USE_D(a)
#define USE_UD(a)
#define DBG(a)
#define DBG_NONEWLINE(a)
#define DBG_U(a)
#define DBG_T(a)
#define DBG1(a, b)
#define DBG1_W_NONEWLINE(a, b)
#define DBG1_U(a, b)
#define DBG1_T(a, b)
#define DBG2(a, b, c)
#define DBG2_U(a, b, c)
#define DBG2_T(a, b, c)
#endif

/* Serial output for one-time initialisation messages, in every build */
#include <clib/debug_protos.h>
#define INIT_DBG(a) KPrintF(a "\n")
#define INIT_DBG1(a,b) KPrintF(a "\n",(LONG)b)
#define INIT_DBG_U(a) KPrintF(a "\n")
#define INIT_DBG1_U(a,b) KPrintF(a "\n",(LONG)b)

///
/// diagnostics

/* Diagnostic counters and state, readable at any time with S2_GETSPECIALSTATS (see the   */
/* s2stat tool). They are copied in the caller's context, so they can be read even when    */
/* the unit task is stuck - DG_TASKSTATE then tells where.                                 */

enum
{
    /* Unit task and card setup */
    DG_TASKSTATE,     /* what the unit task is doing, TS_* */
    DG_TASKPRI,       /* priority of the unit task */
    DG_MMUINHIBIT,    /* 1 if mmu.library reports the card registers mapped cache-inhibited */
    DG_RTLCONFIG,     /* RTL8029 CONFIG0, CONFIG2, CONFIG3 (low byte); $40 in CONFIG3 = full duplex */

    /* Requests from the stack */
    DG_READSQUEUED,   /* CMD_READ requests received */
    DG_WRITESQUEUED,  /* CMD_WRITE/S2_BROADCAST requests received */

    /* Interrupts and card state */
    DG_INTERRUPTS,    /* interrupts claimed by IntCode() */
    DG_SERVICERUNS,   /* ServiceCard() calls */
    DG_INTSEEN,       /* all interrupt status bits ever seen, including ones the driver does not use */
    DG_LASTINTSTATUS, /* last non-zero interrupt status seen by ServiceCard() */
    DG_NEXTPAGE,      /* driver's ring read pointer */
    DG_LASTCURRENT,   /* card's ring write pointer when last read */
    DG_TXPENDING,     /* requests held in the card's transmit buffers (0 to 2) */

    /* Receive */
    DG_RXFRAMES,      /* frames read from the ring */
    DG_RXDELIVERED,   /* received frames handed to a pending CMD_READ */
    DG_RXNOREADER,    /* received frames dropped: no pending CMD_READ for the packet type */
    DG_LASTDROPTYPE,  /* packet type of the last frame dropped that way */
    DG_RXWRAPPED,     /* received frames that wrapped around the end of the card's ring */

    /* Receive errors */
    DG_OVERFLOWS,     /* ring overflow interrupts */
    DG_RXMISSED,      /* card tally: frames the card could not store (ring full or card stopped) */
    DG_RXCRCERRORS,   /* card tally: received frames with CRC errors (dropped by the card) */
    DG_RXALIGNERRORS, /* card tally: received frames with alignment errors (dropped by the card) */
    DG_RXERRINTS,     /* times the card flagged a receive error (frame rejected by the card) */
    DG_LASTRXSTATUS,  /* the card's receive status register when it last did */
    DG_RXBADHEADER,   /* corrupt ring headers (each resets the ring) */
    DG_LASTBADHEADER, /* the last corrupt ring header */

    /* Transmit */
    DG_TXSTARTED,     /* transmits started */
    DG_TXDONE,        /* transmit interrupts, ok */
    DG_TXWIREIDLE,    /* transmits finished with no next frame waiting on the card (wire goes idle) */
    DG_TXIDLEMSGWAIT, /* ... while write requests were already queued: the driver, not the    */
                      /*     stack, was too slow to pick them up                              */
    DG_TXWAITPOLLS,   /* status reads spent waiting for upload completion, in total */

    /* Transmit errors */
    DG_TXERRORS,      /* transmit interrupts, error */
    DG_TXCOLLISIONS,  /* collisions while transmitting (each one retried by the card) */
    DG_TXLATECOLL,    /* transmits hit by a collision after the first 64 bytes */
    DG_TXCARRIERLOST, /* transmits during which the carrier was lost */
    DG_TXNOPENDING,   /* transmit interrupts with no request in flight */
    DG_TXDMATIMEOUT,  /* upload to the card did not report completion */

    /* Receive checksums (RX_VERIFY_CHECKSUMS) */
    DG_RXSUMCHECKED,  /* received IPv4 frames whose checksums the driver verified (diagnostic) */
    DG_RXSUMBAD,      /* ... that failed: the data read from the card is damaged */
    DG_RXSUMBADWRAP,  /* ... of which wrapped around the end of the ring */
    DG_LASTSUMBAD,    /* last damaged frame: ring start page << 16 | frame length */
    DG_RXECHOREQ,     /* ICMP echo requests (pings) received, for counting against a known number sent */

    /* Timing (make PERF=1) */
    DG_ECLOCKFREQ,    /* PERF: E-clock ticks per second, the unit of the Ticks counters */
    DG_TICKSSERVICE,  /* PERF: total time spent in ServiceCard() */
    DG_TICKSRXREAD,   /* PERF: part of TicksService reading frames from the card */
    DG_TICKSRXCOPY,   /* PERF: part of TicksService handing frames to the stack (its CopyToBuff + reply) */
    DG_TICKSTX,       /* PERF: total time spent in UploadPacket() (the stack's CopyFromBuff + upload) */
    DG_TICKSTXCOPY,   /* PERF: part of TicksTx in the stack's CopyFromBuff */
    DG_TICKSTXUPLOAD, /* PERF: part of TicksTx writing the frame to the card */
    DG_TICKSTXWAIT,   /* PERF: part of TicksTx waiting for the card to report the upload done */
    DG_TICKSINTLAT,   /* PERF: total time from the card's interrupt to the unit task servicing it */
    DG_MAXTICKSINTLAT, /* PERF: longest time from the card's interrupt to the unit task servicing it */
    DG_SLOWINTLAT,    /* PERF: times that took more than 2 ms */
    DG_MAXTICKSTURN,  /* PERF: longest time from handing a frame to the stack to its next write request */
    DG_SLOWTURN,      /* PERF: times that took more than 5 ms */
    DG_COUNT
};

#define TS_WAITING   0 /* in Wait() */
#define TS_PORT      1 /* handling requests */
#define TS_SERVICE   2 /* in ServiceCard() */
#define TS_RXDRAIN   3 /* reading packets from the ring */
#define TS_SEND      4 /* in UploadPacket() */
#define TS_OVERFLOW  5 /* in overflow recovery */

/* Record names, as S2_GETSPECIALSTATS reports them; s2stat groups them by these names */
static const char *const DiagNames[DG_COUNT] =
{
    [DG_TASKSTATE] = "TaskState",
    [DG_TASKPRI] = "TaskPri",
    [DG_MMUINHIBIT] = "MMUInhibit",
    [DG_RTLCONFIG] = "RtlConfig",
    [DG_READSQUEUED] = "ReadsQueued",
    [DG_WRITESQUEUED] = "WritesQueued",
    [DG_INTERRUPTS] = "Interrupts",
    [DG_SERVICERUNS] = "ServiceRuns",
    [DG_INTSEEN] = "IntSeen",
    [DG_LASTINTSTATUS] = "LastIntStatus",
    [DG_NEXTPAGE] = "NextPage",
    [DG_LASTCURRENT] = "LastCurrent",
    [DG_TXPENDING] = "TxPending",
    [DG_RXFRAMES] = "RxFrames",
    [DG_RXDELIVERED] = "RxDelivered",
    [DG_RXNOREADER] = "RxNoReader",
    [DG_LASTDROPTYPE] = "LastDropType",
    [DG_RXWRAPPED] = "RxWrapped",
    [DG_OVERFLOWS] = "Overflows",
    [DG_RXMISSED] = "RxMissed",
    [DG_RXCRCERRORS] = "RxCrcErrors",
    [DG_RXALIGNERRORS] = "RxAlignErrors",
    [DG_RXERRINTS] = "RxErrInts",
    [DG_LASTRXSTATUS] = "LastRxStatus",
    [DG_RXBADHEADER] = "RxBadHeader",
    [DG_LASTBADHEADER] = "LastBadHeader",
    [DG_TXSTARTED] = "TxStarted",
    [DG_TXDONE] = "TxDone",
    [DG_TXWIREIDLE] = "TxWireIdle",
    [DG_TXIDLEMSGWAIT] = "TxIdleMsgWait",
    [DG_TXWAITPOLLS] = "TxWaitPolls",
    [DG_TXERRORS] = "TxErrors",
    [DG_TXCOLLISIONS] = "TxCollisions",
    [DG_TXLATECOLL] = "TxLateColl",
    [DG_TXCARRIERLOST] = "TxCarrierLost",
    [DG_TXNOPENDING] = "TxNoPending",
    [DG_TXDMATIMEOUT] = "TxDmaTimeout",
    [DG_RXSUMCHECKED] = "RxSumChecked",
    [DG_RXSUMBAD] = "RxSumBad",
    [DG_RXSUMBADWRAP] = "RxSumBadWrap",
    [DG_LASTSUMBAD] = "LastSumBad",
    [DG_RXECHOREQ] = "RxEchoReq",
    [DG_ECLOCKFREQ] = "EClockFreq",
    [DG_TICKSSERVICE] = "TicksService",
    [DG_TICKSRXREAD] = "TicksRxRead",
    [DG_TICKSRXCOPY] = "TicksRxCopy",
    [DG_TICKSTX] = "TicksTx",
    [DG_TICKSTXCOPY] = "TicksTxCopy",
    [DG_TICKSTXUPLOAD] = "TicksTxUpload",
    [DG_TICKSTXWAIT] = "TicksTxWait",
    [DG_TICKSINTLAT] = "TicksIntLat",
    [DG_MAXTICKSINTLAT] = "MaxTicksIntLat",
    [DG_SLOWINTLAT] = "SlowIntLat",
    [DG_MAXTICKSTURN] = "MaxTicksTurn",
    [DG_SLOWTURN] = "SlowTurn",
};

#define DIAG(n) (ud->ud_Diag[n])

/* Priority of the unit task, which does all packet work. It must get the CPU within a few */
/* milliseconds of the card's interrupt: the receive ring holds about 10 ms of traffic.    */
#define UNIT_TASK_PRI      6
#define UNIT_TASK_PRI_VAR  "openpci-rtl8029.pri"

/* Timing of the packet paths, in E-clock ticks (about 1.4 microseconds). It costs several */
/* timer reads per packet, so it is a build option: 'make PERF=1'. Without it the Ticks,    */
/* MaxTicks and Slow counters stay 0. PERF_VARS goes with a function's declarations. Only   */
/* the low 32 bits of the E-clock are used: fine for intervals far below an hour.           */
#ifndef PERF_TIMING
#define PERF_TIMING 0
#endif

#if PERF_TIMING
#define PERF_VARS         struct Library *TimerBase = ud->ud_TimerBase; struct EClockVal perf_t0, perf_t1
#define PERF_START()      ReadEClock(&perf_t0)
#define PERF_STOP(n)      (ReadEClock(&perf_t1), DIAG(n) += perf_t1.ev_lo - perf_t0.ev_lo)
#define PERF_LAP(n)       (PERF_STOP(n), perf_t0 = perf_t1)
/* As PERF_LAP, also adding the time to a second (total) counter */
#define PERF_LAP2(n, total) \
    do { ULONG perf_d; ReadEClock(&perf_t1); perf_d = perf_t1.ev_lo - perf_t0.ev_lo; \
         DIAG(n) += perf_d; DIAG(total) += perf_d; perf_t0 = perf_t1; } while (0)
/* Remember when something happened / measure how long ago it was (maximum and count of   */
/* cases over 'slow' ticks)                                                                */
#define PERF_STAMP(stamp, flag) \
    do { struct Library *TimerBase = ud->ud_TimerBase; struct EClockVal perf_now; \
         ReadEClock(&perf_now); (stamp) = perf_now.ev_lo; (flag) = TRUE; } while (0)
#define PERF_SINCE(stamp, flag, nmax, nslow, slow) \
    do { if (flag) { struct Library *TimerBase = ud->ud_TimerBase; struct EClockVal perf_now; ULONG perf_d; \
         ReadEClock(&perf_now); perf_d = perf_now.ev_lo - (stamp); (flag) = FALSE; \
         if (perf_d & 0x80000000UL) perf_d = 0; /* stamp written meanwhile by the other side */ \
         if ((nmax) == DG_MAXTICKSINTLAT) DIAG(DG_TICKSINTLAT) += perf_d; \
         if (perf_d > DIAG(nmax)) DIAG(nmax) = perf_d; \
         if (perf_d > (slow)) DIAG(nslow)++; } } while (0)
#else
#define PERF_VARS         struct EClockVal *perf_unused = NULL
#define PERF_START()      ((void)perf_unused)
#define PERF_STOP(n)      ((void)0)
#define PERF_LAP(n)       ((void)0)
#define PERF_LAP2(n, total) ((void)0)
#define PERF_STAMP(stamp, flag)                    ((void)0)
#define PERF_SINCE(stamp, flag, nmax, nslow, slow) ((void)0)
#endif
#define PERF_TICKS_2MS    1400
#define PERF_TICKS_5MS    3500

///
/// device structures

struct DevData
{
    struct Library dd_Lib;
    APTR dd_SegList;
    struct Library *dd_SysBase;
    struct Library *dd_OpenPciBase;
    struct Library *dd_UtilityBase;
    struct Library *dd_DOSBase;
    struct Library *dd_TimerBase;
    struct Library *dd_MMUBase;  /* optional, see hw_is_cache_inhibited() */
    struct UnitData *dd_Units[MAX_UNITS];
    struct timerequest dd_Treq;
};

struct UnitData
{
    struct Message ud_Message;
    volatile struct Ne2000 *ud_Hardware;
    struct Library *ud_SysBase;
    struct Library *ud_OpenPciBase;
    struct Library *ud_DOSBase;
    struct Library *ud_TimerBase;
    struct Library *ud_MMUBase;  /* optional, see hw_is_cache_inhibited() */
    struct Interrupt *ud_Interrupt;
    struct Task *ud_Task;
    struct MsgPort *ud_TaskPort;
    struct MinList ud_RxQueue;
    struct SignalSemaphore ud_RxSem;
    struct MinList ud_TxQueue;
    struct SignalSemaphore ud_TxSem;
    struct IOSana2Req *ud_TxReq[2];  /* request held by each of the card's two transmit buffers */
    UWORD ud_TxLen[2];               /* ... and its frame length */
    BYTE ud_TxActive;                /* buffer being transmitted, -1 if none */
    BYTE ud_TxWaiting;               /* buffer uploaded and waiting for the wire, -1 if none */
    ULONG ud_IntStamp, ud_RxStamp;   /* PERF timing */
    UBYTE ud_IntStampSet, ud_RxStampSet;
    struct pci_dev* ud_PciDev;
    ULONG ud_OpenCnt;
    ULONG ud_ServiceMask;
    ULONG ud_RxSpace[386];   /* storage for ud_RxBuffer (1536 bytes + alignment) */
    UWORD *ud_RxBuffer;      /* one received frame, 2 bytes into ud_RxSpace: see FRAME_ALIGN */
                             /* and RXW_* for its word offsets                               */
    UBYTE ud_Name[sizeof(DEV_UNITNAME)];
    UBYTE ud_EtherAddress[ETHER_ADDR_LEN];
    UBYTE ud_SoftAddress[ETHER_ADDR_LEN];
    struct Sana2DeviceStats ud_DevStats;
    ULONG ud_Diag[DG_COUNT];
    UBYTE ud_Flags;
    UBYTE ud_NextPage;
    BYTE ud_ServiceBit;
    UBYTE ud_Unit;       /* unit number, index into dd_Units[] */
    UBYTE ud_IntMasked;  /* card interrupts masked by us; only decides IntCode()'s return value */
    UBYTE ud_RegsUncached;       /* registers mapped cache-inhibited: the barriers do nothing */
};

typedef BOOL (*BuffFunc)(register APTR to reg(a0), register APTR from reg(a1), register ULONG n reg(d0));

struct BuffFunctions
{
    BuffFunc bf_CopyTo;
    BuffFunc bf_CopyFrom;
};

///
/// prototypes


struct DevData *DevInit(register void *seglist reg(a0), register struct Library *sysb reg(a6));
LONG DevOpen(register struct IOSana2Req *req reg(a1), register LONG unit reg(d0), register LONG flags reg(d1),
             register struct DevData *dd reg(a6));
APTR DevClose(register struct IOSana2Req *req reg(a1), register struct DevData *dd reg(a6));
APTR DevExpunge(register struct DevData *dd reg(a6));
LONG DevReserved(void);
void DevBeginIO(register struct IOSana2Req *req reg(a1), register struct DevData *dd reg(a6));
ULONG DevAbortIO(register struct IOSana2Req *req reg(a1), register struct DevData *dd reg(a6));

void IoDone(struct UnitData *ud, struct IOSana2Req *req, LONG err, LONG werr);
LONG OpenDeviceLibraries(struct DevData *dd);
void CloseDeviceLibraries(struct DevData *dd);
LONG PrepareCookie(struct IOSana2Req *req, struct DevData *dd);
LONG RunTask(struct DevData *dd, struct UnitData *ud);
void ClearGlobalStats(struct UnitData *ud);
struct UnitData *OpenUnit(struct DevData *dd, LONG unit, LONG flags);
struct UnitData *InitializeUnit(struct DevData *dd, LONG unit);
void CloseUnit(struct DevData *dd, struct UnitData *ud);
void ExpungeUnit(struct DevData *dd, struct UnitData *ud);

void CmdNSDQuery(struct UnitData *ud, struct IOStdReq *req);
void S2DeviceQuery(struct UnitData *ud, struct IOSana2Req *req);
void S2GetStationAddress(struct UnitData *ud, struct IOSana2Req *req);
void S2ConfigInterface(struct UnitData *ud, struct IOSana2Req *req);
void S2Online(struct UnitData *ud, struct IOSana2Req *req);
void S2Offline(struct UnitData *ud, struct IOSana2Req *req);
void S2GetGlobalStats(struct UnitData *ud, struct IOSana2Req *req);
void S2GetSpecialStats(struct UnitData *ud, struct IOSana2Req *req);

void HardwareReset(struct UnitData *ud);
void HardwareInit(struct UnitData *ud);
volatile struct Ne2000 *FindHardware(struct DevData *dd, WORD unit, struct UnitData *ud);
BOOL GoOnline(struct UnitData *ud);
void GoOffline(struct UnitData *ud);
void TakeOffline(struct UnitData *ud);
void FlushQueues(struct UnitData *ud, LONG err, LONG werr);
void GetHwAddress(struct UnitData *ud);
void WriteHwAddress(struct UnitData *ud);
LONG PacketReceived(struct UnitData *ud);
LONG RingBufferNotEmpty(struct UnitData *ud);
BOOL OverflowBegin(struct UnitData *ud);
void OverflowEnd(struct UnitData *ud, BOOL resend);
void RingReset(struct UnitData *ud);

struct IOSana2Req *SearchReadRequest(struct UnitData *ud, struct MinList *queue, ULONG type);
void UnitTask(void);

///
/// tables and constans

extern struct Resident romtag;
const UBYTE IdString[] = DEV_IDSTRING;

const void *FuncTable[] =
    {
        DevOpen,
        DevClose,
        DevExpunge,
        DevReserved,
        DevBeginIO,
        DevAbortIO,
        (APTR)-1};

UWORD NSDSupported[] =
    {
        CMD_READ,
        CMD_WRITE,
        CMD_FLUSH,
        S2_DEVICEQUERY,
        S2_GETSTATIONADDRESS,
        S2_CONFIGINTERFACE,
        S2_BROADCAST,
        S2_GETGLOBALSTATS,
        S2_GETSPECIALSTATS,
        S2_ONLINE,
        S2_OFFLINE,
        NSCMD_DEVICEQUERY,
        0};

///

/* strcpy() replacement: the driver links without a C library */
unsigned char *StrCopy(unsigned char *dest, const unsigned char *src)
{
    unsigned char *d = dest;
    while (*src)
        *dest++ = *src++;
    *dest = 0;
    return d;
}

/* Write barrier: make register writes reach the card */
static inline void hw_write_barrier(volatile struct Ne2000 *hw, struct UnitData *ud)
{
    struct Library *SysBase;

    /* Nothing to do if the registers are mapped cache-inhibited */
    if (ud->ud_RegsUncached)
        return;

    /* Otherwise flush the data cache over the registers */
    SysBase = ud->ud_SysBase;
    if (SysBase) {
        CacheClearE((APTR)hw, sizeof(struct Ne2000), CACRF_ClearD);
    }
}

/* Read barrier: make register reads come from the card, not the data cache */
static inline void hw_read_barrier(volatile struct Ne2000 *hw, struct UnitData *ud)
{
    struct Library *SysBase;

    /* Nothing to do if the registers are mapped cache-inhibited */
    if (ud->ud_RegsUncached)
        return;

    /* Otherwise flush the data cache over the registers */
    SysBase = ud->ud_SysBase;
    if (SysBase) {
        CacheClearE((APTR)hw, sizeof(struct Ne2000), CACRF_ClearD);
    }
}

/* Finds out whether the card's registers are mapped cache-inhibited, in which case the     */
/* cache barriers above are unnecessary. The PCI bridge software normally maps PCI space     */
/* that way. This only asks mmu.library and never changes the mapping. Without mmu.library  */
/* the barriers stay in use.                                                                 */
static BOOL hw_is_cache_inhibited(struct UnitData *ud)
{
    if (ud->ud_MMUBase)
    {
        struct Library *MMUBase = ud->ud_MMUBase;
        struct TagItem notags[1] = {{TAG_DONE, 0}};
        ULONG first = (ULONG)ud->ud_Hardware;
        ULONG last = first + sizeof(struct Ne2000) - 1;
        ULONG flags;

        /* Hardware level properties of the page(s) holding the registers */
        flags = GetPagePropertiesA(NULL, first, notags) & GetPagePropertiesA(NULL, last, notags);
        INIT_DBG1_U("Card register page properties: $%08lx", flags);

        if (flags & (MAPP_CACHEINHIBIT | MAPP_IO))
        {
            INIT_DBG_U("Card registers are cache-inhibited, no cache flushes needed");
            return TRUE;
        }
        INIT_DBG_U("Card registers are not cache-inhibited, using cache flushes");
    }
    return FALSE;
}

///
/// DevInit()
// Called when the device is loaded into memory. Makes system library, initializes Library structure, opens
// libraries used by the device. Returns device base or NULL if init failed.

struct DevData *DevInit(register APTR seglist reg(a0), register struct Library *sysb reg(a6))
{
    struct DevData *dd;
    struct Library *SysBase = sysb;

    if ((dd = (struct DevData *)MakeLibrary(FuncTable, NULL, NULL, sizeof(struct DevData), 0)))
    {
        UBYTE *p;

        /* MakeLibrary() is not documented to clear the base. Everything after the Library */
        /* structure starts out 0: CloseDeviceLibraries() closes only what was opened.     */
        for (p = (UBYTE *)&dd->dd_SegList; p < (UBYTE *)(dd + 1); p++)
            *p = 0;

        dd->dd_Lib.lib_Node.ln_Type = NT_DEVICE;
        dd->dd_Lib.lib_Node.ln_Name = romtag.rt_Name;
        dd->dd_Lib.lib_Flags = LIBF_CHANGED | LIBF_SUMUSED;
        dd->dd_Lib.lib_Version = DEV_VERSION;
        dd->dd_Lib.lib_Revision = DEV_REVISION;
        dd->dd_Lib.lib_IdString = romtag.rt_IdString;
        dd->dd_Lib.lib_OpenCnt = 0;
        dd->dd_SegList = seglist;
        dd->dd_SysBase = SysBase;
        if (OpenDeviceLibraries(dd))
        {
            INIT_DBG1("Device initialized, base at $%08lx.", dd);
            AddDevice((struct Device *)dd);
            return dd;
        }
        CloseDeviceLibraries(dd);

        /* Failing init code must free the base itself (see MakeLibrary()) */
        FreeMem((UBYTE *)dd - dd->dd_Lib.lib_NegSize, dd->dd_Lib.lib_NegSize + dd->dd_Lib.lib_PosSize);
    }
    return NULL;
}

///
/// DevOpen()

LONG DevOpen(register struct IOSana2Req *req reg(a1), register LONG unit reg(d0), register LONG flags reg(d1),
             register struct DevData *dd reg(a6))
{
    struct UnitData *ud;

    DBG("DevOpen() called.");
    dd->dd_Lib.lib_OpenCnt++; // expunge protection

    if ((unit >= 0) && (unit < MAX_UNITS))
    {
        if ((ud = OpenUnit(dd, unit, flags)))
        {
            req->ios2_Req.io_Error = 0;
            req->ios2_Req.io_Message.mn_Node.ln_Type = NT_REPLYMSG;
            req->ios2_Req.io_Device = (struct Device *)dd;
            req->ios2_Req.io_Unit = (struct Unit *)ud;
            if (flags & OPENF_MONITOR)
            {
                /* Query-only open: no buffer management, see DevBeginIO() */
                req->ios2_BufferManagement = NULL;
                dd->dd_Lib.lib_Flags &= ~LIBF_DELEXP;
                return 0;
            }
            if (PrepareCookie(req, dd))
            {
                dd->dd_Lib.lib_Flags &= ~LIBF_DELEXP;
                DBG("DevOpen(): device opened successfully.");
                return 0;
            }
            DBG("PrepareCookie() failed.");
            CloseUnit(dd, ud);
        }
    }
    req->ios2_Req.io_Error = IOERR_OPENFAIL;
    req->ios2_Req.io_Device = (struct Device *)-1;
    req->ios2_Req.io_Unit = (struct Unit *)-1;
    dd->dd_Lib.lib_OpenCnt--; /* end of expunge protection */
    return IOERR_OPENFAIL;
}

///
/// DevClose()

APTR DevClose(register struct IOSana2Req *req reg(a1), register struct DevData *dd reg(a6))
{
    USE(SysBase)

    CloseUnit(dd, (struct UnitData *)req->ios2_Req.io_Unit);

    if (req->ios2_BufferManagement)
        FreeMem(req->ios2_BufferManagement,
                sizeof(struct BuffFunctions));

    if (--dd->dd_Lib.lib_OpenCnt == 0)
    {
        DBG("DevClose(): open counter reached 0.");
        if (dd->dd_Lib.lib_Flags & LIBF_DELEXP)
            return (DevExpunge(dd));
    }
    return 0;
}

///
/// DevExpunge()

APTR DevExpunge(register struct DevData *dd reg(a6))
{
    USE(SysBase)
    APTR seglist;

    if (dd->dd_Lib.lib_OpenCnt)
    {
        dd->dd_Lib.lib_Flags |= LIBF_DELEXP;
        return 0;
    }
    Remove((struct Node *)dd);
    CloseDeviceLibraries(dd);
    seglist = dd->dd_SegList;
    FreeMem((APTR)dd - dd->dd_Lib.lib_NegSize, (LONG)dd->dd_Lib.lib_PosSize + (LONG)dd->dd_Lib.lib_NegSize);
    DBG("DevExpunge(): expunged.");
    return seglist;
}

///
/// DevReserved()

LONG DevReserved(void)
{
    return 0;
}

///
/// DevBeginIo()

void DevBeginIO(register struct IOSana2Req *req reg(a1), register struct DevData *dd reg(a6))
{
    USE(SysBase)
    struct UnitData *ud = (struct UnitData *)req->ios2_Req.io_Unit;
    WORD i;

    switch (req->ios2_Req.io_Command)
    {
    case NSCMD_DEVICEQUERY:
        CmdNSDQuery(ud, (struct IOStdReq *)req);
        break;
    case S2_DEVICEQUERY:
        S2DeviceQuery(ud, req);
        break;
    case S2_GETSTATIONADDRESS:
        S2GetStationAddress(ud, req);
        break;
    /* These reprogram the card, which only the unit task may touch */
    case S2_CONFIGINTERFACE:
    case S2_ONLINE:
    case S2_OFFLINE:
        req->ios2_Req.io_Flags &= ~IOF_QUICK;
        PutMsg(ud->ud_TaskPort, &req->ios2_Req.io_Message);
        break;
    case S2_GETGLOBALSTATS:
        S2GetGlobalStats(ud, req);
        break;
    case S2_GETSPECIALSTATS:
        S2GetSpecialStats(ud, req);
        break;

    case CMD_READ:
        if (!req->ios2_BufferManagement)
        {
            /* Query-only (OPENF_MONITOR) opener */
            IoDone(ud, req, S2ERR_BAD_STATE, S2WERR_GENERIC_ERROR);
            break;
        }

        /* Queued here rather than sent to the unit task: saves two task switches per read. */
        /* The queue is protected by its semaphore; the unit task takes reads from it.      */
        req->ios2_Req.io_Flags &= ~IOF_QUICK;
        req->ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;   /* see DevAbortIO() */
        DIAG(DG_READSQUEUED)++;
        ObtainSemaphore(&ud->ud_RxSem);
        AddTail((struct List *)&ud->ud_RxQueue, (struct Node *)req);
        ReleaseSemaphore(&ud->ud_RxSem);
        break;

    case S2_BROADCAST:
        for (i = 0; i < ETHER_ADDR_LEN; i++)
            req->ios2_DstAddr[i] = 0xFF;
    case CMD_WRITE:
        if (!req->ios2_BufferManagement)
        {
            /* Query-only (OPENF_MONITOR) opener */
            IoDone(ud, req, S2ERR_BAD_STATE, S2WERR_GENERIC_ERROR);
            break;
        }

        /* Queued here rather than sent to the unit task, which only needs waking if one of */
        /* the card's transmit buffers is free: while both are loaded, the transmit-done     */
        /* interrupt will take the next frame from the queue anyway. That saves two task     */
        /* switches per frame when sending at full speed.                                    */
        /*                                                                                   */
        /* No frame is lost between the two: the unit task frees a buffer before it looks at */
        /* the queue, and the request is queued here before the buffers are checked. So     */
        /* either the unit task finds the request, or this sees the free buffer and wakes it.*/
        /* UF_ONLINE is checked under the semaphore: going offline clears it before flushing */
        /* the queue under the same semaphore.                                               */
        req->ios2_Req.io_Flags &= ~IOF_QUICK;
        req->ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;   /* see DevAbortIO() */
        PERF_SINCE(ud->ud_RxStamp, ud->ud_RxStampSet, DG_MAXTICKSTURN, DG_SLOWTURN, PERF_TICKS_5MS);
        ObtainSemaphore(&ud->ud_TxSem);
        if (!(ud->ud_Flags & UF_ONLINE))
        {
            ReleaseSemaphore(&ud->ud_TxSem);
            IoDone(ud, req, S2ERR_OUTOFSERVICE, S2WERR_UNIT_OFFLINE);   /* the card is stopped */
            break;
        }
        DIAG(DG_WRITESQUEUED)++;
        AddTail((struct List *)&ud->ud_TxQueue, (struct Node *)req);
        ReleaseSemaphore(&ud->ud_TxSem);
        if (!ud->ud_TxReq[0] || !ud->ud_TxReq[1])
            Signal(ud->ud_Task, ud->ud_ServiceMask);
        break;

    case CMD_FLUSH:
        req->ios2_Req.io_Flags &= ~IOF_QUICK;
        PutMsg(ud->ud_TaskPort, &req->ios2_Req.io_Message);
        break;

    /* Unimplemented but valid SANA2 commands - return not supported */
    case S2_ADDMULTICASTADDRESS:
    case S2_DELMULTICASTADDRESS:
    case S2_TRACKTYPE:
    case S2_UNTRACKTYPE:
    case S2_GETTYPESTATS:
    case S2_ONEVENT:
    case S2_READORPHAN:
        IoDone(ud, req, S2ERR_NOT_SUPPORTED, S2WERR_GENERIC_ERROR);
        break;

    default:
        DBG1("DevBeginIo(): unknown command code %ld.", req->ios2_Req.io_Command);
        IoDone(ud, req, IOERR_NOCMD, S2WERR_GENERIC_ERROR);
        break;
    }
    return;
}

///
/// DevAbortIo()

ULONG DevAbortIO(register struct IOSana2Req *req reg(a1), register struct DevData *dd reg(a6))
{
    USE(SysBase)
    LONG ret = 0;
    struct UnitData *ud = (struct UnitData *)req->ios2_Req.io_Unit;
    struct MinList *list;
    struct MinNode *node;
    struct SignalSemaphore *sem = NULL;

    DBG1("DevAbortIo: aborting $%08lx.", req);
    switch (req->ios2_Req.io_Command)
    {
    case CMD_READ:
        list = &ud->ud_RxQueue;
        sem = &ud->ud_RxSem;
        break;

    case CMD_WRITE:
    case S2_BROADCAST:
        list = &ud->ud_TxQueue;
        sem = &ud->ud_TxSem;
        break;

    default:
        list = NULL;
    }
    if (list)
    {
        ObtainSemaphore(sem);
        for (node = list->mlh_Head; node->mln_Succ; node = node->mln_Succ)
        {
            if (node == (struct MinNode *)req)
            {
                if (((struct Message *)node)->mn_Node.ln_Type != NT_REPLYMSG)
                {
                    Remove((struct Node *)node);
                    req->ios2_Req.io_Error = IOERR_ABORTED;
                    ReplyMsg((struct Message *)node);
                    ret = 0;
                    break;
                }
            }
        }
        ReleaseSemaphore(sem);
    }
    else
        ret = IOERR_NOCMD;
    return ret;
}

///

// AUXILIARY FUNCTIONS

/// IoDone(struct UnitData *ud, struct IOSana2Req *req, LONG err, LONG werr)
// Function ends IORequest with given error codes. Requests with IOF_QUICK cleared will be ReplyMsg()-ed.

void IoDone(struct UnitData *ud, struct IOSana2Req *req, LONG err, LONG werr)
{
    USE_U(SysBase)

    req->ios2_Req.io_Error = err;
    req->ios2_WireError = werr;
    if (!(req->ios2_Req.io_Flags & IOF_QUICK))
        ReplyMsg(&req->ios2_Req.io_Message);
    return;
}

///
/// OpenDeviceLibraries(struct DevData *dd)

LONG OpenDeviceLibraries(struct DevData *dd)
{
    USE(SysBase)

    if (!(dd->dd_UtilityBase = OpenLibrary("utility.library", 39)))
        return FALSE;
    if (!(dd->dd_OpenPciBase = OpenLibrary("openpci.library", 5)))
        return FALSE;
    if (!(dd->dd_DOSBase = OpenLibrary("dos.library", 38)))
        return FALSE;
    if (OpenDevice("timer.device", UNIT_MICROHZ, (struct IORequest *)&dd->dd_Treq, 0) == 0)
    {
        dd->dd_TimerBase = (struct Library *)dd->dd_Treq.tr_node.io_Device;
    }
#if PERF_TIMING
    if (!dd->dd_TimerBase)
        return FALSE;   /* the timing counters read the E-clock through it */
#endif

    /* Optional: hw_is_cache_inhibited() asks it how the card's registers are mapped */
    dd->dd_MMUBase = OpenLibrary("mmu.library", 39);
    if (dd->dd_MMUBase)
    {
        INIT_DBG("mmu.library opened");
    }
    else
    {
        INIT_DBG("mmu.library not available - will use cache flush method");
    }

    return TRUE;
}

///
/// CloseDeviceLibraries(struct DevData *dd)

void CloseDeviceLibraries(struct DevData *dd)
{
    USE(SysBase)

    if (dd->dd_DOSBase)
        CloseLibrary(dd->dd_DOSBase);
    if (dd->dd_OpenPciBase)
        CloseLibrary(dd->dd_OpenPciBase);
    if (dd->dd_UtilityBase)
        CloseLibrary(dd->dd_UtilityBase);
    if (dd->dd_TimerBase)
        CloseDevice((struct IORequest *)&dd->dd_Treq);
    if (dd->dd_MMUBase)
        CloseLibrary(dd->dd_MMUBase);
}

///
/// PrepareCookie(struct IOSana2Req *req, struct DevData *dd)

LONG PrepareCookie(struct IOSana2Req *req, struct DevData *dd)
{
    USE(SysBase)
    USE(UtilityBase)

    if (req->ios2_BufferManagement)
    {
        struct BuffFunctions *bfun;

        if ((bfun = AllocMem(sizeof(struct BuffFunctions), MEMF_ANY)))
        {
            bfun->bf_CopyFrom = (APTR)GetTagData(S2_CopyFromBuff, NULL,
                                                 (struct TagItem *)req->ios2_BufferManagement);
            bfun->bf_CopyTo = (APTR)GetTagData(S2_CopyToBuff, NULL,
                                               (struct TagItem *)req->ios2_BufferManagement);

            if (bfun->bf_CopyFrom && bfun->bf_CopyTo)
            {
                DBG1("CopyFrom [$%08lx].", bfun->bf_CopyFrom);
                req->ios2_BufferManagement = bfun;
                return TRUE;
            }
            else
                FreeMem(bfun, sizeof(struct BuffFunctions));
        }
    }
    return FALSE;
}

///
/// RunTask(struct DevData *dd, struct UnitData *ud)

LONG RunTask(struct DevData *dd, struct UnitData *ud)
{
    USE(SysBase)
    USE(DOSBase)

    LONG pri = UNIT_TASK_PRI;
    UBYTE buffer[16];

    DBG("RunTask() called.");

    /* The priority can be overridden for experiments: SetEnv openpci-rtl8029.pri <n> */
    if (GetVar(UNIT_TASK_PRI_VAR, buffer, sizeof(buffer), 0) > 0)
    {
        LONG value;

        if (StrToLong(buffer, &value) > 0 && value >= -20 && value <= 60)
            pri = value;
    }
    DIAG(DG_TASKPRI) = pri;

    if ((ud->ud_Task = (struct Task *)CreateNewProcTags(
            NP_Entry, (LONG)UnitTask,
            NP_Name, (LONG)ud->ud_Name,
            NP_Priority, pri,
            NP_StackSize, 16384, /* UploadPacket() keeps a full frame on the stack */
            TAG_END)))
    {
        WORD i;

        DBG1("Task [$%08lx] started.", ud->ud_Task);
        for (i = 0; i < 50; i++)
        {
            if (!(ud->ud_TaskPort = FindPort(ud->ud_Name)))
                Delay(1);
            else
            {
                /* Hand the unit to its task. The task keeps this message until it exits */
                /* and replies it then; KillTask() supplies the reply port.              */
                DBG("Task port detected.");
                ud->ud_Message.mn_Node.ln_Type = NT_MESSAGE;
                ud->ud_Message.mn_Length = sizeof(struct UnitData);
                ud->ud_Message.mn_ReplyPort = NULL;
                PutMsg(ud->ud_TaskPort, &ud->ud_Message);
                return TRUE;
            }
        }
    }

    /* No task, or one that never opened its port: it holds no reference to the unit, so */
    /* forget it rather than have ExpungeUnit() signal it and wait for it forever.        */
    ud->ud_Task = NULL;
    return FALSE;
}

///
/// KillTask(struct DevData *dd, struct UnitData *ud)

/* Tells the unit task to quit and waits until it has. The task replies the unit message   */
/* as its last action. The reply port is created here, by the task that is closing the     */
/* unit: a message port only wakes the task that created it, and the unit is not           */
/* necessarily closed by the task that opened it (Roadshow's NetShutdown is not).          */

void KillTask(struct DevData *dd, struct UnitData *ud)
{
    USE(SysBase)
    USE(DOSBase)
    struct MsgPort *port;

    port = CreateMsgPort();

    /* If the task could not set itself up it has replied (without a port) and gone already. */
    /* Otherwise it only looks at the reply port when it replies, after it got the signal.   */
    Forbid();
    if (ud->ud_Message.mn_Node.ln_Type == NT_FREEMSG)
    {
        Permit();
        if (port)
            DeleteMsgPort(port);
        return;
    }
    ud->ud_Message.mn_ReplyPort = port;
    Signal(ud->ud_Task, SIGBREAKF_CTRL_C);
    Permit();

    if (port)
    {
        WaitPort(port);
        GetMsg(port);
        DeleteMsgPort(port);
    }
    else
    {
        /* No port to be had: a reply without one just marks the message as free */
        while (ud->ud_Message.mn_Node.ln_Type != NT_FREEMSG)
            Delay(1);
    }
    DBG("Task dead.");
    return;
}

///
/// ClearGlobalStats()

void ClearGlobalStats(struct UnitData *ud)
{
    USE_U(TimerBase)

    ud->ud_DevStats.PacketsReceived = 0;
    ud->ud_DevStats.PacketsSent = 0;
    GetSysTime(&ud->ud_DevStats.LastStart);
    return;
}

///
/// OpenUnit()

struct UnitData *OpenUnit(struct DevData *dd, LONG unit, LONG flags)
{
    struct UnitData *ud = dd->dd_Units[unit];

    DBG("OpenUnit() called.");

    /* Eliminate 'promiscuous without exclusive' flag combination. */

    if ((flags & SANA2OPF_PROM) && !(flags & SANA2OPF_MINE))
        return NULL;

    /* Initialize unit if opened first time. */

    if (!ud)
    {
        if (!(ud = InitializeUnit(dd, unit)))
            return NULL;
    }

    /* Exclusive access: reject an exclusive open of a unit somebody else has open, and any */
    /* open of a unit somebody else holds exclusively.                                      */

    if (!(flags & OPENF_MONITOR) && ud->ud_OpenCnt &&
        ((flags & SANA2OPF_MINE) || (ud->ud_Flags & UF_EXCLUSIVE)))
        return NULL;

    if (flags & SANA2OPF_MINE)
        ud->ud_Flags |= UF_EXCLUSIVE;

    /* Promiscuous implies exclusive (checked above), so nobody else has the unit open */

    if (flags & SANA2OPF_PROM)
        ud->ud_Flags |= UF_PROMISCUOUS;

    ud->ud_OpenCnt++;
    DBG2("%s opened [%ld].", ud->ud_Name, ud->ud_OpenCnt);
    return ud;
}

///
/// CloseUnit()

void CloseUnit(struct DevData *dd, struct UnitData *ud)
{
    DBG1("%s closed.", ud->ud_Name);
    if (!(--ud->ud_OpenCnt))
        ExpungeUnit(dd, ud);
    return;
}

///
/// InitializeUnit()

struct UnitData *InitializeUnit(struct DevData *dd, LONG unit)
{
    USE(SysBase)
    struct UnitData *ud;
    WORD i;

    DBG("InitializeUnit() called.");
    if ((ud = AllocMem(sizeof(struct UnitData), MEMF_PUBLIC | MEMF_CLEAR)))
    {
        ud->ud_Unit = unit;
        if ((ud->ud_Hardware = FindHardware(dd, unit, ud)))
        {
            for (i = 5; i >= 0; i--)
            {
                ud->ud_SoftAddress[i] = 0x00;
                ud->ud_EtherAddress[i] = 0x00;
            }
            ud->ud_SysBase = dd->dd_SysBase;
            ud->ud_OpenPciBase = dd->dd_OpenPciBase;
            ud->ud_DOSBase = dd->dd_DOSBase;
            ud->ud_TimerBase = dd->dd_TimerBase;
            ud->ud_MMUBase = dd->dd_MMUBase;
            StrCopy(ud->ud_Name, DEV_UNITNAME);
            ud->ud_Name[UNIT_DIGIT] = '0' + unit;
            ud->ud_RxQueue.mlh_Head = (struct MinNode *)&ud->ud_RxQueue.mlh_Tail;
            ud->ud_RxQueue.mlh_Tail = NULL;
            ud->ud_RxQueue.mlh_TailPred = (struct MinNode *)&ud->ud_RxQueue.mlh_Head;
            InitSemaphore(&ud->ud_RxSem);


            ud->ud_TxQueue.mlh_Head = (struct MinNode *)&ud->ud_TxQueue.mlh_Tail;
            ud->ud_TxQueue.mlh_Tail = NULL;
            ud->ud_TxQueue.mlh_TailPred = (struct MinNode *)&ud->ud_TxQueue.mlh_Head;
            InitSemaphore(&ud->ud_TxSem);


            ud->ud_NextPage = RX_BUFFER;
            ud->ud_TxActive = ud->ud_TxWaiting = -1;
            ud->ud_RxBuffer = (UWORD *)FRAME_START(ud->ud_RxSpace);

            /* Cache barriers are only needed if the registers are not cache-inhibited already */
            ud->ud_RegsUncached = hw_is_cache_inhibited(ud);

            HardwareReset(ud);
            HardwareInit(ud);
            GetHwAddress(ud);
            if (RunTask(dd, ud))
            {
                dd->dd_Units[unit] = ud;
                INIT_DBG1_U("%s initialized.", (LONG)ud->ud_Name);
                return ud;
            }
        }
    }
    ExpungeUnit(dd, ud);
    return NULL;
}

///
/// ExpungeUnit()

void ExpungeUnit(struct DevData *dd, struct UnitData *ud)
{
    USE(SysBase)

    if (ud)
    {
        /* The unit task takes the card offline and returns all requests before it exits */
        if (ud->ud_Task)
            KillTask(dd, ud);
        DBG1("Unit %ld expunged.", ud->ud_Unit);

        /* The unit may not have got as far as being registered (or even named) */
        if (dd->dd_Units[ud->ud_Unit] == ud)
            dd->dd_Units[ud->ud_Unit] = NULL;
        FreeMem(ud, sizeof(struct UnitData));
    }
    return;
}

///

// IMMEDIATE DEVICE COMMANDS

/// S2DeviceQuery()

void S2DeviceQuery(struct UnitData *ud, struct IOSana2Req *req)
{
    struct Sana2DeviceQuery *query = req->ios2_StatData;

    DBG_U("S2_DEVICEQUERY.");
    if (query)
    {
        query->DevQueryFormat = 0;
        query->DeviceLevel = 0;

        if(query->SizeAvailable >= 18)
            query->AddrFieldSize = ETHER_ADDR_LEN * 8;

        if (query->SizeAvailable >= 22)
            query->MTU = ETHERMTU;

        if (query->SizeAvailable >= 26)
            query->BPS = 10000000;

        if (query->SizeAvailable >= 30)
            query->HardwareType = S2WireType_Ethernet;

        // Report 30 bytes of data.
        query->SizeSupplied = query->SizeAvailable < 30 ? query->SizeAvailable : 30;
        IoDone(ud, req, S2ERR_NO_ERROR, S2WERR_GENERIC_ERROR);
    }
    else
    {
        DBG_U("S2_DEVICEQUERY: Error, bad argument (NULL pointer).");
        IoDone(ud, req, S2ERR_BAD_ARGUMENT, S2WERR_NULL_POINTER);
    }
    return;
}

///
/// S2GetStationAddress()

void S2GetStationAddress(struct UnitData *ud, struct IOSana2Req *req)
{
    USE_U(SysBase)

    DBG_U("S2_GETSTATIONADDRESS.");
    CopyMem(ud->ud_SoftAddress, req->ios2_SrcAddr, ETHER_ADDR_LEN);
    CopyMem(ud->ud_EtherAddress, req->ios2_DstAddr, ETHER_ADDR_LEN);
    IoDone(ud, req, S2ERR_NO_ERROR, S2WERR_GENERIC_ERROR);
    return;
}

///
/// S2Online()

/* S2_ONLINE, S2_CONFIGINTERFACE and S2_OFFLINE are executed by the unit task (see         */
/* MainLoop()), never in the caller's context: they reprogram the card.                    */

void S2Online(struct UnitData *ud, struct IOSana2Req *req)
{
    DBG_U("S2_ONLINE.");
    if (!(ud->ud_Flags & UF_CONFIGURED))
        IoDone(ud, req, S2ERR_BAD_STATE, S2WERR_NOT_CONFIGURED);
    else if (ud->ud_Flags & UF_ONLINE)
        IoDone(ud, req, S2ERR_BAD_STATE, S2WERR_UNIT_ONLINE);
    else if (GoOnline(ud))
    {
        ClearGlobalStats(ud);
        ud->ud_Flags |= UF_ONLINE;
        IoDone(ud, req, S2ERR_NO_ERROR, S2WERR_GENERIC_ERROR);
    }
    else
        IoDone(ud, req, S2ERR_NO_RESOURCES, S2WERR_GENERIC_ERROR);
    return;
}

///
/// S2ConfigInterface()

BOOL address_has_all(UBYTE *addr, UBYTE num)
{
    WORD i;

    for (i = 5; i >= 0; i--)
    {
        if (addr[i] != num)
            return FALSE;
    }
    return TRUE;
}

void S2ConfigInterface(struct UnitData *ud, struct IOSana2Req *req)
{
    USE_U(SysBase)

    DBG_U("S2_CONFIGINTERFACE.");
    ClearGlobalStats(ud);
    if (ud->ud_Flags & UF_CONFIGURED)
    {
        IoDone(ud, req, S2ERR_BAD_STATE, S2WERR_IS_CONFIGURED);
    }
    else if (address_has_all(req->ios2_SrcAddr, 0x00) ||
             address_has_all(req->ios2_SrcAddr, 0xFF))
    {
        IoDone(ud, req, S2ERR_BAD_ADDRESS, S2WERR_SRC_ADDRESS);
    }
    else
    {
        /* Not configured means not online either: S2_ONLINE requires configuration.     */
        /* GoOnline() resets the card and programs the new address.                      */
        CopyMem(req->ios2_SrcAddr, ud->ud_SoftAddress, ETHER_ADDR_LEN);
        if (GoOnline(ud))
        {
            ud->ud_Flags |= UF_ONLINE | UF_CONFIGURED;
            IoDone(ud, req, S2ERR_NO_ERROR, S2WERR_GENERIC_ERROR);
        }
        else
            IoDone(ud, req, S2ERR_NO_RESOURCES, S2WERR_GENERIC_ERROR);
    }
    return;
}

///
/// S2Offline()

void S2Offline(struct UnitData *ud, struct IOSana2Req *req)
{
    DBG_U("S2_OFFLINE.");
    if (ud->ud_Flags & UF_ONLINE)
    {
        TakeOffline(ud);
        IoDone(ud, req, S2ERR_NO_ERROR, S2WERR_GENERIC_ERROR);
    }
    else
        IoDone(ud, req, S2ERR_BAD_STATE, S2WERR_UNIT_OFFLINE);
    return;
}

///
/// S2GetGlobalStats()

void S2GetGlobalStats(struct UnitData *ud, struct IOSana2Req *req)
{
    USE_U(SysBase)

    if (req->ios2_StatData)
    {
        CopyMem(&ud->ud_DevStats, req->ios2_StatData, sizeof(struct Sana2DeviceStats));
        IoDone(ud, req, S2ERR_NO_ERROR, S2WERR_GENERIC_ERROR);
    }
    else
        IoDone(ud, req, S2ERR_BAD_ARGUMENT, S2WERR_NULL_POINTER);
}

///
/// S2GetSpecialStats()

void S2GetSpecialStats(struct UnitData *ud, struct IOSana2Req *req)
{
    struct Sana2SpecialStatHeader *hdr = (struct Sana2SpecialStatHeader *)req->ios2_StatData;
    struct Sana2SpecialStatRecord *rec;
    ULONG i;

    if (!hdr)
    {
        IoDone(ud, req, S2ERR_BAD_ARGUMENT, S2WERR_NULL_POINTER);
        return;
    }

    DIAG(DG_TXPENDING) = (ud->ud_TxReq[0] ? 1 : 0) + (ud->ud_TxReq[1] ? 1 : 0);
    DIAG(DG_NEXTPAGE) = ud->ud_NextPage;
    DIAG(DG_MMUINHIBIT) = ud->ud_RegsUncached ? 1 : 0;
#if PERF_TIMING
    {
        USE_U(TimerBase)
        struct EClockVal now;

        DIAG(DG_ECLOCKFREQ) = ReadEClock(&now);
    }
#endif

    rec = (struct Sana2SpecialStatRecord *)(hdr + 1);
    for (i = 0; i < DG_COUNT && i < hdr->RecordCountMax; i++)
    {
        rec[i].Type = (S2WireType_Ethernet << 16) | i;
        rec[i].Count = DIAG(i);
        rec[i].String = (STRPTR)DiagNames[i];
    }
    hdr->RecordCountSupplied = i;
    IoDone(ud, req, S2ERR_NO_ERROR, S2WERR_GENERIC_ERROR);
}

///
/// CmdNSDQuery()

void CmdNSDQuery(struct UnitData *ud, struct IOStdReq *req)
{
    USE_U(SysBase)
    struct NSDeviceQueryResult *qdata;
    LONG error = 0;

    DBG_U("NSCMD_DEVICEQUERY.");
    if (req->io_Length >= sizeof(struct NSDeviceQueryResult))
    {
        if ((qdata = (struct NSDeviceQueryResult *)req->io_Data))
        {
            if ((qdata->nsdqr_DevQueryFormat == 0) && (qdata->nsdqr_SizeAvailable == 0))
            {
                qdata->nsdqr_SizeAvailable = sizeof(struct NSDeviceQueryResult);
                qdata->nsdqr_DeviceType = NSDEVTYPE_SANA2;
                qdata->nsdqr_DeviceSubType = 0;
                qdata->nsdqr_SupportedCommands = NSDSupported;
                req->io_Actual = sizeof(struct NSDeviceQueryResult);
            }
            else
                error = IOERR_BADLENGTH;
        }
        else
            error = IOERR_BADADDRESS;
    }
    else
        error = IOERR_BADLENGTH;

    /* Not IoDone(): it writes ios2_WireError, and this may be a plain IOStdReq */

    req->io_Error = error;
    if (!(req->io_Flags & IOF_QUICK))
        ReplyMsg(&req->io_Message);
    return;
}

///

// HARDWARE ACCESS FUNCTIONS

/// IntCode()

/* Interrupt server. It does no packet work: it only masks the card's interrupts (which   */
/* releases the PCI interrupt line) and signals the unit task. ServiceCard() in the unit   */
/* task handles the events and unmasks again. All other access to the card happens in the  */
/* unit task, so nothing needs Disable() and no semaphores are used in interrupt context.  */
/*                                                                                         */
/* The interrupt chain is shared, so this is also called for other devices' interrupts,    */
/* possibly while the unit task has register page 1 selected. The unit task only selects   */
/* page 1 with the card's interrupts masked, so in that case the interrupt is not ours.    */

LONG IntCode(register struct UnitData *ud reg(a1))
{
    USE_U(SysBase)
    volatile struct Ne2000 *hw = ud->ud_Hardware;
    UBYTE was_masked;

    /* Ensure we read fresh values from hardware, not cached ones */
    hw_read_barrier(hw, ud);

    if (hw->regs[NE2000_COMMAND] & (COMMAND_PAGE1 | COMMAND_PAGE2))
        return 0;

    if (!(hw->regs[NE2000_INT_STATUS] & INTMASK))
        return 0;

    /* Always mask and signal when something is pending, so the line is released whatever  */
    /* the flag says. The flag only decides whether to claim the interrupt: when the card   */
    /* was already masked it cannot have raised this one, so let the other servers on the   */
    /* shared chain have it.                                                                */
    was_masked = ud->ud_IntMasked;
    hw->regs[NE2000_INT_MASK] = 0;
    hw_write_barrier(hw, ud);
    ud->ud_IntMasked = TRUE;
    Signal(ud->ud_Task, ud->ud_ServiceMask);
    if (was_masked)
        return 0;
    DIAG(DG_INTERRUPTS)++;
    PERF_STAMP(ud->ud_IntStamp, ud->ud_IntStampSet);
    return 1;
}

///
/// InstallInterrupt()

LONG InstallInterrupt(struct UnitData *ud)
{
    USE_U(SysBase)
    USE_U(OpenPciBase);
    struct Interrupt *intr;

    if ((intr = AllocMem(sizeof(struct Interrupt), MEMF_PUBLIC | MEMF_CLEAR)))
    {
        intr->is_Node.ln_Type = NT_INTERRUPT;
        intr->is_Node.ln_Name = ud->ud_Name;
        intr->is_Data = ud;
        intr->is_Code = (APTR)IntCode;
        if (pci_add_intserver(intr, ud->ud_PciDev))
        {
            ud->ud_Interrupt = intr;
            return TRUE;
        }
        FreeMem(intr, sizeof(struct Interrupt));
    }
    return FALSE;
}

///
/// RemoveInterrupt()

void RemoveInterrupt(struct UnitData *ud)
{
    USE_U(SysBase)
    USE_U(OpenPciBase)

    if (ud->ud_Interrupt)
    {
        pci_rem_intserver(ud->ud_Interrupt, ud->ud_PciDev);
        FreeMem(ud->ud_Interrupt, sizeof(struct Interrupt));
        ud->ud_Interrupt = NULL;
    }
    return;
}

///
/// FindHardware()

volatile struct Ne2000 *FindHardware(struct DevData *dd, WORD unit, struct UnitData *ud)
{
    USE(OpenPciBase)
    WORD u = unit;
    struct pci_dev *board = NULL;
    struct Ne2000 *hwbase;
    struct TagItem tags[3] = {
        {0, 0},
        {0, 0},
        {TAG_END, 0}};

    while (u-- >= 0)
    {
        tags[0].ti_Tag = PRM_Vendor;
        tags[0].ti_Data = PCI_VENDOR_REALTEK;
        tags[1].ti_Tag = PRM_Device;
        tags[1].ti_Data = PCI_DEVICE_RTL8029;
        tags[2].ti_Tag = TAG_END;
        tags[2].ti_Data = 0;
        board = FindBoardA(board, (struct TagItem *)tags);

        if (!board)
            break;
    }

    if (board)
    {
        ud->ud_PciDev = board;
        tags[0].ti_Tag = PRM_MemoryAddr0;
        tags[0].ti_Data = (LONG)&hwbase;
        tags[1].ti_Tag = TAG_END;
        tags[1].ti_Data = 0;
        GetBoardAttrsA(board, (struct TagItem *)tags);

        tags[0].ti_Tag = PRM_BoardOwner;
        tags[0].ti_Data = (LONG)dd;
        tags[1].ti_Tag = TAG_END;
        tags[1].ti_Data = 0;
        SetBoardAttrsA(board, (struct TagItem *)tags);
        return hwbase;
    }
    return NULL;
}

///
/// HardwareInit()

void HardwareInit(struct UnitData *ud)
{
    volatile struct Ne2000 *hw = ud->ud_Hardware;

    hw->regs[NE2000_COMMAND] = COMMAND_PAGE0 | COMMAND_ABORT | COMMAND_STOP;
    hw->regs[NE2000_DATA_CONFIG] = DTCFG_FIFO_8 | DTCFG_WIDE | DTCFG_LOOPSEL;
    hw->regs[NE2000_DMA_COUNTER0] = 0;
    hw->regs[NE2000_DMA_COUNTER1] = 0;
    hw->regs[NE2000_TX_CONFIG] = 0x02;
    hw->regs[NE2000_PAGE_START] = RX_BUFFER;
    hw->regs[NE2000_PAGE_STOP] = BUFFER_END;
    hw->regs[NE2000_BOUNDARY] = BUFFER_END - 1;
    hw->regs[NE2000_INT_STATUS] = 0xFF;
    hw->regs[NE2000_INT_MASK] = 0x00;
    hw->regs[NE2000_COMMAND] = COMMAND_PAGE1 | COMMAND_ABORT | COMMAND_STOP;
    hw->regs[NE2000_CURRENT_PAGE] = RX_BUFFER;
    hw->regs[NE2000_COMMAND] = COMMAND_PAGE0 | COMMAND_ABORT | COMMAND_STOP;
    hw_write_barrier(hw, ud);  /* Ensure hardware init registers reach hardware */
    return;
}

///
/// HardwareReset()

void HardwareReset(struct UnitData *ud)
{
    USE_U(DOSBase)
    volatile struct Ne2000 *hw = ud->ud_Hardware;
    UBYTE trash = 0b10100101;

    hw->ResetPort = trash;
    Delay(1);
    trash = hw->ResetPort;
    HardwareInit(ud);
    return;
}

///
/// GoOnline()

/* Unit task only. Returns FALSE if the interrupt server could not be installed; the card  */
/* is left stopped then.                                                                   */

BOOL GoOnline(struct UnitData *ud)
{
    volatile struct Ne2000 *hw = ud->ud_Hardware;

    HardwareReset(ud);
    WriteHwAddress(ud);

    /* The reset put the card's ring pointers back to the start: follow them */
    ud->ud_NextPage = RX_BUFFER;
    ud->ud_TxActive = ud->ud_TxWaiting = -1;

    /* For diagnostics: the RTL8029's own configuration (media type, duplex). The card's   */
    /* interrupts are masked here, so another register page may be selected.               */
    hw->regs[NE2000_COMMAND] = COMMAND_PAGE3 | COMMAND_ABORT | COMMAND_STOP;
    hw_write_barrier(hw, ud);
    hw_read_barrier(hw, ud);
    DIAG(DG_RTLCONFIG) = ((ULONG)hw->regs[RTL8029_CONFIG0] << 16) |
                         ((ULONG)hw->regs[RTL8029_CONFIG2] << 8) |
                         hw->regs[RTL8029_CONFIG3];
    hw->regs[NE2000_COMMAND] = COMMAND_PAGE0 | COMMAND_ABORT | COMMAND_STOP;
    hw_write_barrier(hw, ud);

    if (!InstallInterrupt(ud))
        return FALSE;

    hw->regs[NE2000_COMMAND] = 0x22;
    hw->regs[NE2000_TX_CONFIG] = 0x00;
    hw->regs[NE2000_RX_CONFIG] = RXCFG_BCAST | RXCFG_MCAST |
                                 ((ud->ud_Flags & UF_PROMISCUOUS) ? RXCFG_PROM : 0);
    hw->regs[NE2000_INT_STATUS] = 0xFF;
    ud->ud_IntMasked = FALSE;
    hw->regs[NE2000_INT_MASK] = INTMASK;
    hw_write_barrier(hw, ud);  /* Ensure interrupt mask reaches hardware */

    return TRUE;
}

///
/// GoOffline()

void GoOffline(struct UnitData *ud)
{
    volatile struct Ne2000 *hw = ud->ud_Hardware;

    hw->regs[NE2000_COMMAND] = 0x21;
    hw->regs[NE2000_TX_CONFIG] = 0x02;
    hw->regs[NE2000_RX_CONFIG] = 0x20;
    hw->regs[NE2000_INT_STATUS] = 0xFF;
    hw->regs[NE2000_INT_MASK] = 0;
    hw_write_barrier(hw, ud);
    RemoveInterrupt(ud);
    return;
}

///
/// BoardShutdown()

void BoardShutdown(struct UnitData *ud)
{
    volatile struct Ne2000 *hw = ud->ud_Hardware;

    GoOffline(ud);
    hw->regs[NE2000_INT_MASK] = 0;
    hw->regs[NE2000_INT_STATUS] = 0xFF;
    return;
}

///
/// GetPacketHeader()

ULONG GetPacketHeader(volatile struct Ne2000 *ne, UBYTE page, struct UnitData *ud)
{
    UWORD w0, w1;
    UBYTE status, next_page, len_lo, len_hi;
    ULONG result;

    ne->regs[NE2000_DMA_COUNTER0] = 4;
    ne->regs[NE2000_DMA_COUNTER1] = 0;
    ne->regs[NE2000_DMA_START_ADDR0] = 0;
    ne->regs[NE2000_DMA_START_ADDR1] = page;
    ne->regs[NE2000_COMMAND] = COMMAND_PAGE0 | COMMAND_START | COMMAND_READ;

    /* Minimal barrier: ensure DMA command reaches hardware */
    hw_write_barrier(ne, ud);

    /* The 4-byte ring header: receive status, next page, byte count low, byte count high. */
    /* Each 16-bit word read from DMAPort has the card's first byte in its high half.      */
    w0 = ne->DMAPort;               /* status, next page */
    w1 = ne->DMAPort;               /* count low, count high */

    status = (w0 >> 8) & 0xFF;
    next_page = w0 & 0xFF;
    len_lo = (w1 >> 8) & 0xFF;
    len_hi = w1 & 0xFF;

    /* Returned as status | next page << 8 | count low << 16 | count high << 24 */
    result = ((ULONG)status) | ((ULONG)next_page << 8) | ((ULONG)len_lo << 16) | ((ULONG)len_hi << 24);

    return result;
}

///
/// GetPacket()

void GetPacket(volatile struct Ne2000 *ne, UBYTE startpage, UWORD len, UWORD *buffer, BOOL print, struct UnitData *ud)
{
    UWORD count;

    /* The data port is 16 bits wide, so the remote DMA count must be even: with an odd    */
    /* count the chip's byte counter steps past zero and the transfer never completes.     */
    /* Reading one byte more from the ring is harmless.                                    */
    count = (len + 1) & ~1;
    ne->regs[NE2000_DMA_COUNTER0] = count & 0xFF;
    ne->regs[NE2000_DMA_COUNTER1] = count >> 8;
    ne->regs[NE2000_DMA_START_ADDR0] = 4;
    ne->regs[NE2000_DMA_START_ADDR1] = startpage;
    ne->regs[NE2000_COMMAND] = COMMAND_PAGE0 | COMMAND_START | COMMAND_READ;

    /* Minimal barrier: ensure DMA command reaches hardware */
    hw_write_barrier(ne, ud);

#if USE_LONG_DMA
    {
        ULONG *lbuffer;
        UWORD longs;

        /* One word first if the buffer is not longword aligned, then aligned longwords */
        if ((ULONG)buffer & 2)
        {
            *buffer++ = ne->DMAPort;
            count -= 2;
        }
        lbuffer = (ULONG *)buffer;
        for (longs = count >> 2; longs; longs--)
            *lbuffer++ = DMAPORT_LONG(ne);
        if (count & 2)
            *(UWORD *)lbuffer = ne->DMAPort;
    }
#else
    for (count >>= 1; count; count--)
        *buffer++ = ne->DMAPort;
#endif
    return;
}

///
/// PacketReceived()

/* Reads the next packet from the ring into ud_RxBuffer. Returns the frame length (Ethernet */
/* header + data, without the CRC), or 0 if the ring header is corrupt (nothing is read      */
/* then; the caller must reset the ring).                                                    */

/* The byte count in the ring header is 4 more than the Ethernet frame (header + data).    */
/* They must not be passed on: a full-sized frame would exceed the MTU and the TCP/IP stack */
/* would drop it.                                                                            */
#define RING_OVERHEAD   4

/* Diagnostic: verify the IPv4 header checksum and the ICMP/UDP/TCP checksum of a received */
/* frame, to tell whether the data read from the card arrived intact. The card has already */
/* checked the frame's CRC on the wire, so a failure here means the copy out of the card's  */
/* ring is damaged. It costs time per packet, so it is off unless needed: set to 1 to       */
/* enable (RxSumChecked, RxSumBad, RxSumBadWrap, LastSumBad and RxEchoReq stay 0 otherwise). */
#define RX_VERIFY_CHECKSUMS 0

#if RX_VERIFY_CHECKSUMS
static ULONG sum_words(const UWORD *p, ULONG bytes, ULONG sum)
{
    for (; bytes > 1; bytes -= 2)
        sum += *p++;
    if (bytes)
        sum += *p & 0xFF00;
    return sum;
}

static UWORD fold_sum(ULONG sum)
{
    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += sum >> 16;
    return (UWORD)sum;
}

static void VerifyChecksums(struct UnitData *ud, ULONG len, UBYTE startpage, BOOL wrapped)
{
    const UWORD *ip = &ud->ud_RxBuffer[RXW_DATA];
    ULONG ihl, total, tlen, sum;
    UBYTE proto;
    BOOL bad;

    if (ud->ud_RxBuffer[RXW_TYPE] != ETHERTYPE_IP || len < ETHER_HDR_LEN + IP_MIN_HDR_LEN || (ip[0] >> 12) != 4)
        return;

    ihl = ((ip[0] >> 8) & 0x0F) * 4;
    total = ip[1];
    if (ihl < IP_MIN_HDR_LEN || total < ihl || total > len - ETHER_HDR_LEN)
        return;

    DIAG(DG_RXSUMCHECKED)++;
    bad = fold_sum(sum_words(ip, ihl, 0)) != 0xFFFF;

    /* Transport checksum, unless this is a fragment */
    proto = ip[4] & 0xFF;
    tlen = total - ihl;
    if (!bad && !(ip[3] & 0x3FFF))
    {
        const UWORD *tp = ip + ihl / 2;

        if (proto == IPPROTO_ICMP)
        {
            bad = fold_sum(sum_words(tp, tlen, 0)) != 0xFFFF;
            if (!bad && (tp[0] >> 8) == ICMP_ECHO)
                DIAG(DG_RXECHOREQ)++;
        }
        else if (proto == IPPROTO_TCP || (proto == IPPROTO_UDP && tlen >= 8 && tp[3] != 0))   /* UDP: if it has a checksum */
        {
            sum = sum_words(&ip[6], 8, proto + tlen);          /* pseudo header: addresses, protocol, length */
            bad = fold_sum(sum_words(tp, tlen, sum)) != 0xFFFF;
        }
    }

    if (bad)
    {
        DIAG(DG_RXSUMBAD)++;
        if (wrapped)
            DIAG(DG_RXSUMBADWRAP)++;
        DIAG(DG_LASTSUMBAD) = ((ULONG)startpage << 16) | len;
    }
}
#endif
#define RING_COUNT_MIN  (ETHER_MIN_FRAME + RING_OVERHEAD)    /* shortest frame the card accepts */
#define RING_COUNT_MAX  (ETHER_MAX_FRAME + RING_OVERHEAD)    /* longest Ethernet frame */

LONG PacketReceived(struct UnitData *ud)
{
    ULONG header, count, len;
    UBYTE next_page;

    /* Header layout from GetPacketHeader: status, next page, count low, count high */
    header = GetPacketHeader(ud->ud_Hardware, ud->ud_NextPage, ud);
    next_page = (header >> 8) & 0xFF;
    count = ((header >> 16) & 0xFF) | (((header >> 24) & 0xFF) << 8);

    /* Checked before reading: the count is used as the DMA count into ud_RxBuffer, which */
    /* only holds one full-sized frame.                                                  */
    if (next_page < RX_BUFFER || next_page >= BUFFER_END || count < RING_COUNT_MIN || count > RING_COUNT_MAX)
    {
        DBG1_U("Corrupt ring header $%08lx, resetting ring buffer", header);
        DIAG(DG_RXBADHEADER)++;
        DIAG(DG_LASTBADHEADER) = header;
        return 0;
    }

    len = count - RING_OVERHEAD;
    GetPacket(ud->ud_Hardware, ud->ud_NextPage, len, (UWORD *)ud->ud_RxBuffer, FALSE, ud);

    /* A frame whose next-packet pointer is not ahead of its start went round the ring end */
    if (next_page <= ud->ud_NextPage)
        DIAG(DG_RXWRAPPED)++;
#if RX_VERIFY_CHECKSUMS
    VerifyChecksums(ud, len, ud->ud_NextPage, next_page <= ud->ud_NextPage);
#endif
    ud->ud_NextPage = next_page;
    ud->ud_Hardware->regs[NE2000_BOUNDARY] = ud->ud_NextPage == RX_BUFFER ? BUFFER_END - 1 : ud->ud_NextPage - 1;
    hw_write_barrier(ud->ud_Hardware, ud);
    return len;
}

///
/// BufferOverflow()

/* Recovery from a full receive ring, following the procedure the chip requires: stop it,  */
/* let it finish, restart it in loopback, take the buffered packets out, then return to    */
/* normal mode. The packets already in the ring are good and are kept - only what arrives  */
/* while the ring is full is lost. Unit task only, with the card's interrupts masked.      */
/*                                                                                         */
/* OverflowBegin() does the first half and returns TRUE if a transmit was cut off and must */
/* be started again by OverflowEnd(). In between the caller drains the ring.               */

BOOL OverflowBegin(struct UnitData *ud)
{
    volatile struct Ne2000 *hw = ud->ud_Hardware;
    BOOL resend = FALSE;
    UBYTE txp;
    UWORD wait;

    DBG_U("Overflow detected.");
    DIAG(DG_TASKSTATE) = TS_OVERFLOW;

    txp = hw->regs[NE2000_COMMAND] & COMMAND_TXP;
    hw->regs[NE2000_COMMAND] = COMMAND_PAGE0 | COMMAND_ABORT | COMMAND_STOP;
    hw_write_barrier(hw, ud);

    /* The chip needs up to 1.6 ms to finish a transfer in progress. A Delay() would be at */
    /* least 20 ms, during which every incoming packet is lost; register reads take about  */
    /* 0.6 microseconds each on the bus, so wait with those.                               */
    for (wait = 3500; wait; wait--)
        (void)hw->regs[NE2000_INT_STATUS];

    hw->regs[NE2000_DMA_COUNTER0] = 0;
    hw->regs[NE2000_DMA_COUNTER1] = 0;

    /* A transmit that was under way and has not reported completion was cut off */
    if (txp && !(hw->regs[NE2000_INT_STATUS] & (INT_TXPACKET | INT_TXERROR)))
        resend = TRUE;

    hw->regs[NE2000_TX_CONFIG] = TXCFG_LOOP_INT;
    hw->regs[NE2000_COMMAND] = COMMAND_PAGE0 | COMMAND_ABORT | COMMAND_START;
    hw_write_barrier(hw, ud);
    return resend;
}

void OverflowEnd(struct UnitData *ud, BOOL resend)
{
    volatile struct Ne2000 *hw = ud->ud_Hardware;

    hw->regs[NE2000_INT_STATUS] = INT_OVERFLOW;
    hw->regs[NE2000_TX_CONFIG] = TXCFG_LOOP_NONE;
    if (resend)
        hw->regs[NE2000_COMMAND] = COMMAND_PAGE0 | COMMAND_START | COMMAND_ABORT | COMMAND_TXP;
    hw_write_barrier(hw, ud);
}

/* Throws away the ring's contents after a corrupt ring header: the packets in it cannot   */
/* be found any more. Call between OverflowBegin() and OverflowEnd().                      */

void RingReset(struct UnitData *ud)
{
    volatile struct Ne2000 *hw = ud->ud_Hardware;

    hw->regs[NE2000_COMMAND] = COMMAND_PAGE1 | COMMAND_ABORT | COMMAND_START;
    hw->regs[NE2000_CURRENT_PAGE] = RX_BUFFER;
    hw->regs[NE2000_COMMAND] = COMMAND_PAGE0 | COMMAND_ABORT | COMMAND_START;
    hw->regs[NE2000_BOUNDARY] = BUFFER_END - 1;
    hw_write_barrier(hw, ud);
    ud->ud_NextPage = RX_BUFFER;
}

///
/// UploadPacket()

/* Uploads a packet into one of the card's two transmit buffers. This may be done while   */
/* the other buffer is being transmitted. Returns FALSE if the request was rejected        */
/* instead (it has been replied with an error then).                                       */

BOOL UploadPacket(struct UnitData *ud, struct IOSana2Req *req, WORD slot)
{
    USE_U(SysBase)
    volatile struct Ne2000 *hw = ud->ud_Hardware;
    ULONG ethspace[386];                                  /* one frame, see FRAME_ALIGN */
    UBYTE *ethbuffer = FRAME_START(ethspace), *datapointer;
    ULONG data_len = req->ios2_DataLength, dma_len;
    UWORD cycles;
    PERF_VARS;

    PERF_START();

    /* ethbuffer holds one frame: refuse anything longer than the MTU (plus the Ethernet    */
    /* header for raw packets, which carry their own).                                      */

    if (data_len > ((req->ios2_Req.io_Flags & SANA2IOF_RAW) ? ETHER_MAX_FRAME : ETHERMTU))
    {
        IoDone(ud, req, S2ERR_MTU_EXCEEDED, S2WERR_GENERIC_ERROR);
        return FALSE;
    }

    /* A raw packet brings its own Ethernet header; otherwise build it, and the stack's data */
    /* goes after it.                                                                         */

    if (!(req->ios2_Req.io_Flags & SANA2IOF_RAW))
    {
        struct EthFrame *ef = (struct EthFrame *)ethbuffer;

        CopyMem(req->ios2_DstAddr, ef->ef_DestAddr, ETHER_ADDR_LEN);
        CopyMem(ud->ud_SoftAddress, ef->ef_SrcAddr, ETHER_ADDR_LEN);  /* the configured address, as the card filters on */
        ef->ef_Type = req->ios2_PacketType;
        datapointer = ef->ef_Data;
    }
    else
        datapointer = ethbuffer;

    if (!((struct BuffFunctions *)req->ios2_BufferManagement)->bf_CopyFrom(datapointer, req->ios2_Data, data_len))
    {
        IoDone(ud, req, S2ERR_NO_RESOURCES, S2WERR_BUFF_ERROR);
        return FALSE;
    }
    PERF_LAP2(DG_TICKSTXCOPY, DG_TICKSTX);

    /* ios2_DataLength does not count the header unless the packet is raw */

    if (!(req->ios2_Req.io_Flags & SANA2IOF_RAW))
        data_len += ETHER_HDR_LEN;

    /* Pad to the shortest frame; the card appends the 4-byte CRC */

    while (data_len < ETHER_MIN_FRAME)
        ethbuffer[data_len++] = 0;

    /* Upload through remote write DMA. Receive uses the same DMA channel; both run in the */
    /* unit task only, so they cannot interleave.                                          */

    /* The data port is 16 bits wide, so the remote DMA count must be even: with an odd     */
    /* count the chip's byte counter steps past zero and INT_DMAFINISH never comes. Upload    */
    /* one pad byte more; the transmit count below stays exact.                               */

    ethbuffer[data_len] = 0;
    dma_len = (data_len + 1) & ~1;

    DIAG(DG_TASKSTATE) = TS_SEND;
    DIAG(DG_TXSTARTED)++;

    hw->regs[NE2000_INT_STATUS] = INT_DMAFINISH; /* may be left set by a receive */
    hw->regs[NE2000_DMA_COUNTER0] = dma_len & 0xFF;
    hw->regs[NE2000_DMA_COUNTER1] = dma_len >> 8;
    hw->regs[NE2000_DMA_START_ADDR0] = 0;
    hw->regs[NE2000_DMA_START_ADDR1] = TX_BUFFER + slot * TX_PAGES;
    hw->regs[NE2000_COMMAND] = COMMAND_PAGE0 | COMMAND_START | COMMAND_WRITE;
    hw_write_barrier(hw, ud);  /* Ensure DMA setup reaches hardware before data transfer */

#if USE_LONG_DMA
    {
        UBYTE *data = ethbuffer;
        UWORD left = dma_len;
        ULONG *ldata;

        /* One word first if the frame is not longword aligned, then aligned longwords */
        if ((ULONG)data & 2)
        {
            hw->DMAPort = *(UWORD *)data;
            data += 2;
            left -= 2;
        }
        ldata = (ULONG *)data;
        for (cycles = left >> 2; cycles; cycles--)
            DMAPORT_LONG(hw) = *ldata++;
        if (left & 2)
            hw->DMAPort = *(UWORD *)ldata;
    }
#else
    {
        UWORD *ethdata = (UWORD *)ethbuffer;

        for (cycles = dma_len >> 1; cycles; cycles--)
            hw->DMAPort = *ethdata++;
    }
#endif
    PERF_LAP2(DG_TICKSTXUPLOAD, DG_TICKSTX);

    /* Wait for the card to report the upload complete, but not forever */

    for (cycles = 10000; cycles && !(hw->regs[NE2000_INT_STATUS] & INT_DMAFINISH); cycles--)
    {
        hw_read_barrier(hw, ud);  /* Ensure we read fresh status, not cached */
    }
    if (!cycles)
    {
        DBG_U("UploadPacket(): no DMA finished flag after upload.");
        DIAG(DG_TXDMATIMEOUT)++;
    }
    hw->regs[NE2000_INT_STATUS] = INT_DMAFINISH;
    DIAG(DG_TXWAITPOLLS) += 10000 - cycles;
    PERF_LAP2(DG_TICKSTXWAIT, DG_TICKSTX);

    ud->ud_TxLen[slot] = data_len;
    return TRUE;
}

///
/// TransmitBuffer()

/* Puts an uploaded transmit buffer on the wire. The card answers with a transmit or       */
/* transmit-error interrupt, handled in ServiceCard().                                     */

void TransmitBuffer(struct UnitData *ud, WORD slot)
{
    volatile struct Ne2000 *hw = ud->ud_Hardware;

    hw->regs[NE2000_TX_PAGE_START] = TX_BUFFER + slot * TX_PAGES;
    hw->regs[NE2000_TX_COUNTER0] = ud->ud_TxLen[slot] & 0xFF;
    hw->regs[NE2000_TX_COUNTER1] = ud->ud_TxLen[slot] >> 8;
    hw->regs[NE2000_COMMAND] = COMMAND_PAGE0 | COMMAND_START | COMMAND_ABORT | COMMAND_TXP;
    hw_write_barrier(hw, ud);  /* Ensure transmit command reaches hardware */
    ud->ud_TxActive = slot;
}
///
/// PrintEtherAddress()

/* Serial output of a station address on one line, as 00:80:AD:90:3E:0A */
static void PrintEtherAddress(const char *label, const UBYTE *a)
{
    KPrintF("%s %02lx:%02lx:%02lx:%02lx:%02lx:%02lx\n", (LONG)label, (LONG)a[0], (LONG)a[1],
            (LONG)a[2], (LONG)a[3], (LONG)a[4], (LONG)a[5]);
}

///
/// GetHwAddress()

void GetHwAddress(struct UnitData *ud)
{
    volatile struct Ne2000 *hw = ud->ud_Hardware;
    WORD i;

    hw->regs[NE2000_DMA_COUNTER0] = 6;
    hw->regs[NE2000_DMA_COUNTER1] = 0;
    hw->regs[NE2000_DMA_START_ADDR0] = 0;
    hw->regs[NE2000_DMA_START_ADDR1] = 0;
    hw->regs[NE2000_COMMAND] = COMMAND_PAGE0 | COMMAND_READ;
    for (i = 0; i < ETHER_ADDR_LEN; i++)
        ud->ud_EtherAddress[i] = hw->DMAPort;

    PrintEtherAddress("Hardware address (read):", ud->ud_EtherAddress);

    return;
}

///
/// WriteHwAddress()

void WriteHwAddress(struct UnitData *ud)
{
    volatile struct Ne2000 *hw = ud->ud_Hardware;
    WORD i;

#ifdef PDEBUG
    PrintEtherAddress("Hardware address (write):", ud->ud_SoftAddress);
#endif

    hw->regs[NE2000_COMMAND] = COMMAND_PAGE1 | COMMAND_ABORT | COMMAND_STOP;
    for (i = 0; i < ETHER_ADDR_LEN; i++)
    {
        hw->regs[NE2000_PHYSICAL_ADDR0 + i] = ud->ud_SoftAddress[i];
    }
    hw->regs[NE2000_COMMAND] = COMMAND_PAGE0 | COMMAND_ABORT | COMMAND_STOP;
    return;
}

///
/// RingBufferNotEmpty()

LONG RingBufferNotEmpty(struct UnitData *ud)
{
    UBYTE current;
    volatile struct Ne2000 *hw = ud->ud_Hardware;

    /* Page 1 may only be selected with the card's interrupts masked, see IntCode().        */
    /* Register 7 is INT_STATUS on page 0 and CURRENT on page 1: without barriers the read  */
    /* could be answered from the data cache with the wrong one (no-ops with mmu.library).  */
    hw->regs[NE2000_COMMAND] = COMMAND_PAGE1 | COMMAND_ABORT | COMMAND_START;
    hw_write_barrier(hw, ud);
    hw_read_barrier(hw, ud);
    current = hw->regs[NE2000_CURRENT_PAGE];
    hw->regs[NE2000_COMMAND] = COMMAND_PAGE0 | COMMAND_ABORT | COMMAND_START;
    hw_write_barrier(hw, ud);
    DIAG(DG_LASTCURRENT) = current;

    if (ud->ud_NextPage == current)
        return FALSE;
    else
        return TRUE;
}

///

// SUBTASK CODE

/// SearchReadRequest()

struct IOSana2Req *SearchReadRequest(struct UnitData *ud, struct MinList *queue, ULONG type)
{
    struct Library *SysBase = ud->ud_SysBase;
    struct IOSana2Req *req, *found = NULL;

    ObtainSemaphore(&ud->ud_RxSem);

    for (req = (struct IOSana2Req *)queue->mlh_Head;
         req->ios2_Req.io_Message.mn_Node.ln_Succ;
         req = (struct IOSana2Req *)req->ios2_Req.io_Message.mn_Node.ln_Succ)
    {
        if (req->ios2_PacketType == type)
        {
            Remove((struct Node *)req);
            found = req;
            break;
        }
    }

    ReleaseSemaphore(&ud->ud_RxSem);

    return found;
}

///
/// FlushQueues()

void FlushQueues(struct UnitData *ud, LONG err, LONG werr)
{
    USE_U(SysBase)
    struct IOSana2Req *xreq;

    ObtainSemaphore(&ud->ud_RxSem);
    for (;;)
    {
        xreq = (struct IOSana2Req *)RemHead((struct List *)&ud->ud_RxQueue);
        if (!xreq)
            break;
        DBG1_T("<- READ [$08%lx] [F].", xreq);
        IoDone(ud, xreq, err, werr);
    }
    ReleaseSemaphore(&ud->ud_RxSem);

    ObtainSemaphore(&ud->ud_TxSem);
    for (;;)
    {
        xreq = (struct IOSana2Req *)RemHead((struct List *)&ud->ud_TxQueue);
        if (!xreq)
            break;
        DBG1_T("<- WRITE [$08%lx] [F].", xreq);
        IoDone(ud, xreq, err, werr);
    }
    ReleaseSemaphore(&ud->ud_TxSem);
    return;
}

///
/// TakeOffline()

/* Unit task only. Stops the card and returns every request the driver holds with          */
/* S2ERR_OUTOFSERVICE: the transmits in the card's buffers (their interrupts will never    */
/* come now) and all queued reads and writes.                                              */

void TakeOffline(struct UnitData *ud)
{
    struct IOSana2Req *req;
    WORD slot;

    if (!(ud->ud_Flags & UF_ONLINE))
        return;

    ud->ud_Flags &= ~UF_ONLINE;
    GoOffline(ud);

    for (slot = 0; slot < 2; slot++)
    {
        if ((req = ud->ud_TxReq[slot]))
        {
            ud->ud_TxReq[slot] = NULL;
            IoDone(ud, req, S2ERR_OUTOFSERVICE, S2WERR_UNIT_OFFLINE);
        }
    }
    ud->ud_TxActive = ud->ud_TxWaiting = -1;
    FlushQueues(ud, S2ERR_OUTOFSERVICE, S2WERR_UNIT_OFFLINE);
}

///
/// StartNextWrite()

/* Fills free transmit buffers from the write queue. A frame is put on the wire at once if */
/* the wire is free; otherwise it waits, already uploaded, and ServiceCard() starts it the  */
/* moment the frame before it is done. Requests UploadPacket() rejects are skipped. The     */
/* transmit state is only touched by the unit task.                                         */

void StartNextWrite(struct UnitData *ud)
{
    USE_U(SysBase)
    struct IOSana2Req *req;
    WORD slot;

    for (;;)
    {
        if (!ud->ud_TxReq[0])
            slot = 0;
        else if (!ud->ud_TxReq[1])
            slot = 1;
        else
            break;

        ObtainSemaphore(&ud->ud_TxSem);
        req = (struct IOSana2Req *)RemHead((struct List *)&ud->ud_TxQueue);
        ReleaseSemaphore(&ud->ud_TxSem);

        if (!req)
            break;

        if (UploadPacket(ud, req, slot))
        {
            ud->ud_TxReq[slot] = req;
            if (ud->ud_TxActive < 0)
                TransmitBuffer(ud, slot);
            else
                ud->ud_TxWaiting = slot;
        }
    }
}

///
/// ServiceCard()

/* Handles all pending card events in the unit task, after IntCode() masked the card and   */
/* signalled us. The card's interrupts stay masked for the whole function (which is what   */
/* allows register page 1 to be selected, see IntCode()) and are unmasked at the end.      */

void ServiceCard(struct UnitData *ud)
{
    USE_U(SysBase)
    volatile struct Ne2000 *hw = ud->ud_Hardware;
    struct IOSana2Req *req;
    UBYTE intstatus;
    BOOL recovering = FALSE, resend = FALSE;
    PERF_VARS;
#if PERF_TIMING
    struct EClockVal perf_service;
#endif

    if (!(ud->ud_Flags & UF_ONLINE))
        return;

    DIAG(DG_SERVICERUNS)++;
    PERF_SINCE(ud->ud_IntStamp, ud->ud_IntStampSet, DG_MAXTICKSINTLAT, DG_SLOWINTLAT, PERF_TICKS_2MS);
#if PERF_TIMING
    ReadEClock(&perf_service);
#endif

    hw->regs[NE2000_INT_MASK] = 0;
    ud->ud_IntMasked = TRUE;
    hw_write_barrier(hw, ud);
    hw_read_barrier(hw, ud);

    /* Diagnostics: receive errors are not among the interrupts the driver enables, but the */
    /* card still records them in the status register.                                     */
    intstatus = hw->regs[NE2000_INT_STATUS];
    DIAG(DG_INTSEEN) |= intstatus;
    if (intstatus & INT_RXERROR)
    {
        DIAG(DG_RXERRINTS)++;
        DIAG(DG_LASTRXSTATUS) = hw->regs[NE2000_RX_STATUS];
        hw->regs[NE2000_INT_STATUS] = INT_RXERROR;
    }

    while ((intstatus = (hw->regs[NE2000_INT_STATUS] & INTMASK)))
    {
        DIAG(DG_TASKSTATE) = TS_SERVICE;
        DIAG(DG_LASTINTSTATUS) = intstatus;

        if (intstatus & (INT_TXERROR | INT_TXPACKET))
        {
            UBYTE txstatus = hw->regs[NE2000_TX_STATUS];

            /* What happened on the wire, for diagnostics */
            if (txstatus & (TXS_COLLIDED | TXS_ABORTED))
                DIAG(DG_TXCOLLISIONS) += (txstatus & TXS_ABORTED) ? 16 : (hw->regs[NE2000_COLLISIONS] & 0x0F);
            if (txstatus & TXS_LATE_COLLISION)
                DIAG(DG_TXLATECOLL)++;
            if (txstatus & TXS_CARRIER_LOST)
                DIAG(DG_TXCARRIERLOST)++;

            hw->regs[NE2000_INT_STATUS] = intstatus & (INT_TXERROR | INT_TXPACKET);

            /* The buffer that was on the wire is free again... */
            req = NULL;
            if (ud->ud_TxActive >= 0)
            {
                req = ud->ud_TxReq[ud->ud_TxActive];
                ud->ud_TxReq[ud->ud_TxActive] = NULL;
                ud->ud_TxActive = -1;
            }

            /* ...and if the other one is loaded it goes out right away, before anything else */
            if (ud->ud_TxWaiting >= 0)
            {
                TransmitBuffer(ud, ud->ud_TxWaiting);
                ud->ud_TxWaiting = -1;
            }
            else
            {
                /* Diagnostics: the wire is idle now. Is a write already queued (the driver is */
                /* behind) or has the stack not supplied one? Only the list's tail pointer is   */
                /* read, a single long: no locking needed for this estimate.                    */
                DIAG(DG_TXWIREIDLE)++;
                if (ud->ud_TxQueue.mlh_TailPred != (struct MinNode *)&ud->ud_TxQueue)
                    DIAG(DG_TXIDLEMSGWAIT)++;
            }

            if (req)
            {
                if (intstatus & INT_TXERROR)
                {
                    DIAG(DG_TXERRORS)++;
                    IoDone(ud, req, S2ERR_TX_FAILURE, S2WERR_TOO_MANY_RETIRES);
                }
                else
                {
                    DIAG(DG_TXDONE)++;
                    ud->ud_DevStats.PacketsSent++;
                    IoDone(ud, req, S2ERR_NO_ERROR, S2WERR_GENERIC_ERROR);
                }
            }
            else
                DIAG(DG_TXNOPENDING)++;
        }

        if (intstatus & INT_OVERFLOW)
        {
            /* The ring is full. Stop the card, then empty the ring below and restart */
            DIAG(DG_OVERFLOWS)++;
            resend = OverflowBegin(ud);
            recovering = TRUE;
        }

        if ((intstatus & INT_RXPACKET) || recovering)
        {
            ULONG offset, len;
            UWORD mask;
            WORD i;

            /* Clear before draining: a packet arriving meanwhile sets it again */
            hw->regs[NE2000_INT_STATUS] = INT_RXPACKET;
            hw_write_barrier(hw, ud);
            hw_read_barrier(hw, ud);

            PERF_START();
            while (RingBufferNotEmpty(ud))
            {
                DIAG(DG_TASKSTATE) = TS_RXDRAIN;
                len = PacketReceived(ud);
                PERF_LAP(DG_TICKSRXREAD);

                /* PacketReceived() returns 0 for a corrupt ring header - reset the ring */
                if (!len)
                {
                    if (!recovering)
                    {
                        resend = OverflowBegin(ud);
                        recovering = TRUE;
                    }
                    RingReset(ud);
                    break;
                }
                DIAG(DG_RXFRAMES)++;

                if ((req = SearchReadRequest(ud, &ud->ud_RxQueue, ud->ud_RxBuffer[RXW_TYPE])))
                {
                    if (req->ios2_Req.io_Flags & SANA2IOF_RAW)
                        offset = 0;
                    else
                        offset = RXW_DATA;
                    ((struct BuffFunctions *)req->ios2_BufferManagement)->bf_CopyTo(req->ios2_Data, &ud->ud_RxBuffer[offset], len - (offset << 1));
                    CopyMem(&ud->ud_RxBuffer[RXW_DST], req->ios2_DstAddr, ETHER_ADDR_LEN);
                    CopyMem(&ud->ud_RxBuffer[RXW_SRC], req->ios2_SrcAddr, ETHER_ADDR_LEN);
                    req->ios2_DataLength = len - (offset << 1);
                    mask = 0xFFFF;
                    for (i = RXW_SRC - 1; i >= RXW_DST; i--)     /* all-ones destination */
                        mask &= ud->ud_RxBuffer[i];
                    if (mask == 0xFFFF)
                        req->ios2_Req.io_Flags |= SANA2IOF_BCAST;
                    ud->ud_DevStats.PacketsReceived++;
                    DIAG(DG_RXDELIVERED)++;
                    PERF_STAMP(ud->ud_RxStamp, ud->ud_RxStampSet);
                    IoDone(ud, req, S2ERR_NO_ERROR, S2WERR_GENERIC_ERROR);
                }
                else
                {
                    DIAG(DG_RXNOREADER)++;
                    DIAG(DG_LASTDROPTYPE) = ud->ud_RxBuffer[RXW_TYPE];
                }
                PERF_LAP(DG_TICKSRXCOPY);
            }
            PERF_STOP(DG_TICKSRXREAD); /* the final "ring is empty" check */
        }

        if (recovering)
        {
            OverflowEnd(ud, resend);
            recovering = FALSE;
        }

        /* Keep transmitting while a long receive burst is being drained */
        StartNextWrite(ud);
    }

    StartNextWrite(ud);

    /* Unmask. If something became pending since the loop ended the card interrupts at once */
    /* and IntCode() signals us again. Not if S2_OFFLINE took the card down meanwhile.      */
    /* The card's error tallies (reading clears them) */
    DIAG(DG_RXALIGNERRORS) += hw->regs[NE2000_CNT_ALIGN_ERRORS];
    DIAG(DG_RXCRCERRORS) += hw->regs[NE2000_CNT_CRC_ERRORS];
    DIAG(DG_RXMISSED) += hw->regs[NE2000_CNT_MISSED];

    DIAG(DG_TASKSTATE) = TS_PORT;
#if PERF_TIMING
    ReadEClock(&perf_t1);
    DIAG(DG_TICKSSERVICE) += perf_t1.ev_lo - perf_service.ev_lo;
#endif
    if (ud->ud_Flags & UF_ONLINE)
    {
        ud->ud_IntMasked = FALSE; /* before the write, see IntCode() */
        hw->regs[NE2000_INT_MASK] = INTMASK;
        hw_write_barrier(hw, ud);
    }
}

///
/// MainLoop()

void MainLoop(struct UnitData *ud, struct MsgPort *port)
{
    USE_U(SysBase)
    ULONG signals, sigmask;

    sigmask = 1 << port->mp_SigBit;
    for (;;)
    {
        DIAG(DG_TASKSTATE) = TS_WAITING;
        signals = Wait(SIGBREAKF_CTRL_C | sigmask | ud->ud_ServiceMask);
        DIAG(DG_TASKSTATE) = TS_PORT;

        if (signals & ud->ud_ServiceMask)
        {
            ServiceCard(ud);
        }

        if (signals & SIGBREAKF_CTRL_C)
        {
            struct IOSana2Req *req;

            /* The unit is going away: stop the card and hand every request back */
            DBG1_T("TASK: %s task killed.", ud->ud_Name);
            TakeOffline(ud);
            FlushQueues(ud, IOERR_ABORTED, S2WERR_GENERIC_ERROR);
            while ((req = (struct IOSana2Req *)GetMsg(port)))
                IoDone(ud, req, IOERR_ABORTED, S2WERR_GENERIC_ERROR);
            return;
        }

        if (signals & sigmask)
        {
            struct IOSana2Req *req;

            while ((req = (struct IOSana2Req *)GetMsg(port)))
            {
                switch (req->ios2_Req.io_Command)
                {
                case S2_CONFIGINTERFACE:
                    S2ConfigInterface(ud, req);
                    break;

                case S2_ONLINE:
                    S2Online(ud, req);
                    break;

                case S2_OFFLINE:
                    S2Offline(ud, req);
                    break;

                case CMD_FLUSH:
                    DBG1_T("-> FLUSH [$%08lx].", req);
                    FlushQueues(ud, IOERR_ABORTED, S2WERR_GENERIC_ERROR);
                    DBG1_T("<- FLUSH [$%08lx].", req);
                    IoDone(ud, req, S2ERR_NO_ERROR, S2WERR_GENERIC_ERROR);
                    break;

                default:
                    DBG2_T("-> Unknown ($%lx) [$%08lx].", req->ios2_Req.io_Command, req);
                }
            }
        }
    }
}

///
/// UnitTask()

void UnitTask(void)
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
    struct Library *SysBase = *(struct Library **)4; /* AbsExecBase */
#pragma GCC diagnostic pop
    struct Task *task;
    struct MsgPort *port;
    struct UnitData *ud;

    task = FindTask(NULL);
    if ((port = CreateMsgPort()))
    {
        port->mp_Node.ln_Name = task->tc_Node.ln_Name;
        port->mp_Node.ln_Pri = 20;
        AddPort(port);
        WaitPort(port);
        RemPort(port);
        ud = (struct UnitData *)GetMsg(port);
        if ((ud->ud_ServiceBit = AllocSignal(-1)) != -1)
        {
            ud->ud_ServiceMask = 1 << ud->ud_ServiceBit;
            MainLoop(ud, port);
            FreeSignal(ud->ud_ServiceBit);
        }
        DeleteMsgPort(port);
        Forbid();
        ReplyMsg(&ud->ud_Message);
    }
    return;
}

///
