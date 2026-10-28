/** @file
 * VBoxTray - Resizing the guest display to the host window ("Auto-resize
 * Guest Display").
 *
 * Follows the NT4 path of upstream VBoxDisplay.cpp (DisplayChangeRequestHandler
 * and ResizeDisplayDeviceNT4): report VMMDEV_GUEST_SUPPORTS_GRAPHICS, wait for
 * VMMDEV_EVENT_DISPLAY_CHANGE_REQUEST, read the wanted resolution with
 * VMMDevReq_GetDisplayChangeRequestEx and switch with ChangeDisplaySettings.
 *
 * Differences from upstream, because Windows 98 takes the mode list from the
 * registry (MODES\{bpp}\{width},{height} keys under the display driver key,
 * written by the driver INF) while the driver (vmdisp9x) itself accepts any
 * resolution that fits in video memory:
 *  - If the exact resolution isn't offered, its MODES key is added and the
 *    switch is tried.  Only the last mode VBoxTray added is kept (remembered
 *    under VBOXDISPLAY_STATE_KEY); modes from the INF are never touched.
 *  - If that fails, the largest offered mode that fits the host window is
 *    used instead of giving up.
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
/** Windows 9x display driver keys (HKLM), one numbered subkey per adapter. */
#define VBOXDISPLAY_CLASS_KEY       "System\\CurrentControlSet\\Services\\Class\\Display"
/** Where VBoxTray remembers the mode key it added (HKLM, value "AddedMode"). */
#define VBOXDISPLAY_STATE_KEY       "Software\\VBoxGuest98\\Display"
/** Smallest mode added; the driver forces anything smaller to 640x480. */
#define VBOXDISPLAY_MIN_CX          640
#define VBOXDISPLAY_MIN_CY          480


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
 * Checks whether a registry key exists (HKLM).
 */
static BOOL vboxDisplayKeyExists(const char *pszKey)
{
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, pszKey, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return FALSE;
    RegCloseKey(hKey);
    return TRUE;
}

/**
 * Finds the registry key of the active display driver: the Class\Display
 * subkey whose mode list contains the current mode and which names a driver.
 *
 * @returns TRUE if found.
 * @param   pCurrent    The current mode.
 * @param   pszKey      Where to return the key (relative to HKLM).
 * @param   cbKey       Size of the buffer, at least MAX_PATH.
 */
static BOOL vboxDisplayFindDriverKey(const DEVMODE *pCurrent, char *pszKey, DWORD cbKey)
{
    char szSubKey[16];
    char szProbe[MAX_PATH];
    DWORD cbValue;
    HKEY hClass;
    HKEY hDefault;
    DWORD iKey;
    BOOL fFound = FALSE;

    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, VBOXDISPLAY_CLASS_KEY, 0, KEY_READ, &hClass) != ERROR_SUCCESS)
        return FALSE;

    for (iKey = 0; !fFound; iKey++)
    {
        DWORD cchSubKey = sizeof(szSubKey);
        if (RegEnumKeyEx(hClass, iKey, szSubKey, &cchSubKey, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            break;

        wsprintf(szProbe, "%s\\%s\\MODES\\%lu\\%lu,%lu", VBOXDISPLAY_CLASS_KEY, szSubKey,
                 pCurrent->dmBitsPerPel, pCurrent->dmPelsWidth, pCurrent->dmPelsHeight);
        if (!vboxDisplayKeyExists(szProbe))
            continue;

        wsprintf(szProbe, "%s\\%s\\DEFAULT", VBOXDISPLAY_CLASS_KEY, szSubKey);
        if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, szProbe, 0, KEY_READ, &hDefault) != ERROR_SUCCESS)
            continue;
        cbValue = 0;
        fFound = RegQueryValueEx(hDefault, "drv", NULL, NULL, NULL, &cbValue) == ERROR_SUCCESS && cbValue > 1;
        RegCloseKey(hDefault);
        if (fFound)
        {
            wsprintf(szProbe, "%s\\%s", VBOXDISPLAY_CLASS_KEY, szSubKey);
            lstrcpyn(pszKey, szProbe, cbKey);
        }
    }

    RegCloseKey(hClass);
    return fFound;
}

/**
 * Reads or writes the mode key VBoxTray added last.
 *
 * @param   pszMode     The key, relative to HKLM.  When reading, a MAX_PATH
 *                      buffer that receives it (empty string if none).
 * @param   fWrite      TRUE to store pszMode, FALSE to read it.
 */
static void vboxDisplayAddedMode(char *pszMode, BOOL fWrite)
{
    DWORD cbValue = MAX_PATH;
    HKEY hKey;

    if (fWrite)
    {
        if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, VBOXDISPLAY_STATE_KEY, 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL)
            == ERROR_SUCCESS)
        {
            RegSetValueEx(hKey, "AddedMode", 0, REG_SZ, (const BYTE *)pszMode, lstrlen(pszMode) + 1);
            RegCloseKey(hKey);
        }
        return;
    }

    pszMode[0] = '\0';
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, VBOXDISPLAY_STATE_KEY, 0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        if (RegQueryValueEx(hKey, "AddedMode", NULL, NULL, (LPBYTE)pszMode, &cbValue) != ERROR_SUCCESS)
            pszMode[0] = '\0';
        pszMode[MAX_PATH - 1] = '\0';
        RegCloseKey(hKey);
    }
}

/**
 * Switches to a mode, keeping it in the registry, and logs the outcome.
 *
 * @returns TRUE on success.
 * @param   pMode       The mode (resolution and color depth are used).
 * @param   pszHow      How the mode was chosen, for the log.
 */
static BOOL vboxDisplaySetMode(DEVMODE *pMode, const char *pszHow)
{
    LONG lRc;

    pMode->dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT;
    lRc = ChangeDisplaySettings(pMode, CDS_UPDATEREGISTRY);
    if (lRc == DISP_CHANGE_SUCCESSFUL)
        VBoxTrayLogRel("Display: switched to %lux%lux%lu (%s)\n",
                       pMode->dmPelsWidth, pMode->dmPelsHeight, pMode->dmBitsPerPel, pszHow);
    else
        VBoxTrayLogRel("Display: switching to %lux%lux%lu (%s) failed, %ld\n",
                       pMode->dmPelsWidth, pMode->dmPelsHeight, pMode->dmBitsPerPel, pszHow, lRc);
    return lRc == DISP_CHANGE_SUCCESSFUL;
}

/**
 * Tries to switch to an exact resolution the driver doesn't list, by adding
 * it to the mode list first.  The previously added mode is removed.
 *
 * @returns TRUE if the display now uses the resolution.
 * @param   pCurrent    The current mode.
 * @param   cx          Wanted width.
 * @param   cy          Wanted height.
 */
static BOOL vboxDisplayResizeExact(const DEVMODE *pCurrent, DWORD cx, DWORD cy)
{
    char szDriverKey[MAX_PATH];
    char szMode[MAX_PATH];
    char szPrevMode[MAX_PATH];
    DEVMODE Mode;
    HKEY hKey;

    if (cx < VBOXDISPLAY_MIN_CX || cy < VBOXDISPLAY_MIN_CY)
        return FALSE;
    if (!vboxDisplayFindDriverKey(pCurrent, szDriverKey, sizeof(szDriverKey)))
        return FALSE;

    wsprintf(szMode, "%s\\MODES\\%lu\\%lu,%lu", szDriverKey, pCurrent->dmBitsPerPel, cx, cy);
    if (RegCreateKeyEx(HKEY_LOCAL_MACHINE, szMode, 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) != ERROR_SUCCESS)
        return FALSE;
    RegCloseKey(hKey);

    Mode = *pCurrent;
    Mode.dmPelsWidth  = cx;
    Mode.dmPelsHeight = cy;
    if (!vboxDisplaySetMode(&Mode, "added to the mode list"))
    {
        RegDeleteKey(HKEY_LOCAL_MACHINE, szMode);
        return FALSE;
    }

    /* The new mode is in use now; forget the one added before. */
    vboxDisplayAddedMode(szPrevMode, FALSE);
    if (szPrevMode[0] && lstrcmpi(szPrevMode, szMode) != 0)
        RegDeleteKey(HKEY_LOCAL_MACHINE, szPrevMode);
    vboxDisplayAddedMode(szMode, TRUE);
    return TRUE;
}

/**
 * Switches to the requested resolution, or the closest offered one.
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
    /* Keep the width even: the display driver makes odd widths even anyway.
       Rounding down further would leave dark strips at both sides in seamless
       mode, where the host centers a narrower guest screen. */
    cx &= ~1U;
    if (cx == Current.dmPelsWidth && cy == Current.dmPelsHeight)
        return;

    if (   vboxDisplayFindMode(cx, cy, Current.dmBitsPerPel, &Mode)
        && Mode.dmPelsWidth == cx
        && Mode.dmPelsHeight == cy)
    {
        vboxDisplaySetMode(&Mode, "listed mode");
        return;
    }

    if (vboxDisplayResizeExact(&Current, cx, cy))
        return;

    /* Fall back to the largest offered mode that fits. */
    if (!vboxDisplayFindMode(cx, cy, Current.dmBitsPerPel, &Mode))
    {
        VBoxTrayLogRel("Display: no mode fits %lux%lu\n", cx, cy);
        return;
    }
    if (Mode.dmPelsWidth == Current.dmPelsWidth && Mode.dmPelsHeight == Current.dmPelsHeight)
        return; /* Already the best fit. */

    vboxDisplaySetMode(&Mode, "best fit");
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
    VBoxTrayLogRel("Display: host asks for %lux%lu\n", cx, cy);
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
        VBoxTrayLogRel("Display: setup failed, rc=%d\n", rc);
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
