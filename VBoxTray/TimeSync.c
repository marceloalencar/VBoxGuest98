/** @file
 * VBoxTray - Guest clock synchronization.
 *
 * Windows 98 has no way to slew the clock (SetSystemTimeAdjustment is NT
 * only), so the clock is set outright when it drifts too far from the host.
 * A sync is done at startup, every interval and right after the VM is
 * restored from a saved state (VMMDEV_EVENT_RESTORED).
 *
 * The host can change the settings with the official VBoxService options
 * (HostOpts.c): --timesync-*, and the global --interval (seconds) and
 * --verbose as defaults for --timesync-interval and --timesync-verbosity.
 * --timesync-set-threshold is accepted but has no effect: every correction
 * sets the clock.
 */

#include "VBoxTray.h"
#include <stddef.h>
#include <string.h>


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/

/** How often to check for shutdown (ms). */
#define VBOXTIMESYNC_POLL_MS        1000
/** Host time queries slower than the maximum latency are tried this many times. */
#define VBOXTIMESYNC_MAX_TRIES      3
/** Milliseconds between 1601-01-01 (FILETIME epoch) and 1970-01-01 (host time epoch). */
#define VBOXTIMESYNC_EPOCH_DIFF_MS  11644473600000ui64
/** The options for this service. */
#define VBOXTIMESYNC_OPT_PATTERNS   VBOXHOSTOPTS_PREFIX "--timesync-*|" VBOXHOSTOPTS_PREFIX "--interval|" \
                                    VBOXHOSTOPTS_PREFIX "--verbose"
/** --timesync-verbosity not set. */
#define VBOXTIMESYNC_NOT_SET        0xffffffff


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/

/** Synchronization settings (the VBoxService --timesync-* options). */
typedef struct VBOXTIMESYNCOPTS
{
    /** Interval between synchronizations (ms). */
    DWORD cMsInterval;
    /** Smallest difference that is corrected (ms). */
    DWORD cMsMinAdjust;
    /** The latency times this factor is also a smallest difference. */
    DWORD cLatencyFactor;
    /** Host time queries slower than this are discarded (ms). */
    DWORD cMsMaxLatency;
    /** Accepted, no effect (there is no slewing). */
    DWORD cMsSetThreshold;
    /** The first sync sets the clock whatever the difference. */
    DWORD fSetStart;
    /** A sync after a restore sets the clock whatever the difference; without
     *  it the restore is not handled specially. */
    DWORD fSetOnRestore;
    /** 1 logs every sync, 2 also the discarded queries. */
    DWORD uVerbosity;
    /** --interval (s), the default interval; 0 if not set. */
    DWORD cSecInterval;
    /** --verbose, verbosity 1 by default. */
    DWORD fVerbose;
} VBOXTIMESYNCOPTS;


/*********************************************************************************************************************************
*   Global Variables                                                                                                             *
*********************************************************************************************************************************/

/** The defaults: upstream's, except a 1 s minimum adjustment (upstream 100 ms)
 *  because every correction here is a jump of the clock. */
static const VBOXTIMESYNCOPTS g_TimeSyncDefaults =
{
    10000, 1000, 8, 250, 20 * 60 * 1000, FALSE, TRUE, 0, 0, FALSE
};

static const VBOXHOSTOPT g_aTimeSyncOpts[] =
{
    { "--interval",                   offsetof(VBOXTIMESYNCOPTS, cSecInterval),    TRUE,  1,  0xffffffff / 1000 - 1 },
    { "--verbose",                    offsetof(VBOXTIMESYNCOPTS, fVerbose),        FALSE, TRUE,  0 },
    { "--timesync-interval",          offsetof(VBOXTIMESYNCOPTS, cMsInterval),     TRUE,  50, 0xfffffffe },
    { "--timesync-min-adjust",        offsetof(VBOXTIMESYNCOPTS, cMsMinAdjust),    TRUE,  0,  3600000 },
    { "--timesync-latency-factor",    offsetof(VBOXTIMESYNCOPTS, cLatencyFactor),  TRUE,  1,  1024 },
    { "--timesync-max-latency",       offsetof(VBOXTIMESYNCOPTS, cMsMaxLatency),   TRUE,  1,  3600000 },
    { "--timesync-set-threshold",     offsetof(VBOXTIMESYNCOPTS, cMsSetThreshold), TRUE,  0,  7 * 24 * 3600000 },
    { "--timesync-set-start",         offsetof(VBOXTIMESYNCOPTS, fSetStart),       FALSE, TRUE,  0 },
    { "--timesync-no-set-start",      offsetof(VBOXTIMESYNCOPTS, fSetStart),       FALSE, FALSE, 0 },
    { "--timesync-set-on-restore",    offsetof(VBOXTIMESYNCOPTS, fSetOnRestore),   FALSE, TRUE,  0 },
    { "--timesync-no-set-on-restore", offsetof(VBOXTIMESYNCOPTS, fSetOnRestore),   FALSE, FALSE, 0 },
    { "--timesync-verbosity",         offsetof(VBOXTIMESYNCOPTS, uVerbosity),      TRUE,  0,  255 }
};

/** The option list last read. */
static VBOXHOSTOPTSCACHE g_TimeSyncOptsCache;


/**
 * Formats a signed number of milliseconds as seconds ("-0.250").
 */
static void vboxTimeSyncFormatMs(char *pszBuf, __int64 cMs)
{
    UINT64 cMsAbs = cMs < 0 ? (UINT64)-cMs : (UINT64)cMs;
    wsprintf(pszBuf, "%s%lu.%03lu", cMs < 0 ? "-" : "", (unsigned long)(cMsAbs / 1000), (unsigned long)(cMsAbs % 1000));
}

/**
 * Reads the host options (HostOpts.c).
 *
 * @param   pOpts       The current settings, updated.
 */
static void vboxTimeSyncReadOpts(HANDLE hDevice, UINT32 idClient, VBOXTIMESYNCOPTS *pOpts)
{
    VBOXTIMESYNCOPTS Opts = g_TimeSyncDefaults;

    /* The interval and verbosity fall back on --interval and --verbose. */
    Opts.cMsInterval = 0;
    Opts.uVerbosity  = VBOXTIMESYNC_NOT_SET;
    if (!VBoxHostOptsRead(hDevice, idClient, "TimeSync", VBOXTIMESYNC_OPT_PATTERNS, g_aTimeSyncOpts,
                          sizeof(g_aTimeSyncOpts) / sizeof(g_aTimeSyncOpts[0]), &Opts, &g_TimeSyncOptsCache))
        return;
    if (!Opts.cMsInterval)
        Opts.cMsInterval = Opts.cSecInterval ? Opts.cSecInterval * 1000 : g_TimeSyncDefaults.cMsInterval;
    if (Opts.uVerbosity == VBOXTIMESYNC_NOT_SET)
        Opts.uVerbosity = Opts.fVerbose ? 1 : 0;

    if (memcmp(&Opts, pOpts, sizeof(Opts)))
    {
        VBoxTrayLogRel("TimeSync: interval %lu ms, min-adjust %lu ms, latency-factor %lu, max-latency %lu ms, "
                       "set-start %s, set-on-restore %s, verbosity %lu\n",
                       Opts.cMsInterval, Opts.cMsMinAdjust, Opts.cLatencyFactor, Opts.cMsMaxLatency,
                       Opts.fSetStart ? "yes" : "no", Opts.fSetOnRestore ? "yes" : "no", Opts.uVerbosity);
        *pOpts = Opts;
    }
}

/**
 * Compares the guest clock with the host and corrects it if needed.
 *
 * @param   hDevice     The device handle.
 * @param   pOpts       The settings.
 * @param   fForce      Set the clock whatever the difference.
 */
static void vboxTimeSyncOnce(HANDLE hDevice, const VBOXTIMESYNCOPTS *pOpts, BOOL fForce)
{
    ULARGE_INTEGER uGuest;
    ULARGE_INTEGER uHost;
    FILETIME ftGuest;
    FILETIME ftHost;
    SYSTEMTIME stHost;
    UINT64 msHost;
    UINT64 msGuest;
    UINT64 cMsMinAdjust;
    UINT64 cMsAbsDiff;
    __int64 cMsDiff;
    DWORD cMsLatency;
    unsigned iTry;
    char szDiff[32];

    for (iTry = 1; ; iTry++)
    {
        DWORD msStart = GetTickCount();
        if (RT_FAILURE(VbglR3GetHostTime(hDevice, &msHost)))
            return;
        GetSystemTimeAsFileTime(&ftGuest);
        cMsLatency = GetTickCount() - msStart;
        if (cMsLatency <= pOpts->cMsMaxLatency)
            break;
        if (pOpts->uVerbosity >= 2)
            VBoxTrayLogRel("TimeSync: host time query took %lu ms, discarded\n", cMsLatency);
        if (iTry >= VBOXTIMESYNC_MAX_TRIES)
        {
            if (pOpts->uVerbosity >= 1)
                VBoxTrayLogRel("TimeSync: host time queries too slow, sync skipped\n");
            return;
        }
    }

    /* The host read its clock about halfway through the query. */
    msHost += cMsLatency / 2;

    uGuest.LowPart  = ftGuest.dwLowDateTime;
    uGuest.HighPart = ftGuest.dwHighDateTime;
    msGuest = uGuest.QuadPart / 10000 - VBOXTIMESYNC_EPOCH_DIFF_MS;

    /* As upstream: a slow query makes the difference less certain, and
     * setting the clock back takes a bigger difference. */
    cMsDiff = (__int64)(msHost - msGuest);
    cMsAbsDiff = cMsDiff < 0 ? (UINT64)-cMsDiff : (UINT64)cMsDiff;
    cMsMinAdjust = (UINT64)cMsLatency * pOpts->cLatencyFactor;
    if (cMsMinAdjust < pOpts->cMsMinAdjust)
        cMsMinAdjust = pOpts->cMsMinAdjust;
    if (cMsDiff < 0)
        cMsMinAdjust += pOpts->cMsMinAdjust;

    vboxTimeSyncFormatMs(szDiff, cMsDiff);
    if (pOpts->uVerbosity >= 1)
        VBoxTrayLogRel("TimeSync: difference %s s, latency %lu ms, min adjust %lu ms%s\n", szDiff, cMsLatency,
                       (unsigned long)cMsMinAdjust, fForce ? ", forced" : "");
    if (!cMsAbsDiff || (!fForce && cMsAbsDiff < cMsMinAdjust))
        return;

    uHost.QuadPart = (msHost + VBOXTIMESYNC_EPOCH_DIFF_MS) * 10000;
    ftHost.dwLowDateTime  = uHost.LowPart;
    ftHost.dwHighDateTime = uHost.HighPart;
    if (FileTimeToSystemTime(&ftHost, &stHost) && SetSystemTime(&stHost))
        VBoxTrayLogRel("TimeSync: guest clock adjusted by %s s\n", szDiff);
    else
        VBoxTrayLogRel("TimeSync: setting the guest clock failed, error %lu\n", GetLastError());
}

/**
 * Time synchronization thread.
 *
 * @returns 0 on normal exit, 1 if the driver could not be used.
 * @param   pvUser      Unused.
 */
unsigned __stdcall VBoxTimeSyncThread(void *pvUser)
{
    VBOXTIMESYNCOPTS Opts = g_TimeSyncDefaults;
    HANDLE hDevice;
    UINT32 idClient = 0;
    DWORD msLastSync = 0;
    DWORD msLastOpts = 0;
    BOOL fSync = TRUE;      /* Sync right away... */
    BOOL fForce;
    UINT32 fEvents;
    int rc;

    hDevice = VbglR3Open();
    if (hDevice == INVALID_HANDLE_VALUE)
        return 1;

    /* Without the options the defaults are used. */
    rc = VbglR3HgcmConnect(hDevice, GUEST_PROP_SERVICE_NAME, &idClient);
    if (RT_FAILURE(rc))
    {
        VBoxTrayLogRel("TimeSync: connecting to the guest property service failed, rc=%d; no host options\n", rc);
        idClient = 0;
    }
    else
    {
        vboxTimeSyncReadOpts(hDevice, idClient, &Opts);
        msLastOpts = GetTickCount();
    }
    fForce = Opts.fSetStart; /* ...forced if --timesync-set-start. */

    /* Without the restore event we still sync periodically. */
    VbglR3ChangeFilterMask(hDevice, VMMDEV_EVENT_RESTORED, 0);

    while (!g_fVBoxTrayShutdown)
    {
        DWORD cMsWait;

        if (idClient && GetTickCount() - msLastOpts >= VBOXHOSTOPTS_INTERVAL_MS)
        {
            vboxTimeSyncReadOpts(hDevice, idClient, &Opts);
            msLastOpts = GetTickCount();
        }

        if (fSync || GetTickCount() - msLastSync >= Opts.cMsInterval)
        {
            vboxTimeSyncOnce(hDevice, &Opts, fForce);
            msLastSync = GetTickCount();
            fSync  = FALSE;
            fForce = FALSE;
        }

        cMsWait = Opts.cMsInterval - (GetTickCount() - msLastSync);
        if (cMsWait > Opts.cMsInterval) /* Already past it. */
            cMsWait = 0;
        if (cMsWait > VBOXTIMESYNC_POLL_MS)
            cMsWait = VBOXTIMESYNC_POLL_MS;
        if (!cMsWait)
            continue;

        rc = VbglR3WaitForEvents(hDevice, VMMDEV_EVENT_RESTORED, cMsWait, &fEvents);
        if (RT_SUCCESS(rc) && (fEvents & VMMDEV_EVENT_RESTORED))
        {
            /* Without --timesync-set-on-restore the next sync comes at its time. */
            if (Opts.fSetOnRestore)
                fSync = fForce = TRUE;
        }
        else if (RT_FAILURE(rc) && rc != VERR_TIMEOUT)
            Sleep(cMsWait); /* The wait didn't block, don't spin. */
    }

    VBoxHostOptsFree(&g_TimeSyncOptsCache);
    if (idClient)
        VbglR3HgcmDisconnect(hDevice, idClient);
    VbglR3Close(hDevice);
    return 0;
}
