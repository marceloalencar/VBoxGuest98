/** @file
 * VBoxTray - Drag and drop between the host and the guest (files and text),
 * VBoxDragAndDropSvc HGCM protocol v3, as upstream VBoxDnD.cpp.
 *
 * Two threads, each with its own HGCM client (like upstream):
 *  - The worker (VBoxDnDThread) blocks in GUEST_DND_FN_GET_NEXT_HOST_MSG,
 *    fetches the host messages and passes them on to the proxy window.  It
 *    also does the transfers: it receives the data dropped on the guest
 *    (files and directories are written to %TEMP%\VirtualBox Dropped
 *    Files\<time stamp>) and sends the data dropped on the host.
 *  - The proxy window thread (vboxDnDUiThread) runs the OLE side.
 *
 * Host to guest: when the host drag enters the guest, the topmost proxy
 * window covers the screen and the left button is pressed on it with
 * mouse_event; the button down starts DoDragDrop, and the pointer follows
 * the host's moves.  On the drop, the data is requested from the host and
 * the button released once it is here, so the drop target gets it right
 * away (CF_HDROP for files, CF_TEXT for text).
 *
 * Guest to host: when the host pointer leaves the VM window, the host asks
 * whether a guest drag is going on.  The proxy window, a drop target, then
 * covers the screen and the left button is released on it, so a guest drag
 * in progress drops there; its data is kept until the user drops on the
 * host, and then sent.
 *
 * Only copying is offered, both ways.
 *
 * This file has the proxy window and the worker; DnDHgcm.c has the HGCM
 * calls, DnDTransfer.c the transfers, DnDUtil.c the text conversions and
 * DnDObj.c the OLE objects.
 */

#include "DnD.h"
#include <shellapi.h>
#include <process.h>
#include <stdlib.h>
#include <string.h>


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/

/** Worker -> window: the host drag entered.  lParam: VBOXDNDENTER (malloc'ed). */
#define WM_VBOXDND_ENTER            (WM_APP + 20)
/** Worker -> window: the host pointer moved.  wParam: x, lParam: y. */
#define WM_VBOXDND_MOVE             (WM_APP + 21)
/** Worker -> window: the host drag left the guest or was cancelled. */
#define WM_VBOXDND_LEAVE            (WM_APP + 22)
/** Worker -> window: dropped on the guest. */
#define WM_VBOXDND_DROP             (WM_APP + 23)
/** Worker -> window: the data arrived (wParam TRUE, it is in the data
 *  object) or could not be received (FALSE). */
#define WM_VBOXDND_RECEIVED         (WM_APP + 24)
/** Worker -> window: shut down. */
#define WM_VBOXDND_QUIT             (WM_APP + 25)
/** Worker -> window: the host asks whether a guest drag is going on. */
#define WM_VBOXDND_GH_PENDING       (WM_APP + 26)
/** Worker -> window: the guest to host transfer is over. */
#define WM_VBOXDND_GH_RESET         (WM_APP + 27)
/** Worker -> window: the host cancelled the operation. */
#define WM_VBOXDND_CANCEL           (WM_APP + 28)
/** Worker -> window: the VM was restored from a saved state, redo the
 *  protocol handshake. */
#define WM_VBOXDND_RESTORED         (WM_APP + 29)

/** Timer: hide the proxy window if no guest drag showed up. */
#define VBOXDND_GH_TIMER_HIDE       1
/** Timer: forget a caught guest drag the host never asked for. */
#define VBOXDND_GH_TIMER_EXPIRE     2
/** How long a guest drag may take to reach the proxy window (ms). */
#define VBOXDND_GH_HIDE_MS          500
/** How long a caught guest drag is kept for the host (ms). */
#define VBOXDND_GH_EXPIRE_MS        60000
/** How long the host's drop waits for the guest drop on the proxy window (ms), as upstream. */
#define VBOXDND_GH_DROP_WAIT_MS     5000

/** Window class of the proxy window. */
#define VBOXDND_WND_CLASS           "VBoxTrayDnDWndClass"
/** Consecutive failures after which the worker gives up, like upstream. */
#define VBOXDND_MAX_ERRORS          32


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/

/** A format the guest accepts and how it is handed to the drop target. */
typedef struct VBOXDNDFORMAT
{
    const char *pszMime;
    CLIPFORMAT  cf;
} VBOXDNDFORMAT;

/** Proxy window thread start up. */
typedef struct VBOXDNDUISTART
{
    HANDLE hEvtReady;
    BOOL   fOk;
} VBOXDNDUISTART;

/** State only the proxy window thread uses. */
typedef struct VBOXDNDUISTATE
{
    /** The window thread's handle and HGCM client (for the answers to the host). */
    HANDLE               hDevice;
    UINT32               idClient;
    /** The drop source of the current drag. */
    IDropSource         *pDropSource;
    /** The format chosen at the enter, requested at the drop. */
    const VBOXDNDFORMAT *pFormat;
    /** Whether a host drag is in the guest (the proxy window is up). */
    BOOL                 fActive;
    /** Whether we pressed the left button. */
    BOOL                 fButtonDown;
    /** Whether DoDragDrop is running. */
    BOOL                 fDragging;
    /** An enter that came while DoDragDrop was still running (malloc'ed). */
    VBOXDNDENTER        *pPendingEnter;
    /** The drop target catching guest drags for the host, NULL if not registered. */
    IDropTarget         *pDropTarget;
    /** Whether the proxy window is up to catch a guest drag. */
    BOOL                 fGhActive;
    /** Whether a guest drag is over the proxy window (DragEnter came, the drop not yet). */
    BOOL                 fGhOver;
    /** The format of the caught guest drag, NULL if none. */
    const char          *pszGhFormat;
} VBOXDNDUISTATE;

/** State the worker shares with the proxy window thread. */
typedef struct VBOXDNDSHARED
{
    /** The worker's handle (overlapped, so VBoxDnDStop can disconnect). */
    HANDLE               hDevice;
    /** The worker's HGCM client ID, 0 if not connected. */
    UINT32               idClient;
    /** The data object of the current drag (a reference of its own). */
    IDataObject         *pDataObj;
    /** The data of the guest drag dropped on the proxy window (malloc'ed):
     *  a CF_HDROP style path list if fGhFiles, else UTF-8 text. */
    char                *pszGhData;
    BOOL                 fGhFiles;
    /** Protects the members above. */
    CRITICAL_SECTION     CritSect;
    /** Signalled when pszGhData is set. */
    HANDLE               hGhDropped;
    /** The proxy window, set before the window thread says it is ready and
     *  cleared after its message loop; the worker only posts to it. */
    HWND                 hwnd;
} VBOXDNDSHARED;


/*********************************************************************************************************************************
*   Global Variables                                                                                                             *
*********************************************************************************************************************************/

/** The formats the guest accepts, in order of preference. */
static const VBOXDNDFORMAT g_aDnDFormats[] =
{
    { "text/uri-list",              CF_HDROP },
    { "text/plain;charset=utf-8",   CF_TEXT },
    { "UTF8_STRING",                CF_TEXT },
    { "text/plain",                 CF_TEXT },
    { "TEXT",                       CF_TEXT },
    { "STRING",                     CF_TEXT },
};

/** Proxy window thread only. */
static VBOXDNDUISTATE g_DnDUi = { INVALID_HANDLE_VALUE };
/** Shared by the worker and the proxy window thread. */
static VBOXDNDSHARED g_DnDShared = { INVALID_HANDLE_VALUE };


/*********************************************************************************************************************************
*   Proxy window (window thread)                                                                                                 *
*********************************************************************************************************************************/

static void vboxDnDGhReset(void);

/**
 * Moves the pointer to a guest screen position, pressing the left button
 * the first time.
 */
static void vboxDnDMouseMove(UINT32 x, UINT32 y)
{
    int cx = GetSystemMetrics(SM_CXSCREEN);
    int cy = GetSystemMetrics(SM_CYSCREEN);
    DWORD fFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;

    if ((int)x >= cx)
        x = cx - 1;
    if ((int)y >= cy)
        y = cy - 1;
    if (!g_DnDUi.fButtonDown)
    {
        fFlags |= MOUSEEVENTF_LEFTDOWN;
        g_DnDUi.fButtonDown = TRUE;
    }
    mouse_event(fFlags, (DWORD)MulDiv(x, 65535, cx > 1 ? cx - 1 : 1), (DWORD)MulDiv(y, 65535, cy > 1 ? cy - 1 : 1), 0, 0);
}

/**
 * Releases the left button if we pressed it.
 */
static void vboxDnDMouseRelease(void)
{
    if (g_DnDUi.fButtonDown)
    {
        g_DnDUi.fButtonDown = FALSE;
        mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
    }
}

/**
 * Releases the OLE objects of the drag, once DoDragDrop isn't using them.
 */
static void vboxDnDReleaseObjects(void)
{
    IDataObject *pDataObj;

    EnterCriticalSection(&g_DnDShared.CritSect);
    pDataObj = g_DnDShared.pDataObj;
    g_DnDShared.pDataObj = NULL;
    LeaveCriticalSection(&g_DnDShared.CritSect);
    if (pDataObj)
        pDataObj->lpVtbl->Release(pDataObj);
    if (g_DnDUi.pDropSource)
    {
        g_DnDUi.pDropSource->lpVtbl->Release(g_DnDUi.pDropSource);
        g_DnDUi.pDropSource = NULL;
    }
    g_DnDUi.pFormat = NULL;
}

/**
 * Ends the current host drag: cancels DoDragDrop if it runs, releases the
 * button and hides the proxy window.
 */
static void vboxDnDAbort(void)
{
    EnterCriticalSection(&g_DnDShared.CritSect);
    if (g_DnDShared.pDataObj)
        VBoxDnDDataObjectAbort(g_DnDShared.pDataObj);
    LeaveCriticalSection(&g_DnDShared.CritSect);
    if (g_DnDUi.pDropSource)
        VBoxDnDDropSourceAbort(g_DnDUi.pDropSource); /* Before releasing the button, or it would drop. */

    vboxDnDMouseRelease();
    ShowWindow(g_DnDShared.hwnd, SW_HIDE);
    g_DnDUi.fActive = FALSE;
    if (!g_DnDUi.fDragging)
        vboxDnDReleaseObjects(); /* Otherwise when DoDragDrop returns. */
}

/**
 * Picks the first format of our list the host offers.
 */
static const VBOXDNDFORMAT *vboxDnDPickFormat(const char *pszFormats)
{
    unsigned i;

    for (i = 0; i < sizeof(g_aDnDFormats) / sizeof(g_aDnDFormats[0]); i++)
    {
        const char *psz = pszFormats;
        size_t cchFmt = strlen(g_aDnDFormats[i].pszMime);
        while (*psz)
        {
            size_t cch = strcspn(psz, VBOX_DND_FORMATS_SEPARATOR);
            if (cch == cchFmt && !_strnicmp(psz, g_aDnDFormats[i].pszMime, cch))
                return &g_aDnDFormats[i];
            psz += cch;
            while (*psz == '\r' || *psz == '\n')
                psz++;
        }
    }
    return NULL;
}

/**
 * Tells the host what a drop at the current position would do.
 */
static void vboxDnDAck(void)
{
    UINT32 uAction = VBOX_DND_ACTION_IGNORE;

    if (   g_DnDUi.fDragging
        && g_DnDUi.pDropSource
        && (VBoxDnDDropSourceGetEffect(g_DnDUi.pDropSource) & DROPEFFECT_COPY))
        uAction = VBOX_DND_ACTION_COPY;
    VBoxDnDSendAck(g_DnDUi.hDevice, g_DnDUi.idClient, uAction);
}

/**
 * The pointer moved (also right after the enter).
 */
static void vboxDnDOnMove(UINT32 x, UINT32 y)
{
    if (g_DnDUi.fActive)
        vboxDnDMouseMove(x, y);
    vboxDnDAck();
}

/**
 * The host drag entered the guest: prepare the drag objects and cover the
 * screen with the proxy window, which gets the button press.
 *
 * @param   pEnter      The enter data (malloc'ed, freed here).
 */
static void vboxDnDOnEnter(VBOXDNDENTER *pEnter)
{
    if (g_DnDUi.fDragging)
    {
        /* A new drag while the old one still runs (its leave got lost):
           end the old one, and take this one when DoDragDrop returns. */
        vboxDnDAbort();
        free(g_DnDUi.pPendingEnter);
        g_DnDUi.pPendingEnter = pEnter;
        vboxDnDAck();
        return;
    }
    if (g_DnDUi.fActive)
        vboxDnDAbort();
    vboxDnDGhReset(); /* A host drag replaces a guest one. */

    g_DnDUi.pFormat = vboxDnDPickFormat(pEnter->szFormats);
    if (!g_DnDUi.pFormat || !(pEnter->fAllActions & VBOX_DND_ACTION_COPY))
        VBoxTrayLogRel("DnD: nothing the guest can take (actions %#lx, formats %s)\n", pEnter->fAllActions,
                       pEnter->szFormats);
    else
    {
        IDataObject *pDataObj = VBoxDnDDataObjectCreate(g_DnDUi.pFormat->cf);
        g_DnDUi.pDropSource = VBoxDnDDropSourceCreate();
        if (pDataObj && g_DnDUi.pDropSource)
        {
            EnterCriticalSection(&g_DnDShared.CritSect);
            g_DnDShared.pDataObj = pDataObj;
            LeaveCriticalSection(&g_DnDShared.CritSect);

            VBoxTrayLogRel("DnD: host drag entered, using %s\n", g_DnDUi.pFormat->pszMime);
            SetWindowPos(g_DnDShared.hwnd, HWND_TOPMOST, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
                         SWP_SHOWWINDOW | SWP_NOACTIVATE | SWP_NOREDRAW);
            g_DnDUi.fActive = TRUE;
        }
        else
        {
            if (pDataObj)
                pDataObj->lpVtbl->Release(pDataObj);
            vboxDnDReleaseObjects();
        }
    }

    vboxDnDOnMove(pEnter->x, pEnter->y);
    free(pEnter);
}

/**
 * Dropped on the guest: request the data.  The button stays down until it
 * is here (WM_VBOXDND_RECEIVED).
 */
static void vboxDnDOnDrop(void)
{
    IDataObject *pDataObj;
    int rc;

    if (!g_DnDUi.fDragging || !g_DnDUi.pFormat)
    {
        /* Nothing to drop on (like upstream, the host isn't asked for data). */
        VBoxTrayLogRel("DnD: dropped, but no guest drag is in progress\n");
        vboxDnDAbort();
        return;
    }

    EnterCriticalSection(&g_DnDShared.CritSect);
    pDataObj = g_DnDShared.pDataObj;
    if (pDataObj)
        VBoxDnDDataObjectSetDropping(pDataObj);
    LeaveCriticalSection(&g_DnDShared.CritSect);

    VBoxTrayLogRel("DnD: dropped, requesting %s\n", g_DnDUi.pFormat->pszMime);
    rc = VBoxDnDSendReqData(g_DnDUi.hDevice, g_DnDUi.idClient, g_DnDUi.pFormat->pszMime);
    if (RT_FAILURE(rc))
    {
        VBoxTrayLogRel("DnD: requesting the data failed, rc=%d\n", rc);
        vboxDnDAbort();
    }
}

/**
 * The left button went down on the proxy window: start the OLE drag.
 * Returns when the drop is done or cancelled; the worker's messages are
 * dispatched meanwhile by DoDragDrop's message loop.
 */
static void vboxDnDStartDrag(void)
{
    IDataObject *pDataObj;
    IDropSource *pDropSource = g_DnDUi.pDropSource;
    DWORD dwEffect = DROPEFFECT_NONE;
    HRESULT hr;

    EnterCriticalSection(&g_DnDShared.CritSect);
    pDataObj = g_DnDShared.pDataObj;
    LeaveCriticalSection(&g_DnDShared.CritSect);
    if (!g_DnDUi.fActive || g_DnDUi.fDragging || !pDataObj || !pDropSource)
        return;

    /* Out of the way, so the windows below get the drag. */
    ShowWindow(g_DnDShared.hwnd, SW_HIDE);
    g_DnDUi.fDragging = TRUE;
    pDataObj->lpVtbl->AddRef(pDataObj);
    pDropSource->lpVtbl->AddRef(pDropSource);

    hr = DoDragDrop(pDataObj, pDropSource, DROPEFFECT_COPY, &dwEffect);
    if (hr == DRAGDROP_S_DROP)
        VBoxTrayLogRel("DnD: dropped on the guest, effect %#lx\n", dwEffect);
    else if (hr == DRAGDROP_S_CANCEL)
        VBoxTrayLogRel("DnD: guest drag cancelled\n");
    else
        VBoxTrayLogRel("DnD: DoDragDrop failed, hr=%#lx\n", hr);

    pDropSource->lpVtbl->Release(pDropSource);
    pDataObj->lpVtbl->Release(pDataObj);
    g_DnDUi.fDragging = FALSE;
    vboxDnDAbort(); /* Releases the button if still down, and the objects. */

    if (g_DnDUi.pPendingEnter)
    {
        VBOXDNDENTER *pEnter = g_DnDUi.pPendingEnter;
        g_DnDUi.pPendingEnter = NULL;
        vboxDnDOnEnter(pEnter);
    }
}

/**
 * Answers the host's pending query with the caught guest drag, if any.
 * Only ever as the answer to a query: the host waits for each answer with
 * one event, so an answer coming late or unasked would be taken for the
 * answer to its next request.
 */
static void vboxDnDGhAnswer(void)
{
    static const char *s_pszLastLogged = (const char *)1;
    const char *pszFormat;
    int rc;

    EnterCriticalSection(&g_DnDShared.CritSect);
    pszFormat = g_DnDUi.pszGhFormat;
    LeaveCriticalSection(&g_DnDShared.CritSect);

    rc = VBoxDnDSendAckPending(g_DnDUi.hDevice, g_DnDUi.idClient, pszFormat ? VBOX_DND_ACTION_COPY : VBOX_DND_ACTION_IGNORE,
                               pszFormat ? pszFormat : "");
    /* The host asks on every pointer move; log changes only. */
    if (pszFormat != s_pszLastLogged || RT_FAILURE(rc))
        VBoxTrayLogRel("DnD: answered the pending query with %s, rc=%d\n", pszFormat ? pszFormat : "none", rc);
    s_pszLastLogged = pszFormat;
}

/**
 * Ends a guest to host operation: hides the proxy window and forgets the
 * caught drag.
 */
static void vboxDnDGhReset(void)
{
    char *pszData;

    KillTimer(g_DnDShared.hwnd, VBOXDND_GH_TIMER_HIDE);
    KillTimer(g_DnDShared.hwnd, VBOXDND_GH_TIMER_EXPIRE);
    if (g_DnDUi.fGhActive && !g_DnDUi.fGhOver) /* Else when the guest drag ends (vboxDnDGhDragDone). */
    {
        ShowWindow(g_DnDShared.hwnd, SW_HIDE);
        g_DnDUi.fGhActive = FALSE;
    }

    EnterCriticalSection(&g_DnDShared.CritSect);
    g_DnDUi.pszGhFormat = NULL;
    pszData = g_DnDShared.pszGhData;
    g_DnDShared.pszGhData = NULL;
    ResetEvent(g_DnDShared.hGhDropped);
    LeaveCriticalSection(&g_DnDShared.CritSect);
    free(pszData);
}

/**
 * The host asks whether a guest drag is going on: its pointer left the VM
 * window with the button down.  As upstream, the proxy window covers the
 * screen and the left button is released on it, so a guest drag drops on our
 * drop target (otherwise the window just gets the button release).
 *
 * The host waits only 100 ms for the answer, and the drag reaches the window
 * later, so the first query is answered with what we have, usually nothing.
 * The host asks again on every move of its pointer, and finds the drag then.
 */
static void vboxDnDGhOnPending(void)
{
    POINT Pt;
    int cx;
    int cy;

    if (g_DnDUi.fActive || g_DnDUi.fDragging || !g_DnDUi.pDropTarget)
    {
        /* A host drag is in the guest, or no way to catch one: answer "none". */
        VBoxDnDSendAckPending(g_DnDUi.hDevice, g_DnDUi.idClient, VBOX_DND_ACTION_IGNORE, "");
        return;
    }

    if (!g_DnDUi.fGhActive)
    {
        cx = GetSystemMetrics(SM_CXSCREEN);
        cy = GetSystemMetrics(SM_CYSCREEN);
        SetWindowPos(g_DnDShared.hwnd, HWND_TOPMOST, 0, 0, cx, cy, SWP_SHOWWINDOW | SWP_NOACTIVATE | SWP_NOREDRAW);
        g_DnDUi.fGhActive = TRUE;

        /* A move first, so the drag notices the window under the pointer. */
        GetCursorPos(&Pt);
        mouse_event(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE, (DWORD)MulDiv(Pt.x, 65535, cx > 1 ? cx - 1 : 1),
                    (DWORD)MulDiv(Pt.y, 65535, cy > 1 ? cy - 1 : 1), 0, 0);
        mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
    }
    /* Take the window away again if no guest drag shows up. */
    if (!g_DnDUi.pszGhFormat)
        SetTimer(g_DnDShared.hwnd, VBOXDND_GH_TIMER_HIDE, VBOXDND_GH_HIDE_MS, NULL);

    vboxDnDGhAnswer();
}

/**
 * Gets the data of a guest drag: a CF_HDROP style path list or UTF-8 text.
 *
 * @returns The data (malloc'ed), NULL on failure.
 */
static char *vboxDnDGhGetData(CLIPFORMAT cfFormat, IDataObject *pDataObj)
{
    FORMATETC FormatEtc;
    STGMEDIUM Medium;
    char *pszData = NULL;

    FormatEtc.cfFormat = cfFormat;
    FormatEtc.ptd      = NULL;
    FormatEtc.dwAspect = DVASPECT_CONTENT;
    FormatEtc.lindex   = -1;
    FormatEtc.tymed    = TYMED_HGLOBAL;
    if (SUCCEEDED(pDataObj->lpVtbl->GetData(pDataObj, &FormatEtc, &Medium)))
    {
        if (Medium.tymed == TYMED_HGLOBAL)
        {
            if (cfFormat == CF_HDROP)
                pszData = VBoxDnDCopyHDrop((HDROP)Medium.hGlobal);
            else
            {
                const char *pch = (const char *)GlobalLock(Medium.hGlobal);
                if (pch)
                {
                    /* Make sure it is terminated. */
                    DWORD cb = (DWORD)GlobalSize(Medium.hGlobal);
                    char *pszAnsi = (char *)malloc(cb + 1);
                    if (pszAnsi)
                    {
                        memcpy(pszAnsi, pch, cb);
                        pszAnsi[cb] = '\0';
                        pszData = VBoxTrayAnsiToUtf8(pszAnsi);
                        free(pszAnsi);
                    }
                    GlobalUnlock(Medium.hGlobal);
                }
            }
        }
        ReleaseStgMedium(&Medium);
    }
    return pszData;
}

/**
 * A guest drag reached the proxy window (drop target callback): take its
 * data right away.
 *
 * Waiting for the drop doesn't work on Windows 98: while the user drags on
 * the host with the button held, the PS/2 mouse keeps reporting the button
 * down, so our release doesn't end the guest drag, and the drop comes only
 * when the real button goes up, maybe after the host's drop.
 *
 * @param   cfFormat    CF_HDROP, CF_TEXT, or 0 for none we take.
 */
void VBoxDnDGhOnDragEnter(CLIPFORMAT cfFormat, IDataObject *pDataObj)
{
    const char *pszFormat = NULL;
    char *pszData;

    if (!g_DnDUi.fGhActive)
        return;
    g_DnDUi.fGhOver = TRUE;
    if (cfFormat == CF_HDROP)
        pszFormat = "text/uri-list";
    else if (cfFormat == CF_TEXT)
        pszFormat = "text/plain;charset=utf-8";
    else
        return;

    pszData = vboxDnDGhGetData(cfFormat, pDataObj);
    if (!pszData)
    {
        VBoxTrayLogRel("DnD: getting the guest drag's data failed\n");
        return;
    }

    EnterCriticalSection(&g_DnDShared.CritSect);
    free(g_DnDShared.pszGhData);
    g_DnDShared.pszGhData = pszData;
    g_DnDShared.fGhFiles  = cfFormat == CF_HDROP;
    g_DnDUi.pszGhFormat   = pszFormat;
    SetEvent(g_DnDShared.hGhDropped);
    LeaveCriticalSection(&g_DnDShared.CritSect);
    SetTimer(g_DnDShared.hwnd, VBOXDND_GH_TIMER_EXPIRE, VBOXDND_GH_EXPIRE_MS, NULL);

    KillTimer(g_DnDShared.hwnd, VBOXDND_GH_TIMER_HIDE);
    VBoxTrayLogRel("DnD: guest drag caught for the host, offering %s\n", pszFormat);
    /* The host's next query gets it (vboxDnDGhAnswer). */
}

/**
 * The guest drag is over: the proxy window isn't needed any more.  Until
 * then it stays up, or the drag would drop on the window below it.
 */
static void vboxDnDGhDragDone(void)
{
    g_DnDUi.fGhOver = FALSE;
    if (g_DnDUi.fGhActive)
    {
        ShowWindow(g_DnDShared.hwnd, SW_HIDE);
        g_DnDUi.fGhActive = FALSE;
    }
}

/**
 * The guest drag was dropped on the proxy window (drop target callback).
 *
 * @returns TRUE if its data was taken (the guest sees a copy), FALSE if not.
 */
BOOL VBoxDnDGhOnDrop(void)
{
    BOOL fTaken;

    EnterCriticalSection(&g_DnDShared.CritSect);
    fTaken = g_DnDUi.pszGhFormat != NULL;
    LeaveCriticalSection(&g_DnDShared.CritSect);
    vboxDnDGhDragDone();
    return fTaken;
}

/**
 * The guest drag left the proxy window (drop target callback): cancelled.
 */
void VBoxDnDGhOnDragLeave(void)
{
    vboxDnDGhDragDone();
}

/**
 * Proxy window procedure.
 */
static LRESULT CALLBACK vboxDnDWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;

        case WM_ERASEBKGND:
            return 1; /* Stay see-through for the moment it is visible. */

        case WM_PAINT:
        {
            PAINTSTRUCT Ps;
            BeginPaint(hwnd, &Ps);
            EndPaint(hwnd, &Ps);
            return 0;
        }

        case WM_LBUTTONDOWN:
            vboxDnDStartDrag();
            return 0;

        case WM_VBOXDND_ENTER:
            vboxDnDOnEnter((VBOXDNDENTER *)lParam);
            return 0;

        case WM_VBOXDND_MOVE:
            vboxDnDOnMove((UINT32)wParam, (UINT32)lParam);
            return 0;

        case WM_VBOXDND_LEAVE:
            if (g_DnDUi.fActive || g_DnDUi.fDragging)
                VBoxTrayLogRel("DnD: host drag left the guest or was cancelled\n");
            vboxDnDAbort();
            return 0;

        case WM_VBOXDND_DROP:
            vboxDnDOnDrop();
            return 0;

        case WM_VBOXDND_RECEIVED:
            if (wParam && g_DnDUi.fDragging)
                vboxDnDMouseRelease(); /* DoDragDrop drops now. */
            else
                vboxDnDAbort();
            return 0;

        case WM_VBOXDND_GH_PENDING:
            vboxDnDGhOnPending();
            return 0;

        case WM_VBOXDND_GH_RESET:
            /* After the host's drop the user's button is up, but the guest never saw
               that (it went up outside the VM window), so the guest drag would go on
               holding the mouse: end it, on the proxy window (a copy). */
            if (wParam && g_DnDUi.fGhOver)
                mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
            vboxDnDGhReset();
            return 0;

        case WM_VBOXDND_CANCEL:
            VBoxTrayLogRel("DnD: the host cancelled the operation\n");
            vboxDnDAbort();
            vboxDnDGhReset();
            return 0;

        case WM_VBOXDND_RESTORED:
        {
            int rc = VBoxDnDHandshake(g_DnDUi.hDevice, g_DnDUi.idClient);
            if (RT_FAILURE(rc))
                VBoxTrayLogRel("DnD: the window thread's handshake after the restore failed, rc=%d\n", rc);
            return 0;
        }

        case WM_TIMER:
            if (wParam == VBOXDND_GH_TIMER_HIDE)
            {
                KillTimer(hwnd, VBOXDND_GH_TIMER_HIDE);
                if (!g_DnDUi.pszGhFormat)
                    vboxDnDGhReset(); /* No guest drag came. */
            }
            else if (wParam == VBOXDND_GH_TIMER_EXPIRE)
                vboxDnDGhReset();
            return 0;

        case WM_VBOXDND_QUIT:
            free(g_DnDUi.pPendingEnter);
            g_DnDUi.pPendingEnter = NULL;
            vboxDnDAbort();
            vboxDnDGhReset();
            if (g_DnDUi.pDropTarget)
            {
                RevokeDragDrop(hwnd);
                g_DnDUi.pDropTarget->lpVtbl->Release(g_DnDUi.pDropTarget);
                g_DnDUi.pDropTarget = NULL;
            }
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
}

/**
 * Proxy window thread: OLE needs a single threaded apartment with a message loop.
 */
static unsigned __stdcall vboxDnDUiThread(void *pvUser)
{
    VBOXDNDUISTART *pStart = (VBOXDNDUISTART *)pvUser;
    WNDCLASS WndCls;
    HRESULT hrOle;
    BOOL fOk = FALSE;
    MSG Msg;
    int rc;

    hrOle = OleInitialize(NULL);
    if (FAILED(hrOle))
        VBoxTrayLogRel("DnD: OleInitialize failed, hr=%#lx\n", hrOle);
    else
    {
        memset(&WndCls, 0, sizeof(WndCls));
        WndCls.lpfnWndProc   = vboxDnDWndProc;
        WndCls.hInstance     = GetModuleHandle(NULL);
        WndCls.hCursor       = LoadCursor(NULL, IDC_ARROW);
        WndCls.lpszClassName = VBOXDND_WND_CLASS;
        RegisterClass(&WndCls);
        g_DnDShared.hwnd = CreateWindowEx(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_TRANSPARENT, VBOXDND_WND_CLASS, "VBoxTray DnD",
                                   WS_POPUP, 0, 0, 1, 1, NULL, NULL, WndCls.hInstance, NULL);
        if (!g_DnDShared.hwnd)
            VBoxTrayLogRel("DnD: creating the proxy window failed, error %lu\n", GetLastError());
        else
        {
            g_DnDUi.hDevice = VbglR3Open();
            rc = g_DnDUi.hDevice != INVALID_HANDLE_VALUE ? VBoxDnDConnect(g_DnDUi.hDevice, &g_DnDUi.idClient) : VERR_NOT_FOUND;
            if (RT_SUCCESS(rc))
            {
                fOk = TRUE;

                /* Catches guest drags for the host; host to guest works without it. */
                g_DnDUi.pDropTarget = VBoxDnDDropTargetCreate();
                if (g_DnDUi.pDropTarget)
                {
                    HRESULT hr = RegisterDragDrop(g_DnDShared.hwnd, g_DnDUi.pDropTarget);
                    if (FAILED(hr))
                    {
                        VBoxTrayLogRel("DnD: RegisterDragDrop failed, hr=%#lx; no guest to host drag and drop\n", hr);
                        g_DnDUi.pDropTarget->lpVtbl->Release(g_DnDUi.pDropTarget);
                        g_DnDUi.pDropTarget = NULL;
                    }
                }
            }
            else
            {
                VBoxTrayLogRel("DnD: connecting the window thread failed, rc=%d\n", rc);
                DestroyWindow(g_DnDShared.hwnd);
            }
        }
    }
    pStart->fOk = fOk;
    SetEvent(pStart->hEvtReady); /* pStart is gone after this. */

    while (fOk && GetMessage(&Msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&Msg);
        DispatchMessage(&Msg);
    }

    g_DnDShared.hwnd = NULL;
    if (g_DnDUi.idClient)
    {
        VbglR3HgcmDisconnect(g_DnDUi.hDevice, g_DnDUi.idClient);
        g_DnDUi.idClient = 0;
    }
    VbglR3Close(g_DnDUi.hDevice);
    g_DnDUi.hDevice = INVALID_HANDLE_VALUE;
    UnregisterClass(VBOXDND_WND_CLASS, GetModuleHandle(NULL));
    if (SUCCEEDED(hrOle))
        OleUninitialize();
    return 0;
}


/*********************************************************************************************************************************
*   Worker thread                                                                                                                *
*********************************************************************************************************************************/

/**
 * Receives the data dropped on the guest and hands it to the data object.
 */
static int vboxDnDRecvData(HANDLE hDevice, UINT32 idClient)
{
    void *pvData = NULL;
    DWORD cbData = 0;
    int rc;

    rc = VBoxDnDRecvData(hDevice, idClient, &pvData, &cbData);
    if (RT_SUCCESS(rc))
    {
        EnterCriticalSection(&g_DnDShared.CritSect);
        if (g_DnDShared.pDataObj)
        {
            VBoxDnDDataObjectSetData(g_DnDShared.pDataObj, pvData, cbData);
            pvData = NULL;
        }
        else
            rc = VERR_NOT_FOUND; /* The drag was cancelled meanwhile. */
        LeaveCriticalSection(&g_DnDShared.CritSect);
    }
    free(pvData);
    return rc;
}

/**
 * The guest drag was dropped on the host: sends the data the proxy window
 * caught (OnGhDrop).
 *
 * @param   pszFormat   The format the host asked for.
 */
static int vboxDnDGhSend(HANDLE hDevice, UINT32 idClient, const char *pszFormat)
{
    char *pszData;
    BOOL fFiles;
    int rc;

    /* Normally the guest drop on the proxy window is long done. */
    WaitForSingleObject(g_DnDShared.hGhDropped, VBOXDND_GH_DROP_WAIT_MS);

    EnterCriticalSection(&g_DnDShared.CritSect);
    pszData = g_DnDShared.pszGhData;
    fFiles  = g_DnDShared.fGhFiles;
    g_DnDShared.pszGhData = NULL;
    LeaveCriticalSection(&g_DnDShared.CritSect);

    VBoxTrayLogRel("DnD: dropped on the host, which wants %s\n", pszFormat);
    rc = VBoxDnDGhSendData(hDevice, idClient, pszData, fFiles);
    free(pszData);
    return rc;
}

/**
 * Handles a host message.
 */
static int vboxDnDProcessHostMsg(HANDLE hDevice, UINT32 idClient, UINT32 uMsg)
{
    VBOXDNDENTER *pEnter = NULL;
    char szFormat[256];
    UINT32 x = 0;
    UINT32 y = 0;
    int rc;

    if (uMsg != HOST_DND_FN_HG_EVT_MOVE && uMsg != HOST_DND_FN_GH_REQ_PENDING) /* Diagnostics; these come many per second. */
        VBoxTrayLogRel("DnD: host message %lu\n", uMsg);

    switch (uMsg)
    {
        case HOST_DND_FN_HG_EVT_ENTER:
            rc = VBoxDnDRecvAction(hDevice, idClient, uMsg, &x, &y, &pEnter);
            if (RT_SUCCESS(rc) && !PostMessage(g_DnDShared.hwnd, WM_VBOXDND_ENTER, 0, (LPARAM)pEnter))
                free(pEnter);
            break;

        case HOST_DND_FN_HG_EVT_MOVE:
            rc = VBoxDnDRecvAction(hDevice, idClient, uMsg, &x, &y, NULL);
            if (RT_SUCCESS(rc))
                PostMessage(g_DnDShared.hwnd, WM_VBOXDND_MOVE, x, y);
            break;

        case HOST_DND_FN_HG_EVT_DROPPED:
            rc = VBoxDnDRecvAction(hDevice, idClient, uMsg, &x, &y, NULL);
            if (RT_SUCCESS(rc))
                PostMessage(g_DnDShared.hwnd, WM_VBOXDND_DROP, 0, 0);
            break;

        case HOST_DND_FN_HG_EVT_LEAVE:
        case HOST_DND_FN_CANCEL:
            rc = VBoxDnDRecvSimple(hDevice, idClient, uMsg);
            PostMessage(g_DnDShared.hwnd, uMsg == HOST_DND_FN_CANCEL ? WM_VBOXDND_CANCEL : WM_VBOXDND_LEAVE, 0, 0);
            /* The host waits 3 s for an answer to the leave (upstream sends none,
               so the host reports "Leaving VM window failed (VERR_TIMEOUT)"). */
            if (RT_SUCCESS(rc) && uMsg == HOST_DND_FN_HG_EVT_LEAVE)
                VBoxDnDSendAck(hDevice, idClient, VBOX_DND_ACTION_IGNORE);
            break;

        case HOST_DND_FN_HG_SND_DATA_HDR:
            rc = vboxDnDRecvData(hDevice, idClient);
            PostMessage(g_DnDShared.hwnd, rc == VERR_CANCELLED ? WM_VBOXDND_LEAVE : WM_VBOXDND_RECEIVED, RT_SUCCESS(rc), 0);
            if (rc == VERR_CANCELLED)
                rc = VINF_SUCCESS;
            break;

        case HOST_DND_FN_GH_REQ_PENDING:
            /* The window thread answers, once it knows. */
            rc = VBoxDnDRecvGhPending(hDevice, idClient);
            if (RT_SUCCESS(rc))
                PostMessage(g_DnDShared.hwnd, WM_VBOXDND_GH_PENDING, 0, 0);
            break;

        case HOST_DND_FN_GH_EVT_DROPPED:
            rc = VBoxDnDRecvGhDropped(hDevice, idClient, szFormat, sizeof(szFormat));
            if (RT_SUCCESS(rc))
                vboxDnDGhSend(hDevice, idClient, szFormat); /* Failures were reported to the host. */
            PostMessage(g_DnDShared.hwnd, WM_VBOXDND_GH_RESET, TRUE /* dropped on the host */, 0);
            break;

        default:
            /* Like upstream: not fetched, so the host keeps offering it until we give up. */
            VBoxTrayLogRel("DnD: unsupported host message %lu\n", uMsg);
            rc = VERR_NOT_SUPPORTED;
            break;
    }
    return rc;
}

/**
 * Prepares the service, before any thread starts.
 */
void VBoxDnDInit(void)
{
    InitializeCriticalSection(&g_DnDShared.CritSect);
    g_DnDShared.hGhDropped = CreateEvent(NULL, TRUE, FALSE, NULL);
}

/**
 * Wakes up the worker: disconnecting completes its pending
 * GUEST_DND_FN_GET_NEXT_HOST_MSG.
 */
void VBoxDnDStop(void)
{
    UINT32 idClient;
    HANDLE hDevice;

    EnterCriticalSection(&g_DnDShared.CritSect);
    idClient = g_DnDShared.idClient;
    hDevice  = g_DnDShared.hDevice;
    g_DnDShared.idClient = 0;
    LeaveCriticalSection(&g_DnDShared.CritSect);
    if (idClient)
        VbglR3HgcmDisconnect(hDevice, idClient);
}

/**
 * Drag and drop worker thread.
 *
 * @returns 0 on normal exit, 1 if the service could not be used.
 * @param   pvUser      Unused.
 */
unsigned __stdcall VBoxDnDThread(void *pvUser)
{
    VBOXDNDUISTART Start;
    HANDLE hDevice;
    HANDLE hUiThread = NULL;
    UINT32 idClient = 0;
    UINT64 idSession = 0;
    unsigned cErrors = 0;
    unsigned idThread;
    int rc;

    /* VBoxDnDStop disconnects while a call waits, hence overlapped. */
    hDevice = VbglR3OpenEx(TRUE);
    if (hDevice == INVALID_HANDLE_VALUE)
        return 1;
    rc = VBoxDnDConnect(hDevice, &idClient);
    if (RT_FAILURE(rc))
    {
        VBoxTrayLogRel("DnD: connecting to the service failed, rc=%d\n", rc);
        VbglR3Close(hDevice);
        return 1;
    }

    Start.fOk      = FALSE;
    Start.hEvtReady = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (Start.hEvtReady)
    {
        hUiThread = (HANDLE)_beginthreadex(NULL, 0, vboxDnDUiThread, &Start, 0, &idThread);
        if (hUiThread)
            WaitForSingleObject(Start.hEvtReady, INFINITE);
        CloseHandle(Start.hEvtReady);
    }
    if (!hUiThread || !Start.fOk)
    {
        if (hUiThread)
        {
            WaitForSingleObject(hUiThread, INFINITE);
            CloseHandle(hUiThread);
        }
        VbglR3HgcmDisconnect(hDevice, idClient);
        VbglR3Close(hDevice);
        return 1;
    }

    EnterCriticalSection(&g_DnDShared.CritSect);
    g_DnDShared.hDevice  = hDevice;
    g_DnDShared.idClient = idClient;
    LeaveCriticalSection(&g_DnDShared.CritSect);
    VBoxTrayLogRel("DnD: connected, client %lu\n", idClient);
    VbglR3QuerySessionId(hDevice, &idSession);

    while (!g_fVBoxTrayShutdown)
    {
        UINT32 uMsg = 0;
        UINT32 cParms = 0;
        UINT64 idSessionNow;

        rc = VBoxDnDGetNextMsg(hDevice, idClient, &uMsg, &cParms);
        if (RT_SUCCESS(rc))
        {
            /* After a restore from a saved state the service has forgotten the
               protocol version of both clients, like upstream's VbglR3DnDEventGetNext
               checks.  The window thread redoes its own handshake before it
               handles what we post next. */
            if (   RT_SUCCESS(VbglR3QuerySessionId(hDevice, &idSessionNow))
                && idSessionNow != idSession)
            {
                VBoxTrayLogRel("DnD: the VM was restored, redoing the protocol handshake\n");
                idSession = idSessionNow;
                rc = VBoxDnDHandshake(hDevice, idClient);
                if (RT_FAILURE(rc))
                    VBoxTrayLogRel("DnD: the handshake after the restore failed, rc=%d\n", rc);
                PostMessage(g_DnDShared.hwnd, WM_VBOXDND_RESTORED, 0, 0);
            }
            rc = vboxDnDProcessHostMsg(hDevice, idClient, uMsg);
        }
        else if (g_fVBoxTrayShutdown || rc == VERR_INVALID_HANDLE)
            break; /* Disconnected by VBoxDnDStop. */
        else if (rc == VERR_CANCELLED)
        {
            /* The host cancelled the current operation (HOST_DND_FN_CANCEL wakes waiters up this way). */
            PostMessage(g_DnDShared.hwnd, WM_VBOXDND_CANCEL, 0, 0);
            rc = VINF_SUCCESS;
        }

        if (RT_SUCCESS(rc))
            cErrors = 0;
        else
        {
            PostMessage(g_DnDShared.hwnd, WM_VBOXDND_LEAVE, 0, 0); /* Don't leave the proxy window blocking the screen. */
            if (++cErrors > VBOXDND_MAX_ERRORS)
            {
                VBoxTrayLogRel("DnD: too many failures (last rc=%d), giving up\n", rc);
                break;
            }
            Sleep(1000);
        }
    }

    PostMessage(g_DnDShared.hwnd, WM_VBOXDND_QUIT, 0, 0);
    WaitForSingleObject(hUiThread, 3000);
    CloseHandle(hUiThread);

    EnterCriticalSection(&g_DnDShared.CritSect);
    idClient = g_DnDShared.idClient;
    g_DnDShared.idClient = 0;
    g_DnDShared.hDevice  = INVALID_HANDLE_VALUE;
    LeaveCriticalSection(&g_DnDShared.CritSect);
    if (idClient)
        VbglR3HgcmDisconnect(hDevice, idClient);
    VbglR3Close(hDevice);
    return 0;
}
