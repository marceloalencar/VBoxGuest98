/** @file
 * VBoxTray - Seamless mode.
 *
 * Follows upstream VBoxSeamless.cpp: report VMMDEV_GUEST_SUPPORTS_SEAMLESS,
 * wait for VMMDEV_EVENT_SEAMLESS_MODE_CHANGE_REQUEST, and while the host
 * wants VMMDev_Seamless_Visible_Region send it the union of the visible top
 * level windows with VMMDevReq_VideoSetVisibleRegion.  The host then shows
 * only those parts of the guest screen, on top of its own desktop.
 *
 * Differences from upstream:
 *  - Changes are found by building the region again every
 *    VBOXSEAMLESS_POLL_MS and sending it only when it differs, instead of
 *    an in-context hook DLL (VBoxHook.dll).
 *  - VBoxTray's own windows (the drag and drop proxy) are left out by
 *    process instead of by class.
 *  - A region with more rectangles than fit in one request is sent as its
 *    bounding box.
 *  - Windows 98 drags only an outline of the window by default, so the
 *    region stays put and the pointer leaves it for the host, which then
 *    keeps the mouse: the window can't be dropped outside its own area.
 *    While seamless mode is on, windows are dragged with their contents
 *    (SPI_SETDRAGFULLWINDOWS, not saved, the setting is restored
 *    afterwards) and the region is looked at every VBOXSEAMLESS_DRAG_POLL_MS
 *    while a mouse button is down, so it follows the window.
 */

#include "VBoxTray.h"
#include <stdlib.h>
#include <string.h>


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/

/** How often to check for shutdown while seamless mode is off (ms). */
#define VBOXSEAMLESS_IDLE_MS        1000
/** How often to look for window changes while seamless mode is on (ms). */
#define VBOXSEAMLESS_POLL_MS        200
/** The same while a mouse button is down, e.g. while a window is dragged (ms). */
#define VBOXSEAMLESS_DRAG_POLL_MS   20
/** Most rectangles one VMMDevReq_VideoSetVisibleRegion request can carry. */
#define VBOXSEAMLESS_MAX_RECTS      ((VBOXGUEST98_MAX_VMMDEV_REQUEST_SIZE - VMMDEV_VISIBLE_REGION_SIZE(0)) / sizeof(RTRECT))


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/

/** Seamless thread state. */
typedef struct VBOXSEAMLESSSTATE
{
    /** The mode the host wants (VMMDevSeamlessMode). */
    UINT32  enmMode;
    /** The region last sent, NULL if none since seamless mode was turned on. */
    RTRECT *paRects;
    UINT32  cRects;
    /** Whether the bounding box fallback was logged. */
    BOOL    fLoggedTooMany;
    /** Whether full window drag was turned on by us, to be turned off again. */
    BOOL    fDragFullSet;
} VBOXSEAMLESSSTATE;

/** EnumWindows context. */
typedef struct VBOXSEAMLESSENUM
{
    /** The region being built. */
    HRGN    hrgn;
    /** VBoxTray's process, whose windows are left out. */
    DWORD   idProcess;
} VBOXSEAMLESSENUM;


/**
 * Adds a top level window to the visible region, if it is shown.
 */
static BOOL CALLBACK vboxSeamlessEnumProc(HWND hwnd, LPARAM lParam)
{
    VBOXSEAMLESSENUM *pEnum = (VBOXSEAMLESSENUM *)lParam;
    DWORD idProcess = 0;
    char szClass[32];
    RECT Rect;
    HRGN hrgnWnd;

    if (!IsWindowVisible(hwnd) || IsIconic(hwnd))
        return TRUE;
    if (GetWindowLong(hwnd, GWL_EXSTYLE) & WS_EX_TRANSPARENT)
        return TRUE;
    GetWindowThreadProcessId(hwnd, &idProcess);
    if (idProcess == pEnum->idProcess)
        return TRUE;
    /* The desktop (icons and wallpaper) is replaced by the host's. */
    if (GetClassName(hwnd, szClass, sizeof(szClass)) && lstrcmpi(szClass, "Progman") == 0)
        return TRUE;
    if (!GetWindowRect(hwnd, &Rect) || IsRectEmpty(&Rect))
        return TRUE;

    hrgnWnd = CreateRectRgn(0, 0, 0, 0);
    if (!hrgnWnd)
        return TRUE;
    /* Shaped windows have a region relative to the window's corner. */
    if (GetWindowRgn(hwnd, hrgnWnd) != ERROR)
        OffsetRgn(hrgnWnd, Rect.left, Rect.top);
    else
        SetRectRgn(hrgnWnd, Rect.left, Rect.top, Rect.right, Rect.bottom);
    CombineRgn(pEnum->hrgn, pEnum->hrgn, hrgnWnd, RGN_OR);
    DeleteObject(hrgnWnd);
    return TRUE;
}

/**
 * Builds the visible region: the visible top level windows, clipped to the
 * screen.
 *
 * @returns The rectangles (malloc'ed), NULL on failure.
 * @param   pState      The thread state (for the log).
 * @param   pcRects     Where to return the number of rectangles, at least 1.
 */
static RTRECT *vboxSeamlessBuildRegion(VBOXSEAMLESSSTATE *pState, UINT32 *pcRects)
{
    VBOXSEAMLESSENUM Enum;
    HRGN hrgnScreen;
    RGNDATA *pData;
    RTRECT *paRects = NULL;
    const RECT *paRgnRects;
    DWORD cbData;
    UINT32 cRects;
    UINT32 i;

    Enum.hrgn = CreateRectRgn(0, 0, 0, 0);
    if (!Enum.hrgn)
        return NULL;
    Enum.idProcess = GetCurrentProcessId();
    EnumWindows(vboxSeamlessEnumProc, (LPARAM)&Enum);

    hrgnScreen = CreateRectRgn(0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    if (hrgnScreen)
    {
        CombineRgn(Enum.hrgn, Enum.hrgn, hrgnScreen, RGN_AND);
        DeleteObject(hrgnScreen);
    }

    cbData = GetRegionData(Enum.hrgn, 0, NULL);
    pData = cbData ? (RGNDATA *)malloc(cbData) : NULL;
    if (pData && GetRegionData(Enum.hrgn, cbData, pData) == cbData)
    {
        cRects = pData->rdh.nCount;
        paRgnRects = (const RECT *)pData->Buffer;
        if (cRects > VBOXSEAMLESS_MAX_RECTS)
        {
            if (!pState->fLoggedTooMany)
                VBoxTrayLogRel("Seamless: %lu rectangles don't fit in one request, sending the bounding box\n", cRects);
            pState->fLoggedTooMany = TRUE;
            cRects = 1;
            paRgnRects = &pData->rdh.rcBound;
        }

        /* An empty region (nothing on screen) still takes one rectangle. */
        paRects = (RTRECT *)calloc(cRects ? cRects : 1, sizeof(RTRECT));
        if (paRects)
        {
            for (i = 0; i < cRects; i++)
            {
                paRects[i].xLeft   = paRgnRects[i].left;
                paRects[i].yTop    = paRgnRects[i].top;
                paRects[i].xRight  = paRgnRects[i].right;
                paRects[i].yBottom = paRgnRects[i].bottom;
            }
            *pcRects = cRects ? cRects : 1;
        }
    }

    free(pData);
    DeleteObject(Enum.hrgn);
    return paRects;
}

/**
 * Sends the visible region if it changed since the last time.
 */
static void vboxSeamlessUpdate(HANDLE hDevice, VBOXSEAMLESSSTATE *pState)
{
    UINT32 cRects = 0;
    RTRECT *paRects;
    int rc;

    paRects = vboxSeamlessBuildRegion(pState, &cRects);
    if (!paRects)
        return;
    if (   pState->paRects
        && pState->cRects == cRects
        && memcmp(pState->paRects, paRects, cRects * sizeof(RTRECT)) == 0)
    {
        free(paRects);
        return;
    }

    rc = VbglR3SeamlessSendRects(hDevice, paRects, cRects);
    if (RT_FAILURE(rc))
    {
        VBoxTrayLogRel("Seamless: sending %lu rectangles failed, rc=%d\n", cRects, rc);
        free(paRects);
        return; /* Try again on the next round. */
    }
    if (!pState->paRects)
        VBoxTrayLogRel("Seamless: sent the visible region, %lu rectangles\n", cRects);
    free(pState->paRects);
    pState->paRects = paRects;
    pState->cRects = cRects;
}

/**
 * Turns full window drag on for seamless mode, or back off when we turned
 * it on.  The change isn't saved, so a crash can't leave it behind.
 */
static void vboxSeamlessDragFull(VBOXSEAMLESSSTATE *pState, BOOL fOn)
{
    BOOL fCurrent = FALSE;

    if (fOn)
    {
        if (   SystemParametersInfo(SPI_GETDRAGFULLWINDOWS, 0, &fCurrent, 0)
            && !fCurrent
            && SystemParametersInfo(SPI_SETDRAGFULLWINDOWS, TRUE, NULL, 0))
            pState->fDragFullSet = TRUE;
    }
    else if (pState->fDragFullSet)
    {
        SystemParametersInfo(SPI_SETDRAGFULLWINDOWS, FALSE, NULL, 0);
        pState->fDragFullSet = FALSE;
    }
}

/**
 * Checks whether a mouse button is down.
 */
static BOOL vboxSeamlessButtonDown(void)
{
    return (GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) | GetAsyncKeyState(VK_MBUTTON)) & 0x8000;
}

/**
 * Reads the mode the host wants.
 *
 * @param   hDevice     The device handle.
 * @param   pState      The thread state.
 * @param   fAck        Whether this answers VMMDEV_EVENT_SEAMLESS_MODE_CHANGE_REQUEST.
 */
static void vboxSeamlessHandleRequest(HANDLE hDevice, VBOXSEAMLESSSTATE *pState, BOOL fAck)
{
    UINT32 enmMode = VMMDev_Seamless_Disabled;

    if (RT_FAILURE(VbglR3SeamlessGetMode(hDevice, &enmMode, fAck)))
        return;
    if (enmMode == pState->enmMode)
        return;
    VBoxTrayLogRel("Seamless: host asks for mode %lu\n", enmMode);
    pState->enmMode = enmMode;
    vboxSeamlessDragFull(pState, enmMode == VMMDev_Seamless_Visible_Region);

    /* Send the region afresh the next time seamless mode is on. */
    free(pState->paRects);
    pState->paRects = NULL;
    pState->cRects = 0;
}

/**
 * Seamless mode thread.
 *
 * @returns 0 on normal exit, 1 if the driver could not be used.
 * @param   pvUser      Unused.
 */
unsigned __stdcall VBoxSeamlessThread(void *pvUser)
{
    VBOXSEAMLESSSTATE State;
    HANDLE hDevice;
    UINT32 fEvents;
    int rc;

    hDevice = VbglR3Open();
    if (hDevice == INVALID_HANDLE_VALUE)
        return 1;

    rc = VbglR3ChangeFilterMask(hDevice, VMMDEV_EVENT_SEAMLESS_MODE_CHANGE_REQUEST, 0);
    if (RT_SUCCESS(rc))
        rc = VbglR3ChangeGuestCapabilities(hDevice, VMMDEV_GUEST_SUPPORTS_SEAMLESS, 0);
    if (RT_FAILURE(rc))
    {
        VBoxTrayLogRel("Seamless: setup failed, rc=%d\n", rc);
        VbglR3Close(hDevice);
        return 1;
    }

    memset(&State, 0, sizeof(State));
    State.enmMode = VMMDev_Seamless_Disabled;
    /* Seamless mode may already be on, e.g. when VBoxTray restarts. */
    vboxSeamlessHandleRequest(hDevice, &State, FALSE);

    while (!g_fVBoxTrayShutdown)
    {
        DWORD cMsWait = VBOXSEAMLESS_IDLE_MS;
        if (State.enmMode == VMMDev_Seamless_Visible_Region)
            cMsWait = vboxSeamlessButtonDown() ? VBOXSEAMLESS_DRAG_POLL_MS : VBOXSEAMLESS_POLL_MS;
        rc = VbglR3WaitForEvents(hDevice, VMMDEV_EVENT_SEAMLESS_MODE_CHANGE_REQUEST, cMsWait, &fEvents);
        if (RT_SUCCESS(rc) && (fEvents & VMMDEV_EVENT_SEAMLESS_MODE_CHANGE_REQUEST))
            vboxSeamlessHandleRequest(hDevice, &State, TRUE);
        else if (rc != VERR_TIMEOUT && rc != VERR_INTERRUPTED)
            Sleep(VBOXSEAMLESS_IDLE_MS); /* The wait didn't block, don't spin. */
        if (State.enmMode == VMMDev_Seamless_Visible_Region && !g_fVBoxTrayShutdown)
            vboxSeamlessUpdate(hDevice, &State);
    }

    vboxSeamlessDragFull(&State, FALSE);
    free(State.paRects);
    /* Closing the handle withdraws the capability as well, but be explicit. */
    VbglR3ChangeGuestCapabilities(hDevice, 0, VMMDEV_GUEST_SUPPORTS_SEAMLESS);
    VbglR3ChangeFilterMask(hDevice, 0, VMMDEV_EVENT_SEAMLESS_MODE_CHANGE_REQUEST);
    VbglR3Close(hDevice);
    return 0;
}
