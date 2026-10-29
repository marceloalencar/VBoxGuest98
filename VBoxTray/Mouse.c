/** @file
 * VBoxTray - Mouse pointer integration.
 *
 * Windows 98 drives the PS/2 mouse with VxDs, so instead of a WDM filter
 * driver the pointer is positioned from user mode: once the guest reports
 * VMMDEV_MOUSE_GUEST_CAN_ABSOLUTE the host stops sending relative movement
 * over PS/2 (buttons still go that way) and raises
 * VMMDEV_EVENT_MOUSE_POSITION_CHANGED instead.  The absolute position
 * (0..0xffff) is fed to mouse_event, which uses the same normalized range.
 *
 * mouse_event only reaches USER, though: DirectInput and MS-DOS programs get
 * the mouse from VMOUSE, which only hears from the mouse drivers.  So when
 * VBOXMOU.VXD is available every move is also posted to VMOUSE as a
 * relative move in pixels.  VMOUSE then moves the Windows pointer too, with
 * USER's acceleration, so the move posted is the one USER accelerates to
 * the host's (what a real mouse would have sent; DirectInput gets that one
 * too), and when Windows has the mouse the absolute position follows right
 * away with mouse_event, which DirectInput and MS-DOS don't see, for what
 * is left (rounding).  Without the compensation the pointer overshot and
 * came back on every fast move.  When a full screen MS-DOS program has the mouse, the VxD puts its
 * INT 33h pointer at the host position instead, as the program counts the
 * relative moves its own way.  Should either pointer still end up
 * elsewhere, it is put back once the movement settles.
 */

#include "VBoxTray.h"
#include <stdlib.h>
#include <string.h>


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/

/** The mouse features we tell the host about. */
#define VBOXMOUSE_GUEST_FEATURES    (VMMDEV_MOUSE_GUEST_CAN_ABSOLUTE | VMMDEV_MOUSE_NEW_PROTOCOL)
/** The events we are interested in. */
#define VBOXMOUSE_EVENTS            (VMMDEV_EVENT_MOUSE_POSITION_CHANGED | VMMDEV_EVENT_MOUSE_CAPABILITIES_CHANGED)
/** How often to check for shutdown while no events arrive (ms). */
#define VBOXMOUSE_POLL_MS           500
/** Check the pointer against the host position after this long without moves (ms). */
#define VBOXMOUSE_SETTLE_MS         100
/** How many pointer corrections to log; they shouldn't happen at all. */
#define VBOXMOUSE_MAX_CORRECT_LOGS  10

/** VBOXMOU.VXD, found in the system directory, and its DeviceIoControl codes
 *  (see VBoxMou\VBoxMou.asm). */
#define VBOXMOU_VXD_NAME            "\\\\.\\VBOXMOU.VXD"
/** In: four LONGs, dx and dy in pixels, then the host x and y (0..0xffff);
 *  out (optional): as VBOXMOU_IOCTL_OWNER_IS_SYS. */
#define VBOXMOU_IOCTL_POST_MOVE     1
/** Out: DWORD, 1 if the system VM has the mouse, 0 if an MS-DOS VM has it. */
#define VBOXMOU_IOCTL_OWNER_IS_SYS  2
/** In: two LONGs, the host x and y (0..0xffff); puts the MS-DOS pointer
 *  there if an MS-DOS VM has the mouse. */
#define VBOXMOU_IOCTL_SET_DOS_POS   3


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/

/** Mouse thread state. */
typedef struct VBOXMOUSESTATE
{
    /** The VxD, INVALID_HANDLE_VALUE when not available. */
    HANDLE  hVxD;
    /** Last host position (0..0xffff), -1 when there is none. */
    INT32   x;
    INT32   y;
    /** The same in pixels. */
    LONG    xPx;
    LONG    yPx;
    /** The screen size they were worked out with. */
    int     cx;
    int     cy;
    /** A move was posted to the VxD since the pointer was last put back. */
    BOOL    fCorrect;
    /** GetTickCount of the last move. */
    DWORD   dwLastMove;
    /** Corrections logged so far. */
    unsigned cCorrectLogs;
} VBOXMOUSESTATE;


/**
 * Loads VBOXMOU.VXD; it is unloaded when the handle is closed.
 */
static HANDLE vboxMouseOpenVxD(void)
{
    HANDLE hVxD = CreateFile(VBOXMOU_VXD_NAME, 0, 0, NULL, 0, FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (hVxD == INVALID_HANDLE_VALUE)
        VBoxTrayLogRel("Mouse: VBOXMOU.VXD not loaded (error %u), DirectInput and MS-DOS programs won't see the moves\n",
                       GetLastError());
    return hVxD;
}

/**
 * Checks whether Windows has the mouse, not a full screen MS-DOS program.
 */
static BOOL vboxMouseWindowsOwnsMouse(VBOXMOUSESTATE *pState)
{
    DWORD fSysVM = 1;
    DWORD cb = 0;

    DeviceIoControl(pState->hVxD, VBOXMOU_IOCTL_OWNER_IS_SYS, NULL, 0, &fSysVM, sizeof(fSysVM), &cb, NULL);
    return fSysVM != 0;
}

/**
 * Puts the pointer at the last host position: the Windows one, or the
 * MS-DOS one when a full screen MS-DOS program has the mouse.
 */
static void vboxMouseCorrect(VBOXMOUSESTATE *pState)
{
    BOOL fAfterPost = pState->fCorrect;

    pState->fCorrect = FALSE;
    if (pState->hVxD != INVALID_HANDLE_VALUE)
    {
        POINT Pt;
        if (!vboxMouseWindowsOwnsMouse(pState))
        {
            LONG aPos[2];
            DWORD cb = 0;
            aPos[0] = pState->x;
            aPos[1] = pState->y;
            DeviceIoControl(pState->hVxD, VBOXMOU_IOCTL_SET_DOS_POS, aPos, sizeof(aPos), NULL, 0, &cb, NULL);
            return;
        }
        if (!GetCursorPos(&Pt) || (Pt.x == pState->xPx && Pt.y == pState->yPx))
            return;
        if (   GetSystemMetrics(SM_CXSCREEN) != pState->cx
            || GetSystemMetrics(SM_CYSCREEN) != pState->cy)
            fAfterPost = FALSE; /* The display mode changed since, the pixels don't compare. */
        if (fAfterPost && pState->cCorrectLogs < VBOXMOUSE_MAX_CORRECT_LOGS)
        {
            pState->cCorrectLogs++;
            VBoxTrayLogRel("Mouse: pointer at %d,%d instead of %d,%d, put back\n", Pt.x, Pt.y, pState->xPx, pState->yPx);
        }
    }
    mouse_event(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE, (DWORD)pState->x, (DWORD)pState->y, 0, 0);
}

/**
 * Turns the move the pointer must make into the one to post, which USER's
 * acceleration (SPI_GETMOUSE) brings to it: when the move along either
 * axis is over the first threshold USER doubles both, and over the second
 * one too with speed 2 it doubles them again.  When no posted move lands
 * exactly, the largest that doesn't overshoot is posted.
 *
 * @param   pdx     In: the move in pixels; out: the move to post.
 * @param   pdy     The same for y.
 */
static void vboxMouseCompensate(LONG *pdx, LONG *pdy)
{
    int aMouse[3]; /* First threshold, second threshold, speed. */
    LONG m = labs(*pdx) > labs(*pdy) ? labs(*pdx) : labs(*pdy);
    LONG lLimit;

    if (!SystemParametersInfo(SPI_GETMOUSE, 0, aMouse, 0) || !aMouse[2] || m <= aMouse[0])
        return; /* Not accelerated. */
    if (aMouse[2] >= 2 && m / 4 > aMouse[0] && m / 4 > aMouse[1])
    {
        *pdx /= 4;
        *pdy /= 4;
        return;
    }
    if (m / 2 > aMouse[0] && (aMouse[2] < 2 || m / 2 <= aMouse[1]))
    {
        *pdx /= 2;
        *pdy /= 2;
        return;
    }
    lLimit = aMouse[0];
    if (aMouse[2] >= 2 && aMouse[1] > aMouse[0] && 2 * (LONG)aMouse[1] <= m)
        lLimit = aMouse[1];
    *pdx = MulDiv(*pdx, lLimit, m);
    *pdy = MulDiv(*pdy, lLimit, m);
}

/**
 * Follows the host position if the host wants absolute mode.
 *
 * @param   hDevice     The device handle.
 * @param   pState      The mouse thread state.
 */
static void vboxMouseUpdate(HANDLE hDevice, VBOXMOUSESTATE *pState)
{
    UINT32 fFeatures = 0;
    INT32 x = 0;
    INT32 y = 0;
    int cx;
    int cy;
    LONG xPx;
    LONG yPx;

    if (RT_FAILURE(VbglR3GetMouseStatus(hDevice, &fFeatures, &x, &y)))
        return;
    if (!(fFeatures & VMMDEV_MOUSE_HOST_WANTS_ABSOLUTE))
    {
        /* Mouse integration disabled on the host, PS/2 does the job. */
        pState->x = pState->y = -1;
        pState->fCorrect = FALSE;
        return;
    }

    if (x < VMMDEV_MOUSE_RANGE_MIN) x = VMMDEV_MOUSE_RANGE_MIN;
    if (x > VMMDEV_MOUSE_RANGE_MAX) x = VMMDEV_MOUSE_RANGE_MAX;
    if (y < VMMDEV_MOUSE_RANGE_MIN) y = VMMDEV_MOUSE_RANGE_MIN;
    if (y > VMMDEV_MOUSE_RANGE_MAX) y = VMMDEV_MOUSE_RANGE_MAX;
    if (x == pState->x && y == pState->y)
        return;

    /* The pixel mouse_event puts the normalized position at. */
    cx = GetSystemMetrics(SM_CXSCREEN);
    cy = GetSystemMetrics(SM_CYSCREEN);
    xPx = (LONG)(((DWORD)x * (DWORD)cx) >> 16);
    yPx = (LONG)(((DWORD)y * (DWORD)cy) >> 16);

    /* The first position, or the first in a new display mode: just put the
       pointer there, a move between two modes would be meaningless. */
    if (   pState->hVxD == INVALID_HANDLE_VALUE
        || pState->x < 0
        || cx != pState->cx
        || cy != pState->cy)
    {
        pState->x = x;
        pState->y = y;
        pState->xPx = xPx;
        pState->yPx = yPx;
        pState->cx = cx;
        pState->cy = cy;
        pState->fCorrect = FALSE;
        vboxMouseCorrect(pState);
        return;
    }

    if (xPx != pState->xPx || yPx != pState->yPx)
    {
        LONG aMove[4];
        DWORD fSysVM = 0;
        DWORD cb = 0;
        aMove[0] = xPx - pState->xPx;
        aMove[1] = yPx - pState->yPx;
        vboxMouseCompensate(&aMove[0], &aMove[1]);
        aMove[2] = x;
        aMove[3] = y;
        DeviceIoControl(pState->hVxD, VBOXMOU_IOCTL_POST_MOVE, aMove, sizeof(aMove), &fSysVM, sizeof(fSysVM), &cb, NULL);
        /* VMOUSE gave USER the move when the call returned to the system VM,
           so this comes after it. */
        if (fSysVM)
            mouse_event(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE, (DWORD)x, (DWORD)y, 0, 0);
        pState->fCorrect = TRUE;
    }
    pState->x = x;
    pState->y = y;
    pState->xPx = xPx;
    pState->yPx = yPx;
    pState->dwLastMove = GetTickCount();
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
    VBOXMOUSESTATE State;
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
        VBoxTrayLogRel("Mouse: setup failed, rc=%d\n", rc);
        VbglR3Close(hDevice);
        return 1;
    }

    memset(&State, 0, sizeof(State));
    State.x = State.y = -1;
    State.hVxD = vboxMouseOpenVxD();

    while (!g_fVBoxTrayShutdown)
    {
        DWORD cMsWait = State.fCorrect ? VBOXMOUSE_SETTLE_MS : VBOXMOUSE_POLL_MS;
        rc = VbglR3WaitForEvents(hDevice, VBOXMOUSE_EVENTS, cMsWait, &fEvents);
        if (RT_SUCCESS(rc))
            vboxMouseUpdate(hDevice, &State);
        else if (rc != VERR_TIMEOUT && rc != VERR_INTERRUPTED)
            Sleep(VBOXMOUSE_POLL_MS); /* Device stopped or similar, don't spin. */
        if (State.fCorrect && GetTickCount() - State.dwLastMove >= VBOXMOUSE_SETTLE_MS)
            vboxMouseCorrect(&State);
    }

    if (State.hVxD != INVALID_HANDLE_VALUE)
        CloseHandle(State.hVxD);
    /* Closing the handle resets the session's mouse status as well, but be explicit. */
    VbglR3SetMouseStatus(hDevice, 0);
    VbglR3Close(hDevice);
    return 0;
}
