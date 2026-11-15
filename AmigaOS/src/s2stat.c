/* s2stat - print the diagnostic counters of openpci-rtl8029.device
 *
 * Usage: s2stat [unit]
 *
 * Opens the device non-exclusively next to the TCP/IP stack and issues
 * S2_GETSPECIALSTATS. The driver answers this in the caller's context, so it
 * works even when the driver's unit task is stuck.
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

#define MAX_RECORDS 40

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

/* Value of a named counter, 0 if the driver does not supply it */
static ULONG stat_value(const char *name)
{
    ULONG i;

    for (i = 0; i < stats.header.RecordCountSupplied; i++)
    {
        if (!strcmp((const char *)stats.records[i].String, name))
            return stats.records[i].Count;
    }
    return 0;
}

/* E-clock ticks to microseconds without overflowing 32 bits */
static ULONG ticks_to_us(ULONG ticks, ULONG eclock)
{
    return (ticks / eclock) * 1000000 + ((ticks % eclock) * 1000) / (eclock / 1000);
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
                ULONG eclock = stat_value("EClockFreq");

                for (i = 0; i < stats.header.RecordCountSupplied; i++)
                {
                    const char *name = (const char *)stats.records[i].String;
                    ULONG value = stats.records[i].Count;

                    printf("%-14s %10lu  $%08lx", name, (unsigned long)value, (unsigned long)value);
                    if (!strcmp(name, "TaskState") && value < sizeof(task_states) / sizeof(task_states[0]))
                        printf("  (%s)", task_states[value]);

                    /* Time counters: total in milliseconds and the average per packet */
                    if (!strncmp(name, "Ticks", 5) && eclock)
                    {
                        ULONG packets = stat_value(strcmp(name, "TicksTx") ? "RxFrames" : "TxStarted");

                        printf("  %lu ms", ticks_to_us(value, eclock) / 1000);
                        if (packets)
                            printf(", %lu us per %s packet", ticks_to_us(value / packets, eclock),
                                   strcmp(name, "TicksTx") ? "received" : "sent");
                    }
                    printf("\n");
                }
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
