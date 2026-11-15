/* OpenPCI NE2000 driver. */
/// includes and defines

// ignore that STRPTR is unsigned char *, but all string literals are char *
#pragma GCC diagnostic ignored "-Wpointer-sign"

#define __NOLIBBASE__

#include <proto/exec.h>
#include <proto/expansion.h>
#include <proto/utility.h>
#include <proto/dos.h>
#include <proto/openpci.h>
#include <proto/timer.h>
#include <exec/libraries.h>
#include <exec/resident.h>
#include <exec/memory.h>
#include <exec/interrupts.h>
#include <devices/sana2.h>
#include <hardware/intbits.h>
#include <dos/dostags.h>
#include <libraries/pcitags.h>
#include <utility/tagitem.h>
#include <libraries/openpci.h>

/*#include <faststring.h>*/ /* Probably used for strcpy */

#include "rev.h"
#include "ne2000.h"
// #include "endian.h"  Not needed, endian swapping is also declared by openpci.h

/* mmu.library for cache-inhibited hardware mapping */
struct AbstractDescriptor; /* only named in a prototype in clib/mmu_protos.h */
#include <proto/mmu.h>
#include <mmu/context.h>

#define OK 0

#define TX_BUFFER   0x40
#define RX_BUFFER   0x46
#define BUFFER_END  0x80
#define INTMASK     (INT_RXPACKET | INT_TXPACKET | INT_TXERROR | INT_OVERFLOW)

/* Cache control flags for hardware I/O */
#define CACRF_ClearD    (1UL<<11)  /* Clear data cache */

/* PCI card ID's */

#define PCI_VENDOR_REALTEK 0x10EC
#define PCI_DEVICE_RTL8029 0x8029

// Unit flags.

#define UF_CONFIGURED   0x01
#define UF_ONLINE       0x02
#define UF_PROMISCUOUS  0x04
#define UF_EXCLUSIVE    0x08    /* opened with SANA2OPF_MINE */

// Macro for registerized parameters (used in some OS functions).

#define reg(a) __asm(#a)

// Macro for declaring local libraries bases.

#define USE(a) struct Library *a = dd->dd_##a;
#define USE_U(a) struct Library *a = ud->ud_##a;

// Macros for debug messages.

#if PDEBUG

#define USE_D(a) struct Library* a = dd->dd_##a;
#define USE_UD(a) struct Library* a = ud->ud_##a;
/*  #define DBG(a) KPrintf(dd->debug, a "\n")
  #define DBG_U(a) KPrintf(ud->debug, a "\n")
  #define DBG_T(a) KPrintf(ud->tdebug, a "\n")
  #define DBG1(a,b) KPrintf(dd->debug, a "\n",(LONG)b)
  #define DBG1_U(a,b) KPrintf(ud->debug, a "\n",(LONG)b)
  #define DBG1_T(a,b) KPrintf(ud->tdebug, a "\n",(LONG)b)
  #define DBG2(a,b,c) KPrintf(dd->debug, a "\n",(LONG)b,(LONG)c)
  #define DBG2_U(a,b,c) KPrintf(ud->debug, a "\n",(LONG)b,(LONG)c)
  #define DBG2_T(a,b,c) KPrintf(ud->tdebug, a "\n",(LONG)b,(LONG)c)*/

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

/* Initialization debug macros - always enabled for important startup messages */
/* These use KPrintF for serial debug output, independent of PDEBUG setting */
#include <clib/debug_protos.h>
#define INIT_DBG(a) KPrintF(a "\n")
#define INIT_DBG1(a,b) KPrintF(a "\n",(LONG)b)
#define INIT_DBG_U(a) KPrintF(a "\n")
#define INIT_DBG1_U(a,b) KPrintF(a "\n",(LONG)b)

// New Style Device support

#define NSCMD_DEVICEQUERY 0x4000

struct NSDeviceQueryResult
{
    ULONG DevQueryFormat;     /* this is type 0               */
    ULONG SizeAvailable;      /* bytes available              */
    UWORD DeviceType;         /* what the device does         */
    UWORD DeviceSubType;      /* depends on the main type     */
    UWORD *SupportedCommands; /* 0 terminated list of cmd's   */
};

#define NSDEVTYPE_SANA2 7 /* A >=SANA2R2 networking device */

///
/// diagnostics

/* Diagnostic counters and state, readable at any time with S2_GETSPECIALSTATS (see the   */
/* s2stat tool). They are copied in the caller's context, so they can be read even when    */
/* the unit task is stuck - DG_TASKSTATE then tells where.                                 */

enum
{
    DG_INTERRUPTS,    /* interrupts claimed by IntCode() */
    DG_SERVICERUNS,   /* ServiceCard() calls */
    DG_RXFRAMES,      /* frames read from the ring */
    DG_RXDELIVERED,   /* ... handed to a pending CMD_READ */
    DG_RXNOREADER,    /* ... dropped, no pending CMD_READ for the packet type */
    DG_LASTDROPTYPE,  /* packet type of the last frame dropped that way */
    DG_RXBADHEADER,   /* corrupt ring headers (each resets the ring) */
    DG_LASTBADHEADER, /* the last corrupt ring header */
    DG_OVERFLOWS,     /* ring overflow interrupts */
    DG_TXSTARTED,     /* transmits started */
    DG_TXDONE,        /* transmit interrupts, ok */
    DG_TXERRORS,      /* transmit interrupts, error */
    DG_TXNOPENDING,   /* transmit interrupts with no request in flight */
    DG_TXDMATIMEOUT,  /* upload to the card did not report completion */
    DG_READSQUEUED,   /* CMD_READ requests received */
    DG_WRITESQUEUED,  /* CMD_WRITE/S2_BROADCAST requests received */
    DG_TXPENDING,     /* 1 if a transmit is in flight */
    DG_NEXTPAGE,      /* driver's ring read pointer */
    DG_LASTCURRENT,   /* card's ring write pointer when last read */
    DG_LASTINTSTATUS, /* last non-zero interrupt status seen by ServiceCard() */
    DG_TASKSTATE,     /* what the unit task is doing, TS_* */
    DG_COUNT
};

#define TS_WAITING   0 /* in Wait() */
#define TS_PORT      1 /* handling requests */
#define TS_SERVICE   2 /* in ServiceCard() */
#define TS_RXDRAIN   3 /* reading packets from the ring */
#define TS_SEND      4 /* in SendPacket() */
#define TS_OVERFLOW  5 /* in BufferOverflow() */

static const char *const DiagNames[DG_COUNT] =
{
    "Interrupts", "ServiceRuns", "RxFrames", "RxDelivered", "RxNoReader", "LastDropType", "RxBadHeader",
    "LastBadHeader", "Overflows", "TxStarted", "TxDone", "TxErrors", "TxNoPending",
    "TxDmaTimeout", "ReadsQueued", "WritesQueued", "TxPending", "NextPage", "LastCurrent",
    "LastIntStatus", "TaskState"
};

#define DIAG(n) (ud->ud_Diag[n])

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
    struct Library *dd_MMUBase;  /* Optional: for cache-inhibited I/O mapping */
    struct UnitData *dd_Units[4];
    struct timerequest dd_Treq;

#ifdef PDEBUG
    BPTR debug;
    UBYTE dpath[128];
#endif
};

struct UnitData
{
    struct Message ud_Message;
    volatile struct Ne2000 *ud_Hardware;
    struct Library *ud_SysBase;
    struct Library *ud_OpenPciBase;
    struct Library *ud_DOSBase;
    struct Library *ud_TimerBase;
    struct Library *ud_MMUBase;  /* Optional: for cache-inhibited I/O mapping */
    struct Interrupt *ud_Interrupt;
    struct Task *ud_Task;
    struct MsgPort *ud_TaskPort;
    struct MsgPort *ud_LifeTime;
    struct MinList ud_RxQueue;
    struct SignalSemaphore ud_RxSem;
    struct MinList ud_TxQueue;
    struct SignalSemaphore ud_TxSem;
    struct IOSana2Req *ud_PendingWrite;
    struct pci_dev* ud_PciDev;
    ULONG ud_OpenCnt;
    ULONG ud_ServiceMask;
    UWORD ud_RxBuffer[768];
    UBYTE ud_Name[27];
    UBYTE ud_EtherAddress[6];
    UBYTE ud_SoftAddress[6];
    struct Sana2DeviceStats ud_DevStats;
    ULONG ud_Diag[DG_COUNT];
    UBYTE ud_Flags;
    UBYTE ud_NextPage;
    BYTE ud_ServiceBit;
    UBYTE ud_Unit;       /* unit number, index into dd_Units[] */
    UBYTE ud_IntMasked;  /* card interrupts masked by us; only decides IntCode()'s return value */
    UBYTE ud_UseMMUCacheInhibit;  /* TRUE if using MMU lib for cache-inhibit */
#ifdef PDEBUG
    BPTR debug;
    BPTR tdebug;
#endif
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
void GoOnline(struct UnitData *ud);
void GoOffline(struct UnitData *ud);
void GetHwAddress(struct UnitData *ud);
void WriteHwAddress(struct UnitData *ud);
LONG PacketReceived(struct UnitData *ud);
LONG RingBufferNotEmpty(struct UnitData *ud);
void BufferOverflow(struct UnitData *ud);

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

/* Good enough */
unsigned char *StrCopy(unsigned char *dest, const unsigned char *src)
{
    unsigned char *d = dest;
    while (*src)
        *dest++ = *src++;
    *dest = 0;
    return d;
}

/* Helper function to ensure cache coherency for hardware writes */
static inline void hw_write_barrier(volatile struct Ne2000 *hw, struct UnitData *ud)
{
    struct Library *SysBase;

    /* If MMU library marked hardware as cache-inhibited, no flush needed */
    if (ud->ud_UseMMUCacheInhibit)
        return;

    /* Fallback: Flush data cache to ensure all writes reach hardware immediately */
    SysBase = ud->ud_SysBase;
    if (SysBase) {
        CacheClearE((APTR)hw, sizeof(struct Ne2000), CACRF_ClearD);
    }
}

/* Read barrier: invalidate cache before reading from hardware */
static inline void hw_read_barrier(volatile struct Ne2000 *hw, struct UnitData *ud)
{
    struct Library *SysBase;

    /* If MMU library marked hardware as cache-inhibited, no flush needed */
    if (ud->ud_UseMMUCacheInhibit)
        return;

    /* Fallback: Invalidate data cache to ensure reads come from hardware */
    SysBase = ud->ud_SysBase;
    if (SysBase) {
        CacheClearE((APTR)hw, sizeof(struct Ne2000), CACRF_ClearD);
    }
}

/* Setup cache-inhibited mapping for hardware using MMU library (if available) */
static BOOL setup_cache_inhibited_hw(struct UnitData *ud)
{
    if (ud->ud_MMUBase)
    {
        /* MMU library is available - mark hardware region as cache-inhibited */
        ULONG hw_addr = (ULONG)ud->ud_Hardware;
        ULONG hw_size = sizeof(struct Ne2000);

        /* SetProperties requires MMUBase to be set for the inline call */
        struct Library *MMUBase = ud->ud_MMUBase;

        /* SetProperties(context, flags, mask, address, size, tags...)
         * context = NULL means current context
         * MAPP_CACHEINHIBIT = mark as cache-inhibited
         */
        if (SetProperties(NULL, MAPP_CACHEINHIBIT, MAPP_CACHEINHIBIT,
                         hw_addr, hw_size, TAG_DONE))
        {
            INIT_DBG_U("Hardware region marked as cache-inhibited via mmu.library");
            return TRUE;
        }
        else
        {
            INIT_DBG_U("Warning: Failed to set cache-inhibit via mmu.library, will use cache flush");
        }
    }
    return FALSE;
}

/// DevInit()
// Called when the device is loaded into memory. Makes system library, initializes Library structure, opens
// libraries used by the device. Returns device base or NULL if init failed.

struct DevData *DevInit(register APTR seglist reg(a0), register struct Library *sysb reg(a6))
{
    struct DevData *dd;
    struct Library *SysBase = sysb;

    if ((dd = (struct DevData *)MakeLibrary(FuncTable, NULL, NULL, sizeof(struct DevData), 0)))
    {
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
            WORD i;

            for (i = 0; i < 4; i++)
                dd->dd_Units[i] = NULL;

#ifdef PDEBUG
/*USE_D(DOSBase)*/
/*StrCopy(dd->dpath, "KCON:0/17/400/300/openpci-rtl8029.device (main)/AUTO/CLOSE/WAIT");*/
/*GetVar("PrometheusDebug", dd->dpath, 128, 0);*/
/*dd->debug = Open(dd->dpath, MODE_NEWFILE);*/
#endif

            INIT_DBG1("Device initialized, base at $%08lx.", dd);
            AddDevice((struct Device *)dd);
            return dd;
        }
        CloseDeviceLibraries(dd);
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

    if ((unit >= 0) && (unit <= 3))
    {
        if ((ud = OpenUnit(dd, unit, flags)))
        {
            req->ios2_Req.io_Error = 0;
            req->ios2_Req.io_Message.mn_Node.ln_Type = NT_REPLYMSG;
            req->ios2_Req.io_Device = (struct Device *)dd;
            req->ios2_Req.io_Unit = (struct Unit *)ud;
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
    case S2_CONFIGINTERFACE:
        S2ConfigInterface(ud, req);
        break;
    case S2_ONLINE:
        S2Online(ud, req);
        break;
    case S2_OFFLINE:
        S2Offline(ud, req);
        break;
    case S2_GETGLOBALSTATS:
        S2GetGlobalStats(ud, req);
        break;
    case S2_GETSPECIALSTATS:
        S2GetSpecialStats(ud, req);
        break;

    case CMD_READ:
        /* DBG1("CMD_READ [$%08lx].", (LONG)req); */
        req->ios2_Req.io_Flags &= ~IOF_QUICK;
        PutMsg(ud->ud_TaskPort, &req->ios2_Req.io_Message);
        break;

    case S2_BROADCAST:
        for (i = 0; i < 6; i++)
            req->ios2_DstAddr[i] = 0xFF;
    case CMD_WRITE:
        /* DBG1("CMD_WRITE [$%08lx].", (LONG)req); */
        req->ios2_Req.io_Flags &= ~IOF_QUICK;
        PutMsg(ud->ud_TaskPort, &req->ios2_Req.io_Message);
        break;

    case CMD_FLUSH:
        /* DBG1("CMD_FLUSH [$%08lx].", (LONG)req); */
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

    /* Try to open mmu.library - optional, for cache-inhibited hardware mapping */
    dd->dd_MMUBase = OpenLibrary("mmu.library", 39);
    if (dd->dd_MMUBase)
    {
        INIT_DBG("mmu.library opened - will use cache-inhibited I/O mapping");
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

    DBG("RunTask() called.");

    if ((ud->ud_LifeTime = CreateMsgPort()))
    {
        if ((ud->ud_Task = (struct Task *)CreateNewProcTags(
                NP_Entry, (LONG)UnitTask,
                NP_Name, (LONG)ud->ud_Name,
                NP_Priority, 6,
                NP_StackSize, 16384, /* SendPacket() keeps a full frame on the stack */
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
                    DBG("Task port detected.");
                    ud->ud_Message.mn_Node.ln_Type = NT_MESSAGE;
                    ud->ud_Message.mn_Length = sizeof(struct UnitData);
                    ud->ud_Message.mn_ReplyPort = ud->ud_LifeTime;
                    PutMsg(ud->ud_TaskPort, &ud->ud_Message);
                    return TRUE;
                }
            }
        }
    }
    return FALSE;
}

///
/// KillTask(struct DevData *dd, struct UnitData *ud)

void KillTask(struct DevData *dd, struct UnitData *ud)
{
    USE(SysBase)

    Signal(ud->ud_Task, SIGBREAKF_CTRL_C);
    WaitPort(ud->ud_LifeTime);
    GetMsg(ud->ud_LifeTime);
    DeleteMsgPort(ud->ud_LifeTime);
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

    if (ud->ud_OpenCnt && ((flags & SANA2OPF_MINE) || (ud->ud_Flags & UF_EXCLUSIVE)))
        return NULL;

    if (flags & SANA2OPF_MINE)
        ud->ud_Flags |= UF_EXCLUSIVE;

    /* Set promiscuous flag if requested - we konw here MINE was requested too, and noone else has opened */
    /* the unit. So we can just set it if requested. */

    if (flags & SANA2OPF_PROM)
        ud->ud_Flags |= UF_PROMISCUOUS;

    /* OK, increment open counter and exit with success. */

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
#ifdef PDEBUG
            ud->debug = dd->debug;
#endif

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
            StrCopy(ud->ud_Name, "openpci-rtl8029.device (x)");
            ud->ud_Name[24] = '0' + unit;
            ud->ud_RxQueue.mlh_Head = (struct MinNode *)&ud->ud_RxQueue.mlh_Tail;
            ud->ud_RxQueue.mlh_Tail = NULL;
            ud->ud_RxQueue.mlh_TailPred = (struct MinNode *)&ud->ud_RxQueue.mlh_Head;
            InitSemaphore(&ud->ud_RxSem);


            ud->ud_TxQueue.mlh_Head = (struct MinNode *)&ud->ud_TxQueue.mlh_Tail;
            ud->ud_TxQueue.mlh_Tail = NULL;
            ud->ud_TxQueue.mlh_TailPred = (struct MinNode *)&ud->ud_TxQueue.mlh_Head;
            InitSemaphore(&ud->ud_TxSem);


            ud->ud_NextPage = RX_BUFFER;

            /* Try to setup cache-inhibited mapping via MMU library */
            ud->ud_UseMMUCacheInhibit = setup_cache_inhibited_hw(ud);

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
        if (ud->ud_Flags & UF_ONLINE)
            GoOffline(ud);
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
            query->AddrFieldSize = 48;

        if (query->SizeAvailable >= 22)
            query->MTU = 1500;

        if (query->SizeAvailable >= 26)
            query->BPS = 10000000;

        if (query->SizeAvailable >= 30)
            query->HardwareType = S2WireType_Ethernet;

        // Report 30 bytes of data.
        query->SizeSupplied = query->SizeAvailable < 30 ? query->SizeAvailable : 30;
        IoDone(ud, req, OK, OK);
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
    CopyMem(ud->ud_SoftAddress, req->ios2_SrcAddr, 6);
    CopyMem(ud->ud_EtherAddress, req->ios2_DstAddr, 6);
    IoDone(ud, req, OK, OK);
    return;
}

///
/// S2Online()

void S2Online(struct UnitData *ud, struct IOSana2Req *req)
{
    DBG_U("S2_ONLINE.");
    ClearGlobalStats(ud);
    if (!(ud->ud_Flags & UF_ONLINE))
    {
        GoOnline(ud);
        ud->ud_Flags |= UF_ONLINE;
        IoDone(ud, req, OK, OK);
    }
    else
        IoDone(ud, req, S2ERR_BAD_STATE, S2WERR_UNIT_ONLINE);
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
        HardwareInit(ud);
        CopyMem(req->ios2_SrcAddr, ud->ud_SoftAddress, 6);
        WriteHwAddress(ud);
        if (!(ud->ud_Flags & UF_ONLINE))
        {
            GoOnline(ud);
            ud->ud_Flags |= UF_ONLINE;
        }
        ud->ud_Flags |= UF_CONFIGURED;
        IoDone(ud, req, OK, OK);
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
        ud->ud_Flags &= ~UF_ONLINE;
        GoOffline(ud);
        IoDone(ud, req, OK, OK);
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
        IoDone(ud, req, OK, OK);
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

    DIAG(DG_TXPENDING) = ud->ud_PendingWrite ? 1 : 0;
    DIAG(DG_NEXTPAGE) = ud->ud_NextPage;

    rec = (struct Sana2SpecialStatRecord *)(hdr + 1);
    for (i = 0; i < DG_COUNT && i < hdr->RecordCountMax; i++)
    {
        rec[i].Type = (S2WireType_Ethernet << 16) | i;
        rec[i].Count = DIAG(i);
        rec[i].String = (STRPTR)DiagNames[i];
    }
    hdr->RecordCountSupplied = i;
    IoDone(ud, req, OK, OK);
}

///
/// CmdNSDQuery()

void CmdNSDQuery(struct UnitData *ud, struct IOStdReq *req)
{
    USE_U(SysBase)
    struct NSDeviceQueryResult *qdata;
    LONG error = OK;

    DBG_U("NSCMD_DEVICEQUERY.");
    if (req->io_Length >= sizeof(struct NSDeviceQueryResult))
    {
        if ((qdata = (struct NSDeviceQueryResult *)req->io_Data))
        {
            if ((qdata->DevQueryFormat == 0) && (qdata->SizeAvailable == 0))
            {
                qdata->SizeAvailable = sizeof(struct NSDeviceQueryResult);
                qdata->DeviceType = NSDEVTYPE_SANA2;
                qdata->DeviceSubType = 0;
                qdata->SupportedCommands = NSDSupported;
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

    /* I don't use IoDone() here, because it writes to ios2_WireError */
    /* but this request can be simple IOStdReq one.                   */

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
        pci_add_intserver(intr, ud->ud_PciDev);
        ud->ud_Interrupt = intr;
        return TRUE;
    }
    return FALSE;
}

///
/// RemoveInterrupt()

void RemoveInterrupt(struct UnitData *ud)
{
    USE_U(SysBase)
    USE_U(OpenPciBase)

    pci_rem_intserver(ud->ud_Interrupt, ud->ud_PciDev);
    FreeMem(ud->ud_Interrupt, sizeof(struct Interrupt));
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

    hw->regs[NE2000_COMMAND] = 0x21; // COMMAND_PAGE0 | COMMAND_ABORT | COMMAND_STOP
    hw->regs[NE2000_DATA_CONFIG] = DTCFG_FIFO_8 | DTCFG_WIDE | DTCFG_LOOPSEL;
    hw->regs[NE2000_DMA_COUNTER0] = 0;
    hw->regs[NE2000_DMA_COUNTER1] = 0;
    hw->regs[NE2000_TX_CONFIG] = 0x02;
    hw->regs[NE2000_PAGE_START] = RX_BUFFER;
    hw->regs[NE2000_PAGE_STOP] = BUFFER_END;
    hw->regs[NE2000_BOUNDARY] = BUFFER_END - 1;
    hw->regs[NE2000_INT_STATUS] = 0xFF;
    hw->regs[NE2000_INT_MASK] = 0x00;
    hw->regs[NE2000_COMMAND] = 0x61; // COMMAND_PAGE1 | COMMAND_ABORT | COMMAND_STOP
    hw->regs[NE2000_CURRENT_PAGE] = RX_BUFFER;
    hw->regs[NE2000_COMMAND] = 0x21;
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

void GoOnline(struct UnitData *ud)
{
    volatile struct Ne2000 *hw = ud->ud_Hardware;

    HardwareReset(ud);
    WriteHwAddress(ud);
    InstallInterrupt(ud);
    hw->regs[NE2000_COMMAND] = 0x22;
    hw->regs[NE2000_TX_CONFIG] = 0x00;
    hw->regs[NE2000_RX_CONFIG] = RXCFG_BCAST | RXCFG_MCAST |
                                 ((ud->ud_Flags & UF_PROMISCUOUS) ? RXCFG_PROM : 0);
    hw->regs[NE2000_INT_STATUS] = 0xFF;
    ud->ud_IntMasked = FALSE;
    hw->regs[NE2000_INT_MASK] = INTMASK;
    hw_write_barrier(hw, ud);  /* Ensure interrupt mask reaches hardware */

    return;
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

    /* Read two 16-bit words from DMAPort
     * The NE2000 packet header in memory is 4 bytes:
     *   Byte 0: Receive Status
     *   Byte 1: Next Page Pointer
     *   Byte 2: Length Low
     *   Byte 3: Length High
     *
     * DMAPort reads 16-bit words. On big-endian Amiga reading little-endian data:
     * First word contains bytes 1,0 (in big-endian word format: byte1 in high, byte0 in low)
     * Second word contains bytes 3,2
     */
    w0 = ne->DMAPort;  // Contains status and next_page
    w1 = ne->DMAPort;  // Contains length

    /* Extract bytes - the words are already byte-swapped by hardware */
    status = (w0 >> 8) & 0xFF;      // High byte of first word
    next_page = w0 & 0xFF;          // Low byte of first word
    len_lo = (w1 >> 8) & 0xFF;      // High byte of second word
    len_hi = w1 & 0xFF;             // Low byte of second word

    /* Pack into result with proper layout for our code */
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

    for (count = (len + 1) >> 1; count; count--)
    {
        *buffer = ne->DMAPort;
        if (print)
            DBG1_W_NONEWLINE("%04x", *buffer);
        ++buffer;
    }
    if (print)
        DBG("");
    return;
}

///
/// PacketReceived()

/* Reads the next packet from the ring into ud_RxBuffer. Returns the frame length (Ethernet */
/* header + data, without the CRC), or 0 if the ring header is corrupt (nothing is read      */
/* then; the caller must reset the ring).                                                    */
/* No debug output per packet: serial output takes far longer than a packet.                 */

/* The byte count in the ring header is 4 more than the Ethernet frame (header + data).   */
/* Passing those 4 bytes on made a full sized frame look longer than the MTU to the TCP/IP  */
/* stack, which then dropped it: frames of 1511 bytes and more never got through.            */
#define RING_OVERHEAD   4
#define RING_COUNT_MIN  (60 + RING_OVERHEAD)    /* shortest frame the card accepts */
#define RING_COUNT_MAX  (1514 + RING_OVERHEAD)  /* longest Ethernet frame */

LONG PacketReceived(struct UnitData *ud)
{
    ULONG header, count, len;
    UBYTE next_page;

    /* Header layout from GetPacketHeader: status, next page, count low, count high */
    header = GetPacketHeader(ud->ud_Hardware, ud->ud_NextPage, ud);
    next_page = (header >> 8) & 0xFF;
    count = ((header >> 16) & 0xFF) | (((header >> 24) & 0xFF) << 8);

    /* Validate BEFORE reading: the length is used as DMA count into ud_RxBuffer, which */
    /* only holds one maximum sized frame.                                             */
    if (next_page < RX_BUFFER || next_page >= BUFFER_END || count < RING_COUNT_MIN || count > RING_COUNT_MAX)
    {
        DBG1_U("Corrupt ring header $%08lx, resetting ring buffer", header);
        DIAG(DG_RXBADHEADER)++;
        DIAG(DG_LASTBADHEADER) = header;
        return 0;
    }

    len = count - RING_OVERHEAD;
    GetPacket(ud->ud_Hardware, ud->ud_NextPage, len, (UWORD *)ud->ud_RxBuffer, FALSE, ud);
    ud->ud_NextPage = next_page;
    ud->ud_Hardware->regs[NE2000_BOUNDARY] = ud->ud_NextPage == RX_BUFFER ? BUFFER_END - 1 : ud->ud_NextPage - 1;
    return len;
}

///
/// BufferOverflow()

void BufferOverflow(struct UnitData *ud)
{
    struct Library *DOSBase = ud->ud_DOSBase;
    volatile struct Ne2000 *hw = ud->ud_Hardware;
    UBYTE txp, resent = FALSE, intstatus;

    DBG_U("Overflow detected.");
    DIAG(DG_TASKSTATE) = TS_OVERFLOW;

    txp = hw->regs[NE2000_COMMAND] & COMMAND_TXP;
    hw->regs[NE2000_COMMAND] = COMMAND_PAGE0 | COMMAND_ABORT | COMMAND_STOP;

    /* The chip needs up to 1.6 ms to finish a transfer in progress. Called from the unit */
    /* task only (never from the interrupt server), so Delay() is allowed here.            */
    Delay(1);

    hw->regs[NE2000_DMA_COUNTER0] = 0;
    hw->regs[NE2000_DMA_COUNTER1] = 0;

    if (txp)
    {
        intstatus = hw->regs[NE2000_INT_STATUS];
        if (!(intstatus & (INT_TXPACKET | INT_TXERROR)))
            resent = TRUE;
    }

    hw->regs[NE2000_TX_CONFIG] = TXCFG_LOOP_INT;
    hw->regs[NE2000_COMMAND] = COMMAND_PAGE1 | COMMAND_ABORT | COMMAND_START;
    hw->regs[NE2000_CURRENT_PAGE] = RX_BUFFER;
    hw->regs[NE2000_COMMAND] = COMMAND_PAGE0 | COMMAND_ABORT | COMMAND_START;
    hw->regs[NE2000_BOUNDARY] = BUFFER_END - 1;
    ud->ud_NextPage = RX_BUFFER;

    hw->regs[NE2000_TX_CONFIG] = TXCFG_LOOP_NONE;
    if (resent)
        hw->regs[NE2000_COMMAND] = COMMAND_PAGE0 | COMMAND_START |
                                   COMMAND_ABORT | COMMAND_TXP;
    hw->regs[NE2000_INT_STATUS] = INT_OVERFLOW;

    return;
}

///
/// SendPacket()

void SendPacket(struct UnitData *ud, struct IOSana2Req *req)
{
    USE_U(SysBase)
    volatile struct Ne2000 *hw = ud->ud_Hardware;
    UBYTE ethbuffer[1536], *datapointer;
    UWORD *ethdata = (UWORD *)ethbuffer;
    ULONG data_len = req->ios2_DataLength, dma_len;
    UWORD cycles;

    /* If not raw packets, fill in Dst, Src and Type fields of Ethernet frame. 'datapointer' is a vairable */
    /* holding address of data to copy from network stack. If packet is raw, datapointer points to start   */
    /* of ethbuffer, otherwise points to ef_Data field (first byte after Ethernet header.                  */

    if (!(req->ios2_Req.io_Flags & SANA2IOF_RAW))
    {
        struct EthFrame *ef = (struct EthFrame *)ethbuffer;

        CopyMem(req->ios2_DstAddr, ef->ef_DestAddr, 6);
        CopyMem(ud->ud_EtherAddress, ef->ef_SrcAddr, 6);
        ef->ef_Type = req->ios2_PacketType;
        datapointer = ef->ef_Data;
    }
    else
        datapointer = ethbuffer;

    /* Copy data from network stack using supplied CopyFrom() function. */

    ((struct BuffFunctions *)req->ios2_BufferManagement)->bf_CopyFrom(datapointer, req->ios2_Data, data_len);

    /* Now we need length of data to send to hardware. IORequest ios2_DataLength does not include header   */
    /* length if packet is not RAW. So we sholud add it.                                                   */

    if (!(req->ios2_Req.io_Flags & SANA2IOF_RAW))
        data_len += 14;

    /* Packet sent to Ethernet hardware should be at least 60 bytes long (4 bytes of CRC will be appended  */
    /* by hardware giving 64 bytes). If our packet is shorter we should extend it with zeros.             */

    while (data_len < 60)
        ethbuffer[data_len++] = 0;

    /* Now the packet is ready to send it to NIC buffer. It is done by Remote Write DMA command. Firstly   */
    /* write addres and counter should be initialized, then command register.                              */

    /* Receive uses the same remote DMA channel. Both run in the unit task only, so they     */
    /* cannot interleave.                                                                     */

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
    hw->regs[NE2000_DMA_START_ADDR1] = TX_BUFFER;
    hw->regs[NE2000_COMMAND] = COMMAND_PAGE0 | COMMAND_START | COMMAND_WRITE;
    hw_write_barrier(hw, ud);  /* Ensure DMA setup reaches hardware before data transfer */

    /* Now we can send packet data to DMAPort word by word.                                                */

    for (cycles = (data_len + 1) >> 1; cycles; cycles--)
        hw->DMAPort = *ethdata++;

    /* Wait for DMA to complete by checking INT_DMAFINISH flag.                                            */

    /* Bounded: never spin forever if the flag does not show up.                               */

    for (cycles = 10000; cycles && !(hw->regs[NE2000_INT_STATUS] & INT_DMAFINISH); cycles--)
    {
        hw_read_barrier(hw, ud);  /* Ensure we read fresh status, not cached */
    }
    if (!cycles)
    {
        DBG_U("SendPacket(): no DMA finished flag after upload.");
        DIAG(DG_TXDMATIMEOUT)++;
    }
    hw->regs[NE2000_INT_STATUS] = INT_DMAFINISH;

    /* Send packet to the wire. Register setup first.                                                      */

    hw->regs[NE2000_TX_PAGE_START] = TX_BUFFER;
    hw->regs[NE2000_TX_COUNTER0] = data_len & 0xFF;
    hw->regs[NE2000_TX_COUNTER1] = data_len >> 8;

    /* Three, two, one, go! */

    hw->regs[NE2000_COMMAND] = COMMAND_PAGE0 | COMMAND_START | COMMAND_ABORT | COMMAND_TXP;
    hw_write_barrier(hw, ud);  /* Ensure transmit command reaches hardware */

    /* OK. Packet was sent (succesfully or not). Hardware will respond with TXPACKET or TXERROR interrupt  */
    /* then we will be able to reply IORequest in the interrupt server.                                    */

    /* DBG_T("Packet sent."); */
    return;
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
    for (i = 0; i < 6; i++)
        ud->ud_EtherAddress[i] = hw->DMAPort;

    /* Always display hardware address on initialization */
    INIT_DBG_U("Hardware address (read):");
    for (i = 0; i < 6; i++)
        INIT_DBG1_U("\t%02lx", ud->ud_EtherAddress[i]);

    /* Debug function verification with known test values */
    INIT_DBG_U("Debug verification - testing output formats:");
    INIT_DBG1_U("  UBYTE test: 0x%02lx (expected: 0xAB)", (ULONG)0xAB);
    INIT_DBG1_U("  UWORD test: 0x%04lx (expected: 0x1234)", (ULONG)0x1234);
    INIT_DBG1_U("  ULONG test: 0x%08lx (expected: 0xDEADBEEF)", (ULONG)0xDEADBEEF);
    INIT_DBG1_U("  Decimal test: %ld (expected: 1518)", (LONG)1518);
    INIT_DBG1_U("  Test packet size: %ld bytes", (LONG)1518);
    INIT_DBG1_U("  Calculated pages: %ld (expected: 6 pages)", (LONG)((1518 + 4 + 255) >> 8));

    return;
}

///
/// WriteHwAddress()

void WriteHwAddress(struct UnitData *ud)
{
    volatile struct Ne2000 *hw = ud->ud_Hardware;
    WORD i;

#ifdef PDEBUG
    DBG_U("\thardware address (write):");
    for (i = 0; i < 6; i++)
        DBG1_U("\t%02lx", ud->ud_SoftAddress[i]);
#endif

    hw->regs[NE2000_COMMAND] = COMMAND_PAGE1 | COMMAND_ABORT | COMMAND_STOP;
    for (i = 0; i < 6; i++)
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

    hw->regs[NE2000_COMMAND] = COMMAND_PAGE1 | COMMAND_ABORT | COMMAND_START;
    /* Page 1 may only be selected with the card's interrupts masked, see IntCode(). */
    current = hw->regs[NE2000_CURRENT_PAGE];
    hw->regs[NE2000_COMMAND] = COMMAND_PAGE0 | COMMAND_ABORT | COMMAND_START;
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

void FlushQueues(struct UnitData *ud)
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
        IoDone(ud, xreq, IOERR_ABORTED, S2WERR_GENERIC_ERROR);
    }
    ReleaseSemaphore(&ud->ud_RxSem);

    ObtainSemaphore(&ud->ud_TxSem);
    for (;;)
    {
        xreq = (struct IOSana2Req *)RemHead((struct List *)&ud->ud_TxQueue);
        if (!xreq)
            break;
        DBG1_T("<- WRITE [$08%lx] [F].", xreq);
        IoDone(ud, xreq, IOERR_ABORTED, S2WERR_GENERIC_ERROR);
    }
    ReleaseSemaphore(&ud->ud_TxSem);
    return;
}

///
/// StartNextWrite()

/* Starts transmission of the next queued packet unless one is still in flight.            */
/* ud_PendingWrite is only touched by the unit task: set here, cleared in ServiceCard().   */

void StartNextWrite(struct UnitData *ud)
{
    USE_U(SysBase)
    struct IOSana2Req *req;

    if (ud->ud_PendingWrite)
        return;

    ObtainSemaphore(&ud->ud_TxSem);
    req = (struct IOSana2Req *)RemHead((struct List *)&ud->ud_TxQueue);
    ReleaseSemaphore(&ud->ud_TxSem);

    if (req)
    {
        ud->ud_PendingWrite = req;
        SendPacket(ud, req);
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

    if (!(ud->ud_Flags & UF_ONLINE))
        return;

    DIAG(DG_SERVICERUNS)++;

    hw->regs[NE2000_INT_MASK] = 0;
    ud->ud_IntMasked = TRUE;
    hw_write_barrier(hw, ud);
    hw_read_barrier(hw, ud);

    while ((intstatus = (hw->regs[NE2000_INT_STATUS] & INTMASK)))
    {
        DIAG(DG_TASKSTATE) = TS_SERVICE;
        DIAG(DG_LASTINTSTATUS) = intstatus;

        if (intstatus & (INT_TXERROR | INT_TXPACKET))
        {
            hw->regs[NE2000_INT_STATUS] = intstatus & (INT_TXERROR | INT_TXPACKET);
            req = ud->ud_PendingWrite;
            ud->ud_PendingWrite = NULL;
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
                    IoDone(ud, req, OK, OK);
                }
            }
            else
                DIAG(DG_TXNOPENDING)++;
        }

        if (intstatus & INT_OVERFLOW)
        {
            hw->regs[NE2000_INT_STATUS] = INT_OVERFLOW;
            DIAG(DG_OVERFLOWS)++;
            BufferOverflow(ud);
        }

        if (intstatus & INT_RXPACKET)
        {
            ULONG offset, len;
            UWORD mask;
            WORD i;

            /* Clear before draining: a packet arriving meanwhile sets it again */
            hw->regs[NE2000_INT_STATUS] = INT_RXPACKET;
            hw_write_barrier(hw, ud);
            hw_read_barrier(hw, ud);

            while (RingBufferNotEmpty(ud))
            {
                DIAG(DG_TASKSTATE) = TS_RXDRAIN;
                len = PacketReceived(ud);

                /* PacketReceived() returns 0 for a corrupt ring header - reset the ring */
                if (!len)
                {
                    BufferOverflow(ud);
                    break;
                }
                DIAG(DG_RXFRAMES)++;

                if ((req = SearchReadRequest(ud, &ud->ud_RxQueue, ud->ud_RxBuffer[6])))
                {
                    if (req->ios2_Req.io_Flags & SANA2IOF_RAW)
                        offset = 0;
                    else
                        offset = 7;
                    ((struct BuffFunctions *)req->ios2_BufferManagement)->bf_CopyTo(req->ios2_Data, &ud->ud_RxBuffer[offset], len - (offset << 1));
                    CopyMem(&ud->ud_RxBuffer[0], req->ios2_DstAddr, 6);
                    CopyMem(&ud->ud_RxBuffer[3], req->ios2_SrcAddr, 6);
                    req->ios2_DataLength = len - (offset << 1);
                    mask = 0xFFFF;
                    for (i = 2; i >= 0; i--)
                        mask &= ud->ud_RxBuffer[i];
                    if (mask == 0xFFFF)
                        req->ios2_Req.io_Flags |= SANA2IOF_BCAST;
                    ud->ud_DevStats.PacketsReceived++;
                    DIAG(DG_RXDELIVERED)++;
                    IoDone(ud, req, OK, OK);
                }
                else
                {
                    DIAG(DG_RXNOREADER)++;
                    DIAG(DG_LASTDROPTYPE) = ud->ud_RxBuffer[6];
                }
            }
        }

        /* Keep transmitting while a long receive burst is being drained */
        StartNextWrite(ud);
    }

    StartNextWrite(ud);

    /* Unmask. If something became pending since the loop ended the card interrupts at once */
    /* and IntCode() signals us again. Not if S2_OFFLINE took the card down meanwhile.      */
    DIAG(DG_TASKSTATE) = TS_PORT;
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
    USE_UD(DOSBase)
    ULONG signals, sigmask;

#ifdef PDEBUG
    UBYTE wname[60], *ptr;

    ptr = StrCopy(wname, "CON:400/17/400/300/");
    ptr = StrCopy(ptr, ud->ud_Name);
    StrCopy(ptr, "/AUTO/CLOSE/WAIT");
    ud->tdebug = Open(wname, MODE_NEWFILE);
#endif

    sigmask = 1 << port->mp_SigBit;
    for (;;)
    {
        /* DBG_T("Waiting..."); */
        DIAG(DG_TASKSTATE) = TS_WAITING;
        signals = Wait(SIGBREAKF_CTRL_C | sigmask | ud->ud_ServiceMask);
        DIAG(DG_TASKSTATE) = TS_PORT;

        if (signals & ud->ud_ServiceMask)
        {
            ServiceCard(ud);
        }

        if (signals & SIGBREAKF_CTRL_C)
        {
            DBG1_T("TASK: %s task killed.", ud->ud_Name);
            return;
        }

        if (signals & sigmask)
        {
            struct IOSana2Req *req;

            /* DBG_T("port"); */
            while ((req = (struct IOSana2Req *)GetMsg(port)))
            {
                switch (req->ios2_Req.io_Command)
                {
                case CMD_READ:
                    /* DBG1_T("-> READ [$%08lx].", req); */
                    DIAG(DG_READSQUEUED)++;
                    ObtainSemaphore(&ud->ud_RxSem);
                    AddTail((struct List *)&ud->ud_RxQueue, (struct Node *)req);
                    ReleaseSemaphore(&ud->ud_RxSem);
                    break;

                case S2_BROADCAST:
                case CMD_WRITE:
                    /* DBG1_T("-> WRITE [$%08lx].", req); */
                    DIAG(DG_WRITESQUEUED)++;
                    ObtainSemaphore(&ud->ud_TxSem);
                    AddTail((struct List *)&ud->ud_TxQueue, (struct Node *)req);
                    ReleaseSemaphore(&ud->ud_TxSem);
                    StartNextWrite(ud);
                    break;

                case CMD_FLUSH:
                    DBG1_T("-> FLUSH [$%08lx].", req);
                    FlushQueues(ud);
                    DBG1_T("<- FLUSH [$%08lx].", req);
                    IoDone(ud, req, OK, OK);
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
