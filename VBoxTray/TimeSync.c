/** @file
 * VBoxTray - Guest clock synchronization.
 *
 * Windows 98 has no way to slew the clock (SetSystemTimeAdjustment is NT
 * only), so the clock is set outright when it drifts more than
 * VBOXTIMESYNC_MIN_ADJUST_MS from the host.  A sync is done at startup,
 * every VBOXTIMESYNC_INTERVAL_MS and right after the VM is restored from a
 * saved state (VMMDEV_EVENT_RESTORED).
 */

#include "VBoxTray.h"


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/

/** Interval between synchronizations (ms). */
#define VBOXTIMESYNC_INTERVAL_MS    10000
/** Smallest difference that is corrected (ms). */
#define VBOXTIMESYNC_MIN_ADJUST_MS  1000
/** How often to check for shutdown (ms). */
#define VBOXTIMESYNC_POLL_MS        1000
/** Milliseconds between 1601-01-01 (FILETIME epoch) and 1970-01-01 (host time epoch). */
#define VBOXTIMESYNC_EPOCH_DIFF_MS  11644473600000ui64


/**
 * Compares the guest clock with the host and corrects it if needed.
 *
 * @param   hDevice     The device handle.
 */
static void vboxTimeSyncOnce(HANDLE hDevice)
{
    ULARGE_INTEGER uGuest;
    ULARGE_INTEGER uHost;
    FILETIME ftGuest;
    FILETIME ftHost;
    SYSTEMTIME stHost;
    UINT64 msHost;
    UINT64 msGuest;
    __int64 cMsDiff;

    if (RT_FAILURE(VbglR3GetHostTime(hDevice, &msHost)))
        return;

    GetSystemTimeAsFileTime(&ftGuest);
    uGuest.LowPart  = ftGuest.dwLowDateTime;
    uGuest.HighPart = ftGuest.dwHighDateTime;
    msGuest = uGuest.QuadPart / 10000 - VBOXTIMESYNC_EPOCH_DIFF_MS;

    cMsDiff = (__int64)(msHost - msGuest);
    if (cMsDiff > -VBOXTIMESYNC_MIN_ADJUST_MS && cMsDiff < VBOXTIMESYNC_MIN_ADJUST_MS)
        return;

    uHost.QuadPart = (msHost + VBOXTIMESYNC_EPOCH_DIFF_MS) * 10000;
    ftHost.dwLowDateTime  = uHost.LowPart;
    ftHost.dwHighDateTime = uHost.HighPart;
    if (FileTimeToSystemTime(&ftHost, &stHost))
        SetSystemTime(&stHost);
}

/**
 * Time synchronization thread.
 *
 * @returns 0 on normal exit, 1 if the driver could not be used.
 * @param   pvUser      Unused.
 */
unsigned __stdcall VBoxTimeSyncThread(void *pvUser)
{
    HANDLE hDevice;
    DWORD cMsSinceSync = VBOXTIMESYNC_INTERVAL_MS; /* Sync right away. */
    UINT32 fEvents;
    int rc;

    hDevice = VbglR3Open();
    if (hDevice == INVALID_HANDLE_VALUE)
        return 1;

    /* Without the restore event we still sync periodically. */
    VbglR3ChangeFilterMask(hDevice, VMMDEV_EVENT_RESTORED, 0);

    while (!g_fVBoxTrayShutdown)
    {
        if (cMsSinceSync >= VBOXTIMESYNC_INTERVAL_MS)
        {
            vboxTimeSyncOnce(hDevice);
            cMsSinceSync = 0;
        }

        rc = VbglR3WaitForEvents(hDevice, VMMDEV_EVENT_RESTORED, VBOXTIMESYNC_POLL_MS, &fEvents);
        if (RT_SUCCESS(rc) && (fEvents & VMMDEV_EVENT_RESTORED))
            cMsSinceSync = VBOXTIMESYNC_INTERVAL_MS;
        else
        {
            if (rc != VERR_TIMEOUT)
                Sleep(VBOXTIMESYNC_POLL_MS); /* The wait didn't block, don't spin. */
            cMsSinceSync += VBOXTIMESYNC_POLL_MS;
        }
    }

    VbglR3Close(hDevice);
    return 0;
}
