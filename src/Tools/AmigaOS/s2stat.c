/* s2stat - print the diagnostic counters of openpci-rtl8029.device
 *
 * Usage: s2stat [unit]
 *
 * Opens the device non-exclusively next to the TCP/IP stack and issues
 * S2_GETSPECIALSTATS. The driver answers this in the caller's context, so it
 * works even when the driver's unit task is stuck.
 *
 * Copyright (c) 2026 Steen Lund
 * SPDX-License-Identifier: MIT (see licenses/MIT.txt)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <exec/types.h>
#include <exec/io.h>
#include <exec/ports.h>
#include <devices/sana2.h>
#include <utility/tagitem.h>
#include <proto/exec.h>

#define MAX_RECORDS 64

/* Private OpenDevice() flag of openpci-rtl8029.device: query-only open, allowed even when
 * the TCP/IP stack has the unit open exclusively. Must match the driver. */
#define OPENF_MONITOR 0x80000000UL

static const char *const task_states[] = {
    "waiting", "handling requests", "servicing card", "reading ring",
    "sending packet", "overflow recovery"
};

/* Required by the device at open time, never called: we send no packets. */
static ULONG dummy_copy(void)
{
    return 0;
}

static struct
{
    struct Sana2SpecialStatHeader header;
    struct Sana2SpecialStatRecord records[MAX_RECORDS];
} stats;

static UBYTE printed[MAX_RECORDS];

/* The counters by group, in the order they are printed. Looked up by name, so the driver
 * may report them in any order; anything not listed here is printed under "Other". */
static const char *const group_unit[] = {
    "TaskState", "TaskPri", "MMUInhibit", "RtlConfig", NULL };
static const char *const group_requests[] = {
    "ReadsQueued", "WritesQueued", NULL };
static const char *const group_card[] = {
    "Interrupts", "ServiceRuns", "IntSeen", "LastIntStatus", "NextPage", "LastCurrent",
    "TxPending", NULL };
static const char *const group_rx[] = {
    "RxFrames", "RxDelivered", "RxNoReader", "LastDropType", "RxWrapped", NULL };
static const char *const group_rxerr[] = {
    "Overflows", "RxMissed", "RxCrcErrors", "RxAlignErrors", "RxErrInts", "LastRxStatus",
    "RxBadHeader", "LastBadHeader", NULL };
static const char *const group_tx[] = {
    "TxStarted", "TxDone", "TxWireIdle", "TxIdleMsgWait", "TxWaitPolls", NULL };
static const char *const group_txerr[] = {
    "TxErrors", "TxCollisions", "TxLateColl", "TxCarrierLost", "TxNoPending",
    "TxDmaTimeout", NULL };
static const char *const group_sums[] = {
    "RxSumChecked", "RxSumBad", "RxSumBadWrap", "LastSumBad", "RxEchoReq", NULL };
static const char *const group_timing[] = {
    "EClockFreq", "TicksService", "TicksRxRead", "TicksRxCopy", "TicksTx", "TicksTxCopy",
    "TicksTxUpload", "TicksTxWait", "TicksIntLat", "MaxTicksIntLat", "SlowIntLat",
    "MaxTicksTurn", "SlowTurn", NULL };

/* Groups that only a driver built with an option fills in; the record named in 'present'
 * is non-zero when it was. */
static const struct
{
    const char *title;
    const char *const *names;
    const char *present;
    const char *absent;
} groups[] = {
    { "Unit task and card setup", group_unit,     NULL, NULL },
    { "Requests from the stack",  group_requests, NULL, NULL },
    { "Interrupts and card state", group_card,    NULL, NULL },
    { "Receive",                  group_rx,       NULL, NULL },
    { "Receive errors",           group_rxerr,    NULL, NULL },
    { "Transmit",                 group_tx,       NULL, NULL },
    { "Transmit errors",          group_txerr,    NULL, NULL },
    { "Receive checksums",        group_sums,     "RxSumChecked",
      "none verified (needs a driver built with RX_VERIFY_CHECKSUMS 1)" },
    { "Timing",                   group_timing,   "EClockFreq",
      "not measured (needs a driver built with make PERF=1)" },
};

/* Index of a named record, -1 if the driver does not supply it */
static LONG find_record(const char *name)
{
    ULONG i;

    for (i = 0; i < stats.header.RecordCountSupplied; i++)
    {
        if (!strcmp((const char *)stats.records[i].String, name))
            return (LONG)i;
    }
    return -1;
}

/* Value of a named counter, 0 if the driver does not supply it */
static ULONG stat_value(const char *name)
{
    LONG i = find_record(name);

    return (i >= 0) ? stats.records[i].Count : 0;
}

/* E-clock ticks to microseconds without overflowing 32 bits */
static ULONG ticks_to_us(ULONG ticks, ULONG eclock)
{
    return (ticks / eclock) * 1000000 + ((ticks % eclock) * 1000) / (eclock / 1000);
}

static void print_record(ULONG i, ULONG eclock)
{
    const char *name = (const char *)stats.records[i].String;
    ULONG value = stats.records[i].Count;

    printed[i] = 1;
    printf("  %-14s %10lu  $%08lx", name, (unsigned long)value, (unsigned long)value);
    if (!strcmp(name, "TaskState") && value < sizeof(task_states) / sizeof(task_states[0]))
        printf("  (%s)", task_states[value]);

    /* A single duration */
    if (!strncmp(name, "MaxTicks", 8) && eclock)
        printf("  %lu us", ticks_to_us(value, eclock));

    /* Time counters: total in milliseconds and the average per packet */
    if (!strncmp(name, "Ticks", 5) && eclock)
    {
        BOOL tx = !strncmp(name, "TicksTx", 7);
        BOOL intr = !strcmp(name, "TicksIntLat");
        ULONG count = stat_value(tx ? "TxStarted" : intr ? "Interrupts" : "RxFrames");

        printf("  %lu ms", ticks_to_us(value, eclock) / 1000);
        if (count)
            printf(", %lu us per %s", ticks_to_us(value / count, eclock),
                   tx ? "sent packet" : intr ? "interrupt" : "received packet");
    }
    printf("\n");
}

static void print_stats(void)
{
    ULONG eclock = stat_value("EClockFreq");
    ULONG g, i;
    BOOL other = FALSE;

    memset(printed, 0, sizeof(printed));
    for (g = 0; g < sizeof(groups) / sizeof(groups[0]); g++)
    {
        const char *const *n;

        if (groups[g].present && !stat_value(groups[g].present))
        {
            /* Mark them printed so they do not turn up under "Other" */
            for (n = groups[g].names; *n; n++)
            {
                LONG r = find_record(*n);
                if (r >= 0)
                    printed[r] = 1;
            }
            printf("%s: %s\n\n", groups[g].title, groups[g].absent);
            continue;
        }

        printf("%s\n", groups[g].title);
        for (n = groups[g].names; *n; n++)
        {
            LONG r = find_record(*n);
            if (r >= 0)
                print_record((ULONG)r, eclock);
        }
        printf("\n");
    }

    for (i = 0; i < stats.header.RecordCountSupplied; i++)
    {
        if (!printed[i])
        {
            if (!other)
                printf("Other\n");
            other = TRUE;
            print_record(i, eclock);
        }
    }
}

int main(int argc, char **argv)
{
    static const char *const names[] = {
        "openpci-rtl8029.device",
        "Networks/openpci-rtl8029.device",
        "DEVS:Networks/openpci-rtl8029.device"
    };
    struct TagItem tags[3];
    struct MsgPort *port;
    struct IOSana2Req *req;
    LONG unit = (argc > 1) ? atol(argv[1]) : 0;
    ULONG i;
    int opened = 0, rc = 10;

    tags[0].ti_Tag = S2_CopyToBuff;
    tags[0].ti_Data = (ULONG)dummy_copy;
    tags[1].ti_Tag = S2_CopyFromBuff;
    tags[1].ti_Data = (ULONG)dummy_copy;
    tags[2].ti_Tag = TAG_END;
    tags[2].ti_Data = 0;

    if (!(port = CreateMsgPort()))
        return 20;

    if ((req = (struct IOSana2Req *)CreateIORequest(port, sizeof(struct IOSana2Req))))
    {
        for (i = 0; i < 3 && !opened; i++)
        {
            req->ios2_BufferManagement = tags;
            if (OpenDevice((STRPTR)names[i], unit, (struct IORequest *)req, OPENF_MONITOR) == 0)
                opened = 1;
        }

        if (opened)
        {
            stats.header.RecordCountMax = MAX_RECORDS;
            req->ios2_Req.io_Command = S2_GETSPECIALSTATS;
            req->ios2_StatData = &stats;

            if (DoIO((struct IORequest *)req) == 0)
            {
                print_stats();
                rc = 0;
            }
            else
                printf("S2_GETSPECIALSTATS failed: error %ld, wire error %lu\n",
                       (long)req->ios2_Req.io_Error, (unsigned long)req->ios2_WireError);

            CloseDevice((struct IORequest *)req);
        }
        else
            printf("Cannot open openpci-rtl8029.device unit %ld\n", (long)unit);

        DeleteIORequest((struct IORequest *)req);
    }
    DeleteMsgPort(port);
    return rc;
}
