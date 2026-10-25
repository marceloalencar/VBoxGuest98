/** @file
 * VBoxTray - Resizing the guest display to the host window ("Auto-resize
 * Guest Display").
 *
 * Follows the NT4 path of upstream VBoxDisplay.cpp (DisplayChangeRequestHandler
 * and ResizeDisplayDeviceNT4): report VMMDEV_GUEST_SUPPORTS_GRAPHICS, wait for
 * VMMDEV_EVENT_DISPLAY_CHANGE_REQUEST, read the wanted resolution with
 * VMMDevReq_GetDisplayChangeRequestEx and switch with ChangeDisplaySettings.
 *
 * Differences from upstream, because Windows 98 display drivers (vmdisp9x)
 * have a fixed mode list:
 *  - If the exact resolution isn't offered, the largest mode that fits the
 *    host window is used instead of giving up.
 *  - The color depth is kept: changing it usually requires a restart on
 *    Windows 98.
 *  - Only the primary display is handled.
 */

#include "VBoxTray.h"
#include <string.h>


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/

/** How often to check for shutdown (ms). */
#define VBOXDISPLAY_POLL_MS         1000


/**
 * Gets the current display mode.
 *
 * @returns TRUE on success.
 */
static BOOL vboxDisplayGetCurrentMode(DEVMODE *pMode)
{
    HDC hdc;

    memset(pMode, 0, sizeof(*pMode));
    pMode->dmSize = sizeof(*pMode);
    if (EnumDisplaySettings(NULL, ENUM_CURRENT_SETTINGS, pMode))
        return TRUE;

    /* Fallback in case ENUM_CURRENT_SETTINGS isn't supported. */
    hdc = GetDC(NULL);
    if (!hdc)
        return FALSE;
    pMode->dmPelsWidth  = GetSystemMetrics(SM_CXSCREEN);
    pMode->dmPelsHeight = GetSystemMetrics(SM_CYSCREEN);
    pMode->dmBitsPerPel = GetDeviceCaps(hdc, BITSPIXEL) * GetDeviceCaps(hdc, PLANES);
    ReleaseDC(NULL, hdc);
    return TRUE;
}

/**
 * Finds the mode to use for the requested resolution: the exact one if the
 * driver offers it, otherwise the largest one that fits.
 *
 * @returns TRUE if a mode was found.
 * @param   cx          Wanted width.
 * @param   cy          Wanted height.
 * @param   cBits       Color depth to look for.
 * @param   pMode       Where to return the mode.
 */
static BOOL vboxDisplayFindMode(DWORD cx, DWORD cy, DWORD cBits, DEVMODE *pMode)
{
    DEVMODE Mode;
    DWORD cPixelsBest = 0;
    DWORD iMode;

    for (iMode = 0; ; iMode++)
    {
        memset(&Mode, 0, sizeof(Mode));
        Mode.dmSize = sizeof(Mode);
        if (!EnumDisplaySettings(NULL, iMode, &Mode))
            break;
        if (Mode.dmBitsPerPel != cBits)
            continue;

        if (Mode.dmPelsWidth == cx && Mode.dmPelsHeight == cy)
        {
            *pMode = Mode;
            return TRUE;
        }
        if (   Mode.dmPelsWidth <= cx
            && Mode.dmPelsHeight <= cy
            && Mode.dmPelsWidth * Mode.dmPelsHeight > cPixelsBest)
        {
            *pMode = Mode;
            cPixelsBest = Mode.dmPelsWidth * Mode.dmPelsHeight;
        }
    }
    return cPixelsBest != 0;
}

/**
 * Switches to the mode closest to the requested resolution.
 *
 * @param   cx      Wanted width, 0 = keep.
 * @param   cy      Wanted height, 0 = keep.
 */
static void vboxDisplayResize(DWORD cx, DWORD cy)
{
    DEVMODE Current;
    DEVMODE Mode;

    if (!vboxDisplayGetCurrentMode(&Current))
        return;
    if (!cx)
        cx = Current.dmPelsWidth;
    if (!cy)
        cy = Current.dmPelsHeight;
    if (cx == Current.dmPelsWidth && cy == Current.dmPelsHeight)
        return;

    if (!vboxDisplayFindMode(cx, cy, Current.dmBitsPerPel, &Mode))
        return;
    if (Mode.dmPelsWidth == Current.dmPelsWidth && Mode.dmPelsHeight == Current.dmPelsHeight)
        return; /* Already the best fit. */

    Mode.dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT;
    ChangeDisplaySettings(&Mode, CDS_UPDATEREGISTRY);
}

/**
 * Reads the host's display change request and applies it.
 *
 * @param   hDevice     The device handle.
 * @param   fAck        Whether this answers VMMDEV_EVENT_DISPLAY_CHANGE_REQUEST.
 */
static void vboxDisplayHandleRequest(HANDLE hDevice, BOOL fAck)
{
    UINT32 cx = 0;
    UINT32 cy = 0;
    UINT32 cBits = 0;
    UINT32 iDisplay = 0;
    BOOL fEnabled = TRUE;

    if (RT_FAILURE(VbglR3GetDisplayChangeRequest(hDevice, &cx, &cy, &cBits, &iDisplay, &fEnabled, fAck)))
        return;
    if (iDisplay != 0 || (!cx && !cy))
        return; /* Secondary display, or nothing to change. */
    vboxDisplayResize(cx, cy);
}

/**
 * Display resizing thread.
 *
 * @returns 0 on normal exit, 1 if the driver could not be used.
 * @param   pvUser      Unused.
 */
unsigned __stdcall VBoxDisplayThread(void *pvUser)
{
    HANDLE hDevice;
    UINT32 fEvents;
    int rc;

    hDevice = VbglR3Open();
    if (hDevice == INVALID_HANDLE_VALUE)
        return 1;

    rc = VbglR3ChangeFilterMask(hDevice, VMMDEV_EVENT_DISPLAY_CHANGE_REQUEST, 0);
    if (RT_SUCCESS(rc))
        rc = VbglR3ChangeGuestCapabilities(hDevice, VMMDEV_GUEST_SUPPORTS_GRAPHICS, 0);
    if (RT_FAILURE(rc))
    {
        VbglR3Close(hDevice);
        return 1;
    }

    /* A request made before we started (e.g. at VM start) raised no event we
       could see; apply the last one, like upstream's ApplyLastDisplayChangeRequest. */
    vboxDisplayHandleRequest(hDevice, FALSE);

    while (!g_fVBoxTrayShutdown)
    {
        rc = VbglR3WaitForEvents(hDevice, VMMDEV_EVENT_DISPLAY_CHANGE_REQUEST, VBOXDISPLAY_POLL_MS, &fEvents);
        if (RT_SUCCESS(rc) && (fEvents & VMMDEV_EVENT_DISPLAY_CHANGE_REQUEST))
            vboxDisplayHandleRequest(hDevice, TRUE);
        else if (rc != VERR_TIMEOUT && rc != VERR_INTERRUPTED)
            Sleep(VBOXDISPLAY_POLL_MS); /* The wait didn't block, don't spin. */
    }

    /* Closing the handle withdraws the capability as well, but be explicit. */
    VbglR3ChangeGuestCapabilities(hDevice, 0, VMMDEV_GUEST_SUPPORTS_GRAPHICS);
    VbglR3ChangeFilterMask(hDevice, 0, VMMDEV_EVENT_DISPLAY_CHANGE_REQUEST);
    VbglR3Close(hDevice);
    return 0;
}
