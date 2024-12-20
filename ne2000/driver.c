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

#define OK 0

#define TX_BUFFER   0x40
#define RX_BUFFER   0x46
#define BUFFER_END  0x80
#define INTMASK     (INT_RXPACKET | INT_TXPACKET | INT_TXERROR)

/* PCI card ID's */

#define PCI_VENDOR_REALTEK 0x10EC
#define PCI_DEVICE_RTL8029 0x8029

// Unit flags.

#define UF_CONFIGURED   0x01
#define UF_ONLINE       0x02
#define UF_PROMISCUOUS  0x04

// Macro for registerized parameters (used in some OS functions).

#define reg(a) __asm(#a)

// Macro for declaring local libraries bases.

#define USE(a) struct Library *a = dd->dd_##a;
#define USE_U(a) struct Library *a = ud->ud_##a;

// Macros for debug messages.

#ifdef PDEBUG
#include <proto/debug.h>

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
#define DBG_U(a)
#define DBG_T(a)
#define DBG1(a, b)
#define DBG1_U(a, b)
#define DBG1_T(a, b)
#define DBG2(a, b, c)
#define DBG2_U(a, b, c)
#define DBG2_T(a, b, c)
#endif

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
    ULONG ud_GoWriteMask;
    UWORD ud_RxBuffer[768];
    UBYTE ud_Name[27];
    UBYTE ud_EtherAddress[6];
    UBYTE ud_SoftAddress[6];
    struct Sana2DeviceStats ud_DevStats;
    UBYTE ud_Flags;
    UBYTE ud_NextPage;
    BYTE ud_GoWriteBit;
    UBYTE pad;
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

void HardwareReset(struct UnitData *ud);
void HardwareInit(struct UnitData *ud);
volatile struct Ne2000 *FindHardware(struct DevData *dd, WORD unit, struct UnitData *ud);
void GoOnline(struct UnitData *ud);
void GoOffline(struct UnitData *ud);
void GetHwAddress(struct UnitData *ud);
void WriteHwAddress(struct UnitData *ud);
LONG PacketReceived(struct UnitData *ud);
LONG RingBufferNotEmpty(struct UnitData *ud);

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
        S2_ONLINE,
        S2_OFFLINE,
        NSCMD_DEVICEQUERY,
        0};

///

/* Good enough */
unsigned char *strcpy(unsigned char *dest, const unsigned char *src)
{
    unsigned char *d = dest;
    while (*src)
        *dest++ = *src++;
    *dest = 0;
    return d;
}

/// DevInit()
// Called when the device is loaded into memory. Makes system library, initializes Library structure, opens
// libraries used by the device. Returns device base or NULL if init failed.

struct DevData *DevInit(register APTR seglist reg(a0), register struct Library *sysb reg(a6))
{
    struct DevData *dd;
    struct Library *SysBase = sysb;

    if (dd = (struct DevData *)MakeLibrary(FuncTable, NULL, NULL, sizeof(struct DevData), 0))
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
/*strcpy(dd->dpath, "KCON:0/17/400/300/openpci-rtl8029.device (main)/AUTO/CLOSE/WAIT");*/
/*GetVar("PrometheusDebug", dd->dpath, 128, 0);*/
/*dd->debug = Open(dd->dpath, MODE_NEWFILE);*/
#endif

            DBG1("Device initialized, base at $%08lx.", dd);
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

    case CMD_READ:
        DBG1("CMD_READ [$%08lx].", (LONG)req);
        req->ios2_Req.io_Flags &= ~IOF_QUICK;
        PutMsg(ud->ud_TaskPort, &req->ios2_Req.io_Message);
        break;

    case S2_BROADCAST:
        for (i = 0; i < 6; i++)
            req->ios2_DstAddr[i] = 0xFF;
    case CMD_WRITE:
        DBG1("CMD_WRITE [$%08lx].", (LONG)req);
        req->ios2_Req.io_Flags &= ~IOF_QUICK;
        PutMsg(ud->ud_TaskPort, &req->ios2_Req.io_Message);
        break;

    case CMD_FLUSH:
        DBG1("CMD_FLUSH [$%08lx].", (LONG)req);
        req->ios2_Req.io_Flags &= ~IOF_QUICK;
        PutMsg(ud->ud_TaskPort, &req->ios2_Req.io_Message);
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
    struct SignalSemaphore *sem;
    struct MinNode *node;

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

    if (ud->ud_LifeTime = CreateMsgPort())
    {
        if (ud->ud_Task = (struct Task *)CreateNewProcTags(
                NP_Entry, (LONG)UnitTask,
                NP_Name, (LONG)ud->ud_Name,
                NP_Priority, 6,
                TAG_END))
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

    /* Check exclusive flag - reject if already opened by someone else. */

    if ((flags & SANA2OPF_MINE) && ud->ud_OpenCnt)
        return NULL;

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
            strcpy(ud->ud_Name, "openpci-rtl8029.device (x)");
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
            HardwareReset(ud);
            HardwareInit(ud);
            GetHwAddress(ud);
            if (RunTask(dd, ud))
            {
                dd->dd_Units[unit] = ud;
                DBG1("%s initialized.", (LONG)ud->ud_Name);
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
    WORD unit;

    if (ud)
    {
        unit = ud->ud_Name[24] - '0';
        if (ud->ud_Flags & UF_ONLINE)
            GoOffline(ud);
        if (ud->ud_Task)
            KillTask(dd, ud);
        FreeMem(ud, sizeof(struct UnitData));
        dd->dd_Units[unit] = NULL;
        DBG1("%s expunged.", ud->ud_Name);
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
        GoOffline(ud);
        ud->ud_Flags &= ~UF_ONLINE;
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

LONG IntCode(register struct UnitData *ud reg(a1))
{
    USE_U(SysBase)
    UBYTE intstatus;
    struct IOSana2Req *req;
    LONG my_int = 0;

    while ((intstatus = (ud->ud_Hardware->regs[NE2000_INT_STATUS] & INTMASK)))
    {
        if (intstatus & INT_TXERROR)
        {
            ud->ud_Hardware->regs[NE2000_INT_STATUS] = INT_TXERROR;
            req = ud->ud_PendingWrite;
            IoDone(ud, req, S2ERR_TX_FAILURE, S2WERR_TOO_MANY_RETIRES);
            Signal(ud->ud_Task, ud->ud_GoWriteMask);
            my_int = 1;
        }

        if (intstatus & INT_TXPACKET)
        {
            ud->ud_Hardware->regs[NE2000_INT_STATUS] = INT_TXPACKET;
            req = ud->ud_PendingWrite;
            ud->ud_DevStats.PacketsSent++;
            IoDone(ud, req, OK, OK);
            Signal(ud->ud_Task, ud->ud_GoWriteMask);
            my_int = 1;
        }

        if (intstatus & INT_RXPACKET)
        {
            ULONG offset, len;
            UWORD mask = 0xFFFF;
            WORD i;

            while (RingBufferNotEmpty(ud))
            {
                ud->ud_Hardware->regs[NE2000_INT_STATUS] = INT_RXPACKET;
                len = PacketReceived(ud);
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
                    for (i = 2; i >= 0; i--)
                        mask &= ud->ud_RxBuffer[i];
                    if (mask == 0xFFFF)
                        req->ios2_Req.io_Flags |= SANA2IOF_BCAST;
                    ud->ud_DevStats.PacketsReceived++;
                    IoDone(ud, req, OK, OK);
                }
            }
            my_int = 1;
        }
    }
    return my_int;
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
        board = FindBoardTagList(board, (struct TagItem *)tags);

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
    hw->regs[NE2000_INT_MASK] = INTMASK;

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

ULONG GetPacketHeader(volatile struct Ne2000 *ne, UBYTE page)
{
    union
    {
        ULONG l;   // 32-bit access to 16-bit data
        UWORD w[2];
    } hdr;

    ne->regs[NE2000_DMA_COUNTER0] = 4;
    ne->regs[NE2000_DMA_COUNTER1] = 0;
    ne->regs[NE2000_DMA_START_ADDR0] = 0;
    ne->regs[NE2000_DMA_START_ADDR1] = page;
    ne->regs[NE2000_COMMAND] = COMMAND_PAGE0 | COMMAND_START | COMMAND_READ;
    hdr.w[0] = ne->DMAPort;
    hdr.w[1] = ne->DMAPort;

#if PDEBUG
    /* Ignore multicast packets*/
    if (!(hdr.w[0] & 0x2000))
        DBG1("GetPacketHeader(): header = $%08lx.", hdr.l);

#endif

    return hdr.l;
}

///
/// GetPacket()

void GetPacket(volatile struct Ne2000 *ne, UBYTE startpage, UWORD len, UWORD *buffer, BOOL print)
{
    UWORD count;

    ne->regs[NE2000_DMA_COUNTER0] = len & 0xFF;
    ne->regs[NE2000_DMA_COUNTER1] = len >> 8;
    ne->regs[NE2000_DMA_START_ADDR0] = 4;
    ne->regs[NE2000_DMA_START_ADDR1] = startpage;
    ne->regs[NE2000_COMMAND] = COMMAND_PAGE0 | COMMAND_START | COMMAND_READ;

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

LONG PacketReceived(struct UnitData *ud)
{
    ULONG header, len;

    header = GetPacketHeader(ud->ud_Hardware, ud->ud_NextPage);
    BOOL mc = (header & 0x20000000) != 0;
    if (!mc)
        DBG1("Header: $%08lx.", header);
    len = swapw(header & 0xFFFF);
    if (!mc)
        DBG1("Packet length: %ld.", len);
    GetPacket(ud->ud_Hardware, ud->ud_NextPage, len, (UWORD *)ud->ud_RxBuffer, !mc);
    ud->ud_Hardware->regs[NE2000_BOUNDARY] = ud->ud_NextPage;
    ud->ud_NextPage = (header >> 16) & 0xFF;
    if (!mc)
        DBG1("Next page: $%02lx.", ud->ud_NextPage);
    ud->ud_Hardware->regs[NE2000_INT_STATUS] = INT_RXPACKET;
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

    txp = hw->regs[NE2000_COMMAND] & COMMAND_TXP;
    hw->regs[NE2000_COMMAND] = COMMAND_PAGE0 | COMMAND_ABORT | COMMAND_STOP;
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
    ULONG data_len = req->ios2_DataLength;
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

    /* Packet sent to Ethernet hadrware should be at least 60 bytes long (4 bytes of CRC will be appended  */
    /* by hardware giving 64 bytes). If our packet is shorter we should extend it with spaces.             */

    while (data_len < 60)
        ethbuffer[data_len++] = ' ';

    /* Now the packet is ready to send it to NIC buffer. It is done by Remote Write DMA command. Firstly   */
    /* write addres and counter should be initialized, then command register.                              */

    hw->regs[NE2000_DMA_COUNTER0] = data_len & 0xFF;
    hw->regs[NE2000_DMA_COUNTER1] = data_len >> 8;
    hw->regs[NE2000_DMA_START_ADDR0] = 0;
    hw->regs[NE2000_DMA_START_ADDR1] = TX_BUFFER;
    hw->regs[NE2000_COMMAND] = COMMAND_PAGE0 | COMMAND_START | COMMAND_WRITE;

    /* Now we can send packet data to DMAPort word by word.                                                */

    for (cycles = (data_len + 1) >> 1; cycles; cycles--)
        hw->DMAPort = *ethdata++;

    /* Send packet to the wire. Register setup first.                                                      */

    hw->regs[NE2000_TX_PAGE_START] = TX_BUFFER;
    hw->regs[NE2000_TX_COUNTER0] = data_len & 0xFF;
    hw->regs[NE2000_TX_COUNTER1] = data_len >> 8;

    /* Three, two, one, go! */

    hw->regs[NE2000_COMMAND] = COMMAND_PAGE0 | COMMAND_START | COMMAND_ABORT | COMMAND_TXP;

    /* OK. Packet was sent (succesfully or not). Hardware will respond with TXPACKET or TXERROR interrupt  */
    /* then we will be able to reply IORequest in the interrupt server.                                    */

    DBG_T("Packet sent.");
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

#ifdef PDEBUG
    DBG_U("\thardware address (read):");
    for (i = 0; i < 6; i++)
        DBG1_U("\t%02lx", ud->ud_EtherAddress[i]);
#endif

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
    current = hw->regs[NE2000_CURRENT_PAGE];
    hw->regs[NE2000_COMMAND] = COMMAND_PAGE0 | COMMAND_ABORT | COMMAND_START;

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
/// MainLoop()

void MainLoop(struct UnitData *ud, struct MsgPort *port)
{
    USE_U(SysBase)
    USE_UD(DOSBase)
    ULONG signals, sigmask;
    struct IOSana2Req *req;

#ifdef PDEBUG
    UBYTE wname[60], *ptr;

    ptr = strcpy(wname, "CON:400/17/400/300/");
    ptr = strcpy(ptr, ud->ud_Name);
    strcpy(ptr, "/AUTO/CLOSE/WAIT");
    ud->tdebug = Open(wname, MODE_NEWFILE);
#endif

    sigmask = 1 << port->mp_SigBit;
    for (;;)
    {
        DBG_T("Waiting...");
        signals = Wait(SIGBREAKF_CTRL_C | sigmask | ud->ud_GoWriteMask);

        if (signals & ud->ud_GoWriteMask)
        {
            DBG_T("GoWrite");
            ObtainSemaphore(&ud->ud_TxSem);
            req = (struct IOSana2Req *)RemHead((struct List *)&ud->ud_TxQueue);
            ReleaseSemaphore(&ud->ud_TxSem);
            ud->ud_PendingWrite = req;
            if (req)
                SendPacket(ud, req);
        }

        if (signals & SIGBREAKF_CTRL_C)
        {
            DBG1_T("TASK: %s task killed.", ud->ud_Name);
            return;
        }

        if (signals & sigmask)
        {
            struct IOSana2Req *req;

            DBG_T("port");
            while (req = (struct IOSana2Req *)GetMsg(port))
            {
                switch (req->ios2_Req.io_Command)
                {
                case CMD_READ:
                    DBG1_T("-> READ [$%08lx].", req);
                    ObtainSemaphore(&ud->ud_RxSem);
                    AddTail((struct List *)&ud->ud_RxQueue, (struct Node *)req);
                    ReleaseSemaphore(&ud->ud_RxSem);
                    break;

                case S2_BROADCAST:
                case CMD_WRITE:
                    DBG1_T("-> WRITE [$%08lx].", req);
                    if (ud->ud_PendingWrite)
                    {
                        ObtainSemaphore(&ud->ud_TxSem);
                        AddTail((struct List *)&ud->ud_TxQueue, (struct Node *)req);
                        ReleaseSemaphore(&ud->ud_TxSem);
                    }
                    else
                    {
                        ud->ud_PendingWrite = req;
                        SendPacket(ud, req);
                        DBG_T("Packet sent 2.");
                    }
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
    struct Library *SysBase = *(struct Library **)4;
    struct Task *task;
    struct MsgPort *port;
    struct UnitData *ud;

    task = FindTask(NULL);
    if (port = CreateMsgPort())
    {
        port->mp_Node.ln_Name = task->tc_Node.ln_Name;
        port->mp_Node.ln_Pri = 20;
        AddPort(port);
        WaitPort(port);
        RemPort(port);
        ud = (struct UnitData *)GetMsg(port);
        if ((ud->ud_GoWriteBit = AllocSignal(-1)) != -1)
        {
            ud->ud_GoWriteMask = 1 << ud->ud_GoWriteBit;
            MainLoop(ud, port);
            FreeSignal(ud->ud_GoWriteBit);
        }
        DeleteMsgPort(port);
        Forbid();
        ReplyMsg(&ud->ud_Message);
    }
    return;
}

///
