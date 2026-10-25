/** @file
 * VBoxTray - Mouse pointer integration.
 *
 * Windows 98 drives the PS/2 mouse with VxDs, so instead of a WDM filter
 * driver the pointer is positioned from user mode: once the guest reports
 * VMMDEV_MOUSE_GUEST_CAN_ABSOLUTE the host stops sending relative movement
 * over PS/2 (buttons still go that way) and raises
 * VMMDEV_EVENT_MOUSE_POSITION_CHANGED instead.  The absolute position
 * (0..0xffff) is fed to mouse_event, which uses the same normalized range.
 */

#include "VBoxTray.h"


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/

/** The mouse features we tell the host about. */
#define VBOXMOUSE_GUEST_FEATURES    (VMMDEV_MOUSE_GUEST_CAN_ABSOLUTE | VMMDEV_MOUSE_NEW_PROTOCOL)
/** The events we are interested in. */
#define VBOXMOUSE_EVENTS            (VMMDEV_EVENT_MOUSE_POSITION_CHANGED | VMMDEV_EVENT_MOUSE_CAPABILITIES_CHANGED)
/** How often to check for shutdown while no events arrive (ms). */
#define VBOXMOUSE_POLL_MS           500


/**
 * Moves the pointer to the host position if the host wants absolute mode.
 *
 * @param   hDevice     The device handle.
 * @param   px          Last position set, updated.
 * @param   py          Last position set, updated.
 */
static void vboxMouseUpdate(HANDLE hDevice, INT32 *px, INT32 *py)
{
    UINT32 fFeatures = 0;
    INT32 x = 0;
    INT32 y = 0;

    if (RT_FAILURE(VbglR3GetMouseStatus(hDevice, &fFeatures, &x, &y)))
        return;
    if (!(fFeatures & VMMDEV_MOUSE_HOST_WANTS_ABSOLUTE))
        return; /* Mouse integration disabled on the host, PS/2 does the job. */

    if (x < VMMDEV_MOUSE_RANGE_MIN) x = VMMDEV_MOUSE_RANGE_MIN;
    if (x > VMMDEV_MOUSE_RANGE_MAX) x = VMMDEV_MOUSE_RANGE_MAX;
    if (y < VMMDEV_MOUSE_RANGE_MIN) y = VMMDEV_MOUSE_RANGE_MIN;
    if (y > VMMDEV_MOUSE_RANGE_MAX) y = VMMDEV_MOUSE_RANGE_MAX;
    if (x == *px && y == *py)
        return;

    mouse_event(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE, (DWORD)x, (DWORD)y, 0, 0);
    *px = x;
    *py = y;
}

/**
 * Mouse integration thread.
 *
 * @returns 0 on normal exit, 1 if the driver could not be used.
 * @param   pvUser      Unused.
 */
unsigned __stdcall VBoxMouseThread(void *pvUser)
{
    HANDLE hDevice;
    INT32 xLast = -1;
    INT32 yLast = -1;
    UINT32 fEvents;
    int rc;

    hDevice = VbglR3Open();
    if (hDevice == INVALID_HANDLE_VALUE)
        return 1;

    rc = VbglR3ChangeFilterMask(hDevice, VBOXMOUSE_EVENTS, 0);
    if (RT_SUCCESS(rc))
        rc = VbglR3SetMouseStatus(hDevice, VBOXMOUSE_GUEST_FEATURES);
    if (RT_FAILURE(rc))
    {
        VbglR3Close(hDevice);
        return 1;
    }

    while (!g_fVBoxTrayShutdown)
    {
        rc = VbglR3WaitForEvents(hDevice, VBOXMOUSE_EVENTS, VBOXMOUSE_POLL_MS, &fEvents);
        if (RT_SUCCESS(rc))
            vboxMouseUpdate(hDevice, &xLast, &yLast);
        else if (rc != VERR_TIMEOUT && rc != VERR_INTERRUPTED)
            Sleep(VBOXMOUSE_POLL_MS); /* Device stopped or similar, don't spin. */
    }

    /* Closing the handle resets the session's mouse status as well, but be explicit. */
    VbglR3SetMouseStatus(hDevice, 0);
    VbglR3Close(hDevice);
    return 0;
}
