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
 */

#include "DnD.h"
#include <shellapi.h>
#include <shlobj.h>
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
/** Timeout for fetching queued host messages and for answers (ms). */
#define VBOXDND_CALL_TIMEOUT_MS     30000
/** Largest meta data (text or URI list) accepted. */
#define VBOXDND_MAX_META_SIZE       (16U * 1024 * 1024)
/** Consecutive failures after which the worker gives up, like upstream. */
#define VBOXDND_MAX_ERRORS          32
/** Base directory of the dropped files, under %TEMP% (as upstream). */
#define VBOXDND_DROP_DIR_NAME       "VirtualBox Dropped Files"


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/

/** WM_VBOXDND_ENTER data. */
typedef struct VBOXDNDENTER
{
    UINT32 x;
    UINT32 y;
    UINT32 fAllActions;
    /** Formats offered by the host, VBOX_DND_FORMATS_SEPARATOR separated. */
    char   szFormats[1];
} VBOXDNDENTER;

/** A format the guest accepts and how it is handed to the drop target. */
typedef struct VBOXDNDFORMAT
{
    const char *pszMime;
    CLIPFORMAT  cf;
} VBOXDNDFORMAT;

/** A directory or file sent to the host. */
typedef struct VBOXDNDGHOBJ
{
    /** The guest path. */
    char   *pszAbs;
    /** The path in the transfer (UTF-8, '/' separated). */
    char   *pszRel;
    BOOL    fDir;
    UINT64  cb;
} VBOXDNDGHOBJ;

/** The objects sent to the host, directories before their contents. */
typedef struct VBOXDNDGHLIST
{
    VBOXDNDGHOBJ *paObjs;
    DWORD         cObjs;
    DWORD         cAlloc;
    /** Sum of the file sizes. */
    UINT64        cbFiles;
} VBOXDNDGHLIST;

/** Proxy window thread start up. */
typedef struct VBOXDNDUISTART
{
    HANDLE hEvtReady;
    BOOL   fOk;
} VBOXDNDUISTART;


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

/** Protects g_hDnDDevice, g_idDnDClient and g_pDnDDataObj. */
static CRITICAL_SECTION g_DnDCritSect;
/** The worker's handle (overlapped, so VBoxDnDStop can disconnect). */
static HANDLE g_hDnDDevice = INVALID_HANDLE_VALUE;
/** The worker's HGCM client ID, 0 if not connected. */
static UINT32 g_idDnDClient = 0;
/** The proxy window. */
static HWND g_hwndDnD = NULL;
/** The data object of the current drag (a reference of its own). */
static IDataObject *g_pDnDDataObj = NULL;

/* Proxy window thread only: */
/** The window thread's handle and HGCM client (for the answers to the host). */
static HANDLE g_hDnDUiDevice = INVALID_HANDLE_VALUE;
static UINT32 g_idDnDUiClient = 0;
/** The drop source of the current drag. */
static IDropSource *g_pDnDDropSource = NULL;
/** The format chosen at the enter, requested at the drop. */
static const VBOXDNDFORMAT *g_pDnDFormat = NULL;
/** Whether a host drag is in the guest (the proxy window is up). */
static BOOL g_fDnDActive = FALSE;
/** Whether we pressed the left button. */
static BOOL g_fDnDButtonDown = FALSE;
/** Whether DoDragDrop is running. */
static BOOL g_fDnDDragging = FALSE;
/** An enter that came while DoDragDrop was still running (malloc'ed). */
static VBOXDNDENTER *g_pDnDPendingEnter = NULL;
/** The drop target catching guest drags for the host, NULL if not registered. */
static IDropTarget *g_pDnDDropTarget = NULL;
/** Whether the proxy window is up to catch a guest drag. */
static BOOL g_fDnDGhActive = FALSE;
/** Whether a guest drag is over the proxy window (DragEnter came, the drop not yet). */
static BOOL g_fDnDGhOver = FALSE;

/* Guest to host, shared with the worker (g_DnDCritSect): */
/** The format of the caught guest drag, NULL if none. */
static const char *g_pszDnDGhFormat = NULL;
/** The data of the guest drag dropped on the proxy window (malloc'ed):
 *  a CF_HDROP style path list if g_fDnDGhFiles, else UTF-8 text. */
static char *g_pszDnDGhData = NULL;
static BOOL g_fDnDGhFiles = FALSE;
/** Signalled when g_pszDnDGhData is set. */
static HANDLE g_hDnDGhDropped = NULL;


/*********************************************************************************************************************************
*   HGCM calls                                                                                                                   *
*********************************************************************************************************************************/

/**
 * Connects to the service and does the protocol handshake (VbglR3DnDConnect).
 */
static int vboxDnDConnect(HANDLE hDevice, UINT32 *pidClient)
{
    VBGLR3HGCMMSG Msg;
    int rc;

    rc = VbglR3HgcmConnect(hDevice, VBOX_DND_SERVICE_NAME, pidClient);
    if (RT_FAILURE(rc))
        return rc;

    if (!VbglR3HgcmMsgInit(&Msg, *pidClient, GUEST_DND_FN_CONNECT, 3, VBOXDND_CALL_TIMEOUT_MS, 0))
        rc = VERR_NO_MEMORY;
    else
    {
        VbglR3HgcmMsgSetU32(&Msg, 0, 0);                           /* uContext */
        VbglR3HgcmMsgSetU32(&Msg, 1, VBOX_DND_PROTOCOL_VERSION);   /* uProtocol */
        VbglR3HgcmMsgSetU32(&Msg, 2, 0);                           /* uFlags */
        rc = VbglR3HgcmMsgCall(hDevice, &Msg);
        VbglR3HgcmMsgFree(&Msg);
    }

    /* Reporting the features fails on older hosts, which is fine. */
    if (RT_SUCCESS(rc) && VbglR3HgcmMsgInit(&Msg, *pidClient, GUEST_DND_FN_REPORT_FEATURES, 2, VBOXDND_CALL_TIMEOUT_MS, 0))
    {
        VbglR3HgcmMsgSetU64(&Msg, 0, VBOX_DND_GF_NONE);
        VbglR3HgcmMsgSetU64(&Msg, 1, VBOX_DND_GF_1_MUST_BE_ONE);
        VbglR3HgcmMsgCall(hDevice, &Msg);
        VbglR3HgcmMsgFree(&Msg);
    }

    if (RT_FAILURE(rc))
    {
        VbglR3HgcmDisconnect(hDevice, *pidClient);
        *pidClient = 0;
    }
    return rc;
}

/**
 * Waits for the next host message.
 *
 * @returns VBox status code; VERR_CANCELLED if the host cancelled the
 *          current operation (HOST_DND_FN_CANCEL).
 */
static int vboxDnDGetNextMsg(HANDLE hDevice, UINT32 idClient, UINT32 *puMsg, UINT32 *pcParms)
{
    VBGLR3HGCMMSG Msg;
    int rc;

    do
    {
        if (!VbglR3HgcmMsgInit(&Msg, idClient, GUEST_DND_FN_GET_NEXT_HOST_MSG, 3, VBOXGUEST98_WAIT_INFINITE, 0))
            return VERR_NO_MEMORY;
        VbglR3HgcmMsgSetU32(&Msg, 0, 0);   /* uMsg */
        VbglR3HgcmMsgSetU32(&Msg, 1, 0);   /* cParms */
        VbglR3HgcmMsgSetU32(&Msg, 2, 1);   /* fBlock */
        rc = VbglR3HgcmMsgCall(hDevice, &Msg);
        if (RT_SUCCESS(rc))
        {
            *puMsg   = VbglR3HgcmMsgGetU32(&Msg, 0);
            *pcParms = VbglR3HgcmMsgGetU32(&Msg, 1);
        }
        VbglR3HgcmMsgFree(&Msg);
    } while (rc == VERR_INTERRUPTED && !g_fVBoxTrayShutdown);
    return rc;
}

/**
 * Fetches a message that only has the context ID (leave, cancel).
 */
static int vboxDnDRecvSimple(HANDLE hDevice, UINT32 idClient, UINT32 uMsg)
{
    VBGLR3HGCMMSG Msg;
    int rc;

    if (!VbglR3HgcmMsgInit(&Msg, idClient, uMsg, 1, VBOXDND_CALL_TIMEOUT_MS, 0))
        return VERR_NO_MEMORY;
    VbglR3HgcmMsgSetU32(&Msg, 0, 0);
    rc = VbglR3HgcmMsgCall(hDevice, &Msg);
    VbglR3HgcmMsgFree(&Msg);
    return rc;
}

/**
 * Fetches an "action" message: enter, move or dropped (vbglR3DnDHGRecvAction).
 *
 * @param   ppEnter     Where to return the enter data (malloc'ed), NULL if not wanted.
 */
static int vboxDnDRecvAction(HANDLE hDevice, UINT32 idClient, UINT32 uMsg, UINT32 *px, UINT32 *py,
                             VBOXDNDENTER **ppEnter)
{
    VBGLR3HGCMMSG Msg;
    BYTE *pbFormats;
    UINT32 cbFormats;
    int rc;

    if (!VbglR3HgcmMsgInit(&Msg, idClient, uMsg, 8, VBOXDND_CALL_TIMEOUT_MS, VBOX_DND_CHUNK_SIZE))
        return VERR_NO_MEMORY;
    VbglR3HgcmMsgSetU32(&Msg, 0, 0);   /* uContext */
    VbglR3HgcmMsgSetU32(&Msg, 1, 0);   /* uScreenId */
    VbglR3HgcmMsgSetU32(&Msg, 2, 0);   /* uX */
    VbglR3HgcmMsgSetU32(&Msg, 3, 0);   /* uY */
    VbglR3HgcmMsgSetU32(&Msg, 4, 0);   /* uDefAction */
    VbglR3HgcmMsgSetU32(&Msg, 5, 0);   /* uAllActions */
    pbFormats = VbglR3HgcmMsgSetBuf(&Msg, 6, VBOXGUEST98_HGCM_PARM_BUFFER_OUT, VBOX_DND_CHUNK_SIZE, NULL);
    VbglR3HgcmMsgSetU32(&Msg, 7, 0);   /* cbFormats */

    rc = VbglR3HgcmMsgCall(hDevice, &Msg);
    if (RT_SUCCESS(rc))
    {
        *px = VbglR3HgcmMsgGetU32(&Msg, 2);
        *py = VbglR3HgcmMsgGetU32(&Msg, 3);
        if (ppEnter)
        {
            cbFormats = VbglR3HgcmMsgGetBufSize(&Msg, 6);
            if (cbFormats > VBOX_DND_CHUNK_SIZE)
                cbFormats = VBOX_DND_CHUNK_SIZE;
            *ppEnter = (VBOXDNDENTER *)malloc(sizeof(VBOXDNDENTER) + cbFormats);
            if (*ppEnter)
            {
                (*ppEnter)->x           = *px;
                (*ppEnter)->y           = *py;
                (*ppEnter)->fAllActions = VbglR3HgcmMsgGetU32(&Msg, 5);
                memcpy((*ppEnter)->szFormats, pbFormats, cbFormats);
                (*ppEnter)->szFormats[cbFormats] = '\0';
            }
            else
                rc = VERR_NO_MEMORY;
        }
    }
    VbglR3HgcmMsgFree(&Msg);
    return rc;
}

/**
 * Tells the host the action the guest would do at the current position.
 */
static int vboxDnDSendAck(HANDLE hDevice, UINT32 idClient, UINT32 uAction)
{
    VBGLR3HGCMMSG Msg;
    int rc;

    if (!VbglR3HgcmMsgInit(&Msg, idClient, GUEST_DND_FN_HG_ACK_OP, 2, VBOXDND_CALL_TIMEOUT_MS, 0))
        return VERR_NO_MEMORY;
    VbglR3HgcmMsgSetU32(&Msg, 0, 0);
    VbglR3HgcmMsgSetU32(&Msg, 1, uAction);
    rc = VbglR3HgcmMsgCall(hDevice, &Msg);
    VbglR3HgcmMsgFree(&Msg);
    return rc;
}

/**
 * Asks the host for the dropped data in a format.
 */
static int vboxDnDSendReqData(HANDLE hDevice, UINT32 idClient, const char *pszFormat)
{
    DWORD cbFormat = lstrlen(pszFormat) + 1;
    VBGLR3HGCMMSG Msg;
    int rc;

    if (!VbglR3HgcmMsgInit(&Msg, idClient, GUEST_DND_FN_HG_REQ_DATA, 3, VBOXDND_CALL_TIMEOUT_MS, cbFormat))
        return VERR_NO_MEMORY;
    VbglR3HgcmMsgSetU32(&Msg, 0, 0);
    VbglR3HgcmMsgSetBuf(&Msg, 1, VBOXGUEST98_HGCM_PARM_BUFFER_IN, cbFormat, pszFormat);
    VbglR3HgcmMsgSetU32(&Msg, 2, cbFormat);
    rc = VbglR3HgcmMsgCall(hDevice, &Msg);
    VbglR3HgcmMsgFree(&Msg);
    return rc;
}

/**
 * Reports the transfer status to the host.
 */
static int vboxDnDSendProgress(HANDLE hDevice, UINT32 idClient, UINT32 uStatus, UINT32 uPercent, int rcErr)
{
    VBGLR3HGCMMSG Msg;
    int rc;

    if (!VbglR3HgcmMsgInit(&Msg, idClient, GUEST_DND_FN_HG_EVT_PROGRESS, 4, VBOXDND_CALL_TIMEOUT_MS, 0))
        return VERR_NO_MEMORY;
    VbglR3HgcmMsgSetU32(&Msg, 0, 0);
    VbglR3HgcmMsgSetU32(&Msg, 1, uStatus);
    VbglR3HgcmMsgSetU32(&Msg, 2, uPercent);
    VbglR3HgcmMsgSetU32(&Msg, 3, (UINT32)rcErr);
    rc = VbglR3HgcmMsgCall(hDevice, &Msg);
    VbglR3HgcmMsgFree(&Msg);
    return rc;
}

/**
 * Fetches the host's "is a guest drag pending" query (uContext, uScreenId).
 */
static int vboxDnDRecvGhPending(HANDLE hDevice, UINT32 idClient)
{
    VBGLR3HGCMMSG Msg;
    int rc;

    if (!VbglR3HgcmMsgInit(&Msg, idClient, HOST_DND_FN_GH_REQ_PENDING, 2, VBOXDND_CALL_TIMEOUT_MS, 0))
        return VERR_NO_MEMORY;
    VbglR3HgcmMsgSetU32(&Msg, 0, 0);
    VbglR3HgcmMsgSetU32(&Msg, 1, 0);
    rc = VbglR3HgcmMsgCall(hDevice, &Msg);
    VbglR3HgcmMsgFree(&Msg);
    return rc;
}

/**
 * Answers the "is a guest drag pending" query.
 *
 * @param   pszFormats  The formats of the guest drag, "" (with
 *                      VBOX_DND_ACTION_IGNORE) if there is none.
 */
static int vboxDnDSendAckPending(HANDLE hDevice, UINT32 idClient, UINT32 uAction, const char *pszFormats)
{
    DWORD cbFormats = lstrlen(pszFormats) + 1;
    VBGLR3HGCMMSG Msg;
    int rc;

    if (!VbglR3HgcmMsgInit(&Msg, idClient, GUEST_DND_FN_GH_ACK_PENDING, 5, VBOXDND_CALL_TIMEOUT_MS, cbFormats))
        return VERR_NO_MEMORY;
    VbglR3HgcmMsgSetU32(&Msg, 0, 0);                           /* uContext */
    VbglR3HgcmMsgSetU32(&Msg, 1, uAction);                     /* uDefAction */
    VbglR3HgcmMsgSetU32(&Msg, 2, uAction);                     /* uAllActions */
    VbglR3HgcmMsgSetBuf(&Msg, 3, VBOXGUEST98_HGCM_PARM_BUFFER_IN, cbFormats, pszFormats);
    VbglR3HgcmMsgSetU32(&Msg, 4, cbFormats);                   /* cbFormats */
    rc = VbglR3HgcmMsgCall(hDevice, &Msg);
    VbglR3HgcmMsgFree(&Msg);
    return rc;
}

/**
 * Fetches HOST_DND_FN_GH_EVT_DROPPED: the guest drag was dropped on the host.
 *
 * @param   pszFormat   Where to return the format the host wants.
 * @param   cbFormat    Size of pszFormat.
 */
static int vboxDnDRecvGhDropped(HANDLE hDevice, UINT32 idClient, char *pszFormat, DWORD cbFormat)
{
    VBGLR3HGCMMSG Msg;
    BYTE *pb;
    int rc;

    if (!VbglR3HgcmMsgInit(&Msg, idClient, HOST_DND_FN_GH_EVT_DROPPED, 4, VBOXDND_CALL_TIMEOUT_MS, VBOX_DND_CHUNK_SIZE))
        return VERR_NO_MEMORY;
    VbglR3HgcmMsgSetU32(&Msg, 0, 0);                                                               /* uContext */
    pb = VbglR3HgcmMsgSetBuf(&Msg, 1, VBOXGUEST98_HGCM_PARM_BUFFER_OUT, VBOX_DND_CHUNK_SIZE, NULL);    /* pvFormat */
    VbglR3HgcmMsgSetU32(&Msg, 2, 0);                                                               /* cbFormat */
    VbglR3HgcmMsgSetU32(&Msg, 3, 0);                                                               /* uAction */
    rc = VbglR3HgcmMsgCall(hDevice, &Msg);
    if (RT_SUCCESS(rc))
        lstrcpyn(pszFormat, (const char *)pb, cbFormat < VBOX_DND_CHUNK_SIZE ? cbFormat : VBOX_DND_CHUNK_SIZE);
    VbglR3HgcmMsgFree(&Msg);
    return rc;
}

/**
 * Tells the host the guest data can't be sent, so it doesn't wait for it.
 */
static int vboxDnDSendError(HANDLE hDevice, UINT32 idClient, int rcErr)
{
    VBGLR3HGCMMSG Msg;
    int rc;

    if (!VbglR3HgcmMsgInit(&Msg, idClient, GUEST_DND_FN_EVT_ERROR, 2, VBOXDND_CALL_TIMEOUT_MS, 0))
        return VERR_NO_MEMORY;
    VbglR3HgcmMsgSetU32(&Msg, 0, 0);
    VbglR3HgcmMsgSetU32(&Msg, 1, (UINT32)rcErr);
    rc = VbglR3HgcmMsgCall(hDevice, &Msg);
    VbglR3HgcmMsgFree(&Msg);
    return rc;
}

/**
 * Sends the data header and the meta data of a guest to host transfer
 * (vbglR3DnDGHSendDataInternal).
 *
 * @param   pszMetaFmt  "text/uri-list" for files, NULL for raw data.
 * @param   cbTotal     Meta data plus file bytes.
 */
static int vboxDnDGhSendMeta(HANDLE hDevice, UINT32 idClient, const char *pszMetaFmt, const void *pvMeta, DWORD cbMeta,
                             UINT64 cbTotal, UINT64 cObjects)
{
    DWORD cbMetaFmt = pszMetaFmt ? lstrlen(pszMetaFmt) + 1 : 0;
    DWORD cbSent = 0;
    VBGLR3HGCMMSG Msg;
    int rc;

    if (!VbglR3HgcmMsgInit(&Msg, idClient, GUEST_DND_FN_GH_SND_DATA_HDR, 12, VBOXDND_CALL_TIMEOUT_MS, cbMetaFmt))
        return VERR_NO_MEMORY;
    VbglR3HgcmMsgSetU32(&Msg, 0, 0);                                                           /* uContext */
    VbglR3HgcmMsgSetU32(&Msg, 1, 0);                                                           /* uFlags */
    VbglR3HgcmMsgSetU32(&Msg, 2, 0);                                                           /* uScreenId */
    VbglR3HgcmMsgSetU64(&Msg, 3, cbTotal);                                                     /* cbTotal */
    VbglR3HgcmMsgSetU32(&Msg, 4, cbMeta);                                                      /* cbMeta */
    VbglR3HgcmMsgSetBuf(&Msg, 5, VBOXGUEST98_HGCM_PARM_BUFFER_IN, cbMetaFmt, pszMetaFmt);      /* pvMetaFmt */
    VbglR3HgcmMsgSetU32(&Msg, 6, cbMetaFmt);                                                   /* cbMetaFmt */
    VbglR3HgcmMsgSetU64(&Msg, 7, cObjects);                                                    /* cObjects */
    VbglR3HgcmMsgSetU32(&Msg, 8, 0);                                                           /* enmCompression */
    VbglR3HgcmMsgSetU32(&Msg, 9, 0);                                                           /* enmChecksumType */
    VbglR3HgcmMsgSetBuf(&Msg, 10, VBOXGUEST98_HGCM_PARM_BUFFER_IN, 0, NULL);                   /* pvChecksum */
    VbglR3HgcmMsgSetU32(&Msg, 11, 0);                                                          /* cbChecksum */
    rc = VbglR3HgcmMsgCall(hDevice, &Msg);
    VbglR3HgcmMsgFree(&Msg);

    while (RT_SUCCESS(rc) && cbSent < cbMeta)
    {
        DWORD cbChunk = cbMeta - cbSent < VBOX_DND_CHUNK_SIZE ? cbMeta - cbSent : VBOX_DND_CHUNK_SIZE;
        if (!VbglR3HgcmMsgInit(&Msg, idClient, GUEST_DND_FN_GH_SND_DATA, 5, VBOXDND_CALL_TIMEOUT_MS, cbChunk))
            return VERR_NO_MEMORY;
        VbglR3HgcmMsgSetU32(&Msg, 0, 0);                                                               /* uContext */
        VbglR3HgcmMsgSetBuf(&Msg, 1, VBOXGUEST98_HGCM_PARM_BUFFER_IN, cbChunk, (const BYTE *)pvMeta + cbSent); /* pvData */
        VbglR3HgcmMsgSetU32(&Msg, 2, cbChunk);                                                         /* cbData */
        VbglR3HgcmMsgSetBuf(&Msg, 3, VBOXGUEST98_HGCM_PARM_BUFFER_IN, 0, NULL);                        /* pvChecksum */
        VbglR3HgcmMsgSetU32(&Msg, 4, 0);                                                               /* cbChecksum */
        rc = VbglR3HgcmMsgCall(hDevice, &Msg);
        VbglR3HgcmMsgFree(&Msg);
        cbSent += cbChunk;
    }
    return rc;
}

/**
 * Sends a directory of a guest to host transfer.
 *
 * @param   pszRel      Its path in the transfer (UTF-8, '/' separated).
 */
static int vboxDnDGhSendDir(HANDLE hDevice, UINT32 idClient, const char *pszRel)
{
    DWORD cbRel = lstrlen(pszRel) + 1;
    VBGLR3HGCMMSG Msg;
    int rc;

    if (!VbglR3HgcmMsgInit(&Msg, idClient, GUEST_DND_FN_GH_SND_DIR, 4, VBOXDND_CALL_TIMEOUT_MS, cbRel))
        return VERR_NO_MEMORY;
    VbglR3HgcmMsgSetU32(&Msg, 0, 0);                                                   /* uContext */
    VbglR3HgcmMsgSetBuf(&Msg, 1, VBOXGUEST98_HGCM_PARM_BUFFER_IN, cbRel, pszRel);      /* pvName */
    VbglR3HgcmMsgSetU32(&Msg, 2, cbRel);                                               /* cbName */
    VbglR3HgcmMsgSetU32(&Msg, 3, VBOX_DND_MODE_DIR);                                   /* fMode */
    rc = VbglR3HgcmMsgCall(hDevice, &Msg);
    VbglR3HgcmMsgFree(&Msg);
    return rc;
}

/**
 * Sends a file of a guest to host transfer: the header, then the data in chunks.
 *
 * @param   pszAbs      The guest file.
 * @param   pszRel      Its path in the transfer (UTF-8, '/' separated).
 * @param   cbFile      Its size, as announced in the data header.
 */
static int vboxDnDGhSendFile(HANDLE hDevice, UINT32 idClient, const char *pszAbs, const char *pszRel, UINT64 cbFile)
{
    DWORD cbRel = lstrlen(pszRel) + 1;
    UINT64 cbLeft = cbFile;
    VBGLR3HGCMMSG Msg;
    HANDLE hFile;
    int rc;

    hFile = CreateFile(pszAbs, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return VERR_OPEN_FAILED;

    if (!VbglR3HgcmMsgInit(&Msg, idClient, GUEST_DND_FN_GH_SND_FILE_HDR, 6, VBOXDND_CALL_TIMEOUT_MS, cbRel))
    {
        CloseHandle(hFile);
        return VERR_NO_MEMORY;
    }
    VbglR3HgcmMsgSetU32(&Msg, 0, 0);                                                   /* uContext */
    VbglR3HgcmMsgSetBuf(&Msg, 1, VBOXGUEST98_HGCM_PARM_BUFFER_IN, cbRel, pszRel);      /* pvName */
    VbglR3HgcmMsgSetU32(&Msg, 2, cbRel);                                               /* cbName */
    VbglR3HgcmMsgSetU32(&Msg, 3, 0);                                                   /* uFlags */
    VbglR3HgcmMsgSetU32(&Msg, 4, VBOX_DND_MODE_FILE);                                  /* fMode */
    VbglR3HgcmMsgSetU64(&Msg, 5, cbFile);                                              /* cbTotal */
    rc = VbglR3HgcmMsgCall(hDevice, &Msg);
    VbglR3HgcmMsgFree(&Msg);

    while (RT_SUCCESS(rc) && cbLeft)
    {
        DWORD cbChunk = cbLeft < VBOX_DND_CHUNK_SIZE ? (DWORD)cbLeft : VBOX_DND_CHUNK_SIZE;
        DWORD cbRead = 0;
        BYTE *pb;

        if (!VbglR3HgcmMsgInit(&Msg, idClient, GUEST_DND_FN_GH_SND_FILE_DATA, 5, VBOXDND_CALL_TIMEOUT_MS, cbChunk))
        {
            rc = VERR_NO_MEMORY;
            break;
        }
        VbglR3HgcmMsgSetU32(&Msg, 0, 0);                                                       /* uContext */
        pb = VbglR3HgcmMsgSetBuf(&Msg, 1, VBOXGUEST98_HGCM_PARM_BUFFER_IN, cbChunk, NULL);     /* pvData */
        VbglR3HgcmMsgSetU32(&Msg, 2, cbChunk);                                                 /* cbData */
        VbglR3HgcmMsgSetBuf(&Msg, 3, VBOXGUEST98_HGCM_PARM_BUFFER_IN, 0, NULL);                /* pvChecksum */
        VbglR3HgcmMsgSetU32(&Msg, 4, 0);                                                       /* cbChecksum */
        if (!ReadFile(hFile, pb, cbChunk, &cbRead, NULL) || cbRead != cbChunk)
            rc = VERR_READ_ERROR; /* The file changed size since we looked: the host counts on the announced size. */
        else
            rc = VbglR3HgcmMsgCall(hDevice, &Msg);
        VbglR3HgcmMsgFree(&Msg);
        cbLeft -= cbChunk;
    }

    CloseHandle(hFile);
    return rc;
}


/*********************************************************************************************************************************
*   Text and names                                                                                                               *
*********************************************************************************************************************************/

/**
 * Converts UTF-8 to UTF-16 (invalid sequences become U+FFFD; characters
 * outside the BMP become surrogate pairs).
 *
 * @returns Zero terminated text (malloc'ed), NULL if out of memory.
 */
static WCHAR *vboxDnDUtf8ToUtf16(const char *pch, DWORD cch)
{
    const BYTE *pb = (const BYTE *)pch;
    WCHAR *pwszRet = (WCHAR *)malloc((cch + 1) * sizeof(WCHAR));
    DWORD off = 0;
    DWORD cwc = 0;

    if (!pwszRet)
        return NULL;
    while (off < cch)
    {
        BYTE b = pb[off++];
        DWORD uc;
        DWORD cTrail;
        DWORD ucMin;

        if (b < 0x80)
        {
            pwszRet[cwc++] = b;
            continue;
        }
        if ((b & 0xe0) == 0xc0)
        {
            uc = b & 0x1f;
            cTrail = 1;
            ucMin = 0x80;
        }
        else if ((b & 0xf0) == 0xe0)
        {
            uc = b & 0x0f;
            cTrail = 2;
            ucMin = 0x800;
        }
        else if ((b & 0xf8) == 0xf0)
        {
            uc = b & 0x07;
            cTrail = 3;
            ucMin = 0x10000;
        }
        else
        {
            pwszRet[cwc++] = 0xfffd;
            continue;
        }
        while (cTrail && off < cch && (pb[off] & 0xc0) == 0x80)
        {
            uc = (uc << 6) | (pb[off++] & 0x3f);
            cTrail--;
        }
        if (cTrail || uc < ucMin || uc > 0x10ffff || (uc >= 0xd800 && uc <= 0xdfff))
            pwszRet[cwc++] = 0xfffd;
        else if (uc >= 0x10000)
        {
            /* Room: a 4 byte sequence makes 2 UTF-16 units. */
            uc -= 0x10000;
            pwszRet[cwc++] = (WCHAR)(0xd800 + (uc >> 10));
            pwszRet[cwc++] = (WCHAR)(0xdc00 + (uc & 0x3ff));
        }
        else
            pwszRet[cwc++] = (WCHAR)uc;
    }
    pwszRet[cwc] = 0;
    return pwszRet;
}

/**
 * Converts UTF-8 to the ANSI code page.
 *
 * @returns Zero terminated text (malloc'ed), NULL if out of memory.
 * @param   chDefault   Replaces characters the code page doesn't have.
 */
static char *vboxDnDUtf8ToAnsi(const char *pch, DWORD cch, char chDefault)
{
    char szDefault[2];
    WCHAR *pwsz;
    char *pszRet = NULL;
    int cb;

    szDefault[0] = chDefault;
    szDefault[1] = '\0';
    pwsz = vboxDnDUtf8ToUtf16(pch, cch);
    if (!pwsz)
        return NULL;
    cb = WideCharToMultiByte(CP_ACP, 0, pwsz, -1, NULL, 0, szDefault, NULL);
    if (cb > 0)
    {
        pszRet = (char *)malloc(cb);
        if (pszRet && !WideCharToMultiByte(CP_ACP, 0, pwsz, -1, pszRet, cb, szDefault, NULL))
        {
            free(pszRet);
            pszRet = NULL;
        }
    }
    free(pwsz);
    return pszRet;
}

/**
 * Turns one UTF-8 path component from the host into a safe Windows 98 name.
 *
 * Characters Windows doesn't allow in names become '_', so do trailing dots
 * and spaces (Windows would drop them, and CF_HDROP must name the files we
 * wrote).  "." and ".." are refused, so nothing gets out of the drop directory.
 *
 * @returns VINF_SUCCESS, VERR_INVALID_NAME or VERR_NO_MEMORY.
 * @param   pszDst  Where to append the name.
 * @param   cbDst   Size of pszDst.
 */
static int vboxDnDAppendName(char *pszDst, DWORD cbDst, const char *pchName, DWORD cchName)
{
    DWORD offDst = lstrlen(pszDst);
    char *pszName;
    char *psz;
    DWORD cch;

    if (!cchName)
        return VERR_INVALID_NAME;
    pszName = vboxDnDUtf8ToAnsi(pchName, cchName, '_');
    if (!pszName)
        return VERR_NO_MEMORY;
    if (!lstrcmp(pszName, ".") || !lstrcmp(pszName, ".."))
    {
        free(pszName);
        return VERR_INVALID_NAME;
    }

    for (psz = pszName; *psz; psz++)
    {
        if (IsDBCSLeadByte((BYTE)*psz) && psz[1])
        {
            psz++; /* The trail byte may look like a backslash. */
            continue;
        }
        if ((BYTE)*psz < 0x20 || strchr("\\/:*?\"<>|", *psz))
            *psz = '_';
    }
    cch = lstrlen(pszName);
    while (cch && (pszName[cch - 1] == '.' || pszName[cch - 1] == ' '))
        pszName[--cch] = '_';

    if (offDst + 1 + cch + 1 > cbDst)
    {
        free(pszName);
        return VERR_FILENAME_TOO_LONG;
    }
    if (offDst && pszDst[offDst - 1] != '\\')
        pszDst[offDst++] = '\\';
    memcpy(&pszDst[offDst], pszName, cch + 1);
    free(pszName);
    return VINF_SUCCESS;
}

/**
 * Builds the guest path of an object of the transfer.
 *
 * @param   pszDropDir  The drop directory.
 * @param   pchRel      The host's relative path (UTF-8, '/' separated).
 * @param   cchRel      Its length.
 * @param   pszPath     Where to return the path (MAX_PATH bytes).
 */
static int vboxDnDBuildPath(const char *pszDropDir, const char *pchRel, DWORD cchRel, char *pszPath)
{
    DWORD off = 0;
    int rc = VERR_INVALID_NAME;

    lstrcpyn(pszPath, pszDropDir, MAX_PATH);
    while (off < cchRel)
    {
        DWORD offEnd = off;
        while (offEnd < cchRel && pchRel[offEnd] != '/' && pchRel[offEnd] != '\\')
            offEnd++;
        if (offEnd > off)
        {
            rc = vboxDnDAppendName(pszPath, MAX_PATH, &pchRel[off], offEnd - off);
            if (RT_FAILURE(rc))
                return rc;
        }
        off = offEnd + 1;
    }
    return rc; /* VERR_INVALID_NAME if there was no component at all. */
}

/**
 * Returns the length of a string in a buffer that may lack the terminator.
 */
static DWORD vboxDnDStrNLen(const BYTE *pb, DWORD cb)
{
    DWORD cch = 0;
    while (cch < cb && pb[cch])
        cch++;
    return cch;
}

/**
 * Decodes the %XX escapes of a URI in place.
 */
static void vboxDnDUriDecode(char *psz)
{
    char *pszDst = psz;
    while (*psz)
    {
        if (   psz[0] == '%'
            && strchr("0123456789abcdefABCDEF", psz[1]) && psz[1]
            && strchr("0123456789abcdefABCDEF", psz[2]) && psz[2])
        {
            char szHex[3];
            szHex[0] = psz[1];
            szHex[1] = psz[2];
            szHex[2] = '\0';
            *pszDst++ = (char)strtoul(szHex, NULL, 16);
            psz += 3;
        }
        else
            *pszDst++ = *psz++;
    }
    *pszDst = '\0';
}

/**
 * Builds the CF_HDROP data (DROPFILES and the paths) from the host's URI
 * list: one entry per transfer root, in the drop directory.
 *
 * @returns VBox status code.
 * @param   pszUriList  The URI list (zero terminated, modified).
 * @param   pszDropDir  The drop directory.
 * @param   ppv         Where to return the data (malloc'ed).
 * @param   pcb         Where to return its size.
 */
static int vboxDnDBuildHDrop(char *pszUriList, const char *pszDropDir, void **ppv, DWORD *pcb)
{
    char szPath[MAX_PATH];
    DROPFILES *pDropFiles;
    char *pszLine = pszUriList;
    DWORD cbAlloc = sizeof(DROPFILES) + 1;
    DWORD off = sizeof(DROPFILES);
    BYTE *pb;

    pb = (BYTE *)malloc(cbAlloc);
    if (!pb)
        return VERR_NO_MEMORY;

    while (*pszLine)
    {
        char *pszEnd = pszLine + strcspn(pszLine, "\r\n");
        char *pszName;
        char chSaved = *pszEnd;
        DWORD cchPath;

        *pszEnd = '\0';
        if (*pszLine && *pszLine != '#')
        {
            /* file:///C:/dir/name, file://host/dir/name/ or a plain path: the last component counts. */
            vboxDnDUriDecode(pszLine);
            pszName = pszLine + lstrlen(pszLine);
            while (pszName > pszLine && (pszName[-1] == '/' || pszName[-1] == '\\'))
                *--pszName = '\0';
            while (pszName > pszLine && pszName[-1] != '/' && pszName[-1] != '\\')
                pszName--;

            lstrcpyn(szPath, pszDropDir, sizeof(szPath));
            if (RT_SUCCESS(vboxDnDAppendName(szPath, sizeof(szPath), pszName, lstrlen(pszName))))
            {
                BYTE *pbNew;
                cchPath = lstrlen(szPath) + 1;
                pbNew = (BYTE *)realloc(pb, cbAlloc + cchPath);
                if (!pbNew)
                {
                    free(pb);
                    return VERR_NO_MEMORY;
                }
                pb = pbNew;
                memcpy(pb + off, szPath, cchPath);
                off     += cchPath;
                cbAlloc += cchPath;
            }
        }
        *pszEnd = chSaved;
        pszLine = pszEnd;
        while (*pszLine == '\r' || *pszLine == '\n')
            pszLine++;
    }

    if (off == sizeof(DROPFILES))
    {
        free(pb);
        return VERR_NOT_FOUND;
    }
    pb[off] = '\0'; /* The list ends with an empty string. */

    pDropFiles = (DROPFILES *)pb;
    pDropFiles->pFiles = sizeof(DROPFILES);
    pDropFiles->pt.x   = 0;
    pDropFiles->pt.y   = 0;
    pDropFiles->fNC    = FALSE;
    pDropFiles->fWide  = FALSE;
    *ppv = pb;
    *pcb = cbAlloc;
    return VINF_SUCCESS;
}

/**
 * Builds the CF_TEXT data from UTF-8 text: ANSI, CRLF line ends.
 */
static int vboxDnDBuildText(const char *pch, DWORD cch, void **ppv, DWORD *pcb)
{
    char *pszAnsi;
    char *pszRet;
    DWORD cLf = 0;
    DWORD offSrc;
    DWORD offDst = 0;

    pszAnsi = vboxDnDUtf8ToAnsi(pch, cch, '?');
    if (!pszAnsi)
        return VERR_NO_MEMORY;
    for (offSrc = 0; pszAnsi[offSrc]; offSrc++)
        if (pszAnsi[offSrc] == '\n' && (offSrc == 0 || pszAnsi[offSrc - 1] != '\r'))
            cLf++;

    pszRet = (char *)malloc(offSrc + cLf + 1);
    if (!pszRet)
    {
        free(pszAnsi);
        return VERR_NO_MEMORY;
    }
    for (offSrc = 0; pszAnsi[offSrc]; offSrc++)
    {
        if (pszAnsi[offSrc] == '\n' && (offSrc == 0 || pszAnsi[offSrc - 1] != '\r'))
            pszRet[offDst++] = '\r';
        pszRet[offDst++] = pszAnsi[offSrc];
    }
    pszRet[offDst++] = '\0';
    free(pszAnsi);

    *ppv = pszRet;
    *pcb = offDst;
    return VINF_SUCCESS;
}


/*********************************************************************************************************************************
*   Receiving data (worker thread)                                                                                               *
*********************************************************************************************************************************/

/**
 * Creates the drop directory, %TEMP%\VirtualBox Dropped Files\<UTC time
 * stamp> like upstream's DnDDroppedFilesOpenTemp.
 */
static int vboxDnDCreateDropDir(char *pszDir, DWORD cbDir)
{
    char szStamp[64];
    SYSTEMTIME Now;
    DWORD cch;
    DWORD i;

    cch = GetTempPath(cbDir, pszDir);
    if (!cch || cch >= cbDir)
        return VERR_PATH_NOT_FOUND;
    if (pszDir[cch - 1] != '\\')
        lstrcat(pszDir, "\\");
    if (lstrlen(pszDir) + sizeof(VBOXDND_DROP_DIR_NAME) + 40 > cbDir)
        return VERR_FILENAME_TOO_LONG;
    lstrcat(pszDir, VBOXDND_DROP_DIR_NAME);
    CreateDirectory(pszDir, NULL); /* May exist already. */

    /* RTTimeSpecToString with the colons sanitized, e.g. 2026-10-25T18_30_00.123000000Z. */
    GetSystemTime(&Now);
    wsprintf(szStamp, "\\%04u-%02u-%02uT%02u_%02u_%02u.%03u000000Z", Now.wYear, Now.wMonth, Now.wDay,
             Now.wHour, Now.wMinute, Now.wSecond, Now.wMilliseconds);
    lstrcat(pszDir, szStamp);
    if (CreateDirectory(pszDir, NULL))
        return VINF_SUCCESS;

    /* Taken: add a number, like RTDirCreateUniqueNumbered. */
    cch = lstrlen(pszDir);
    for (i = 1; i < 1000; i++)
    {
        wsprintf(&pszDir[cch], "-%03lu", i);
        if (CreateDirectory(pszDir, NULL))
            return VINF_SUCCESS;
    }
    return VERR_OPEN_FAILED;
}

/**
 * Receives the directories and files of a URI transfer (vbglR3DnDHGRecvURIData).
 *
 * @param   cbToRecv    File bytes to receive.
 * @param   cObjects    Directories and files to receive.
 */
static int vboxDnDRecvUriData(HANDLE hDevice, UINT32 idClient, const char *pszDropDir, UINT64 cbToRecv, UINT64 cObjects,
                              UINT32 *pcFiles)
{
    char szPath[MAX_PATH];
    HANDLE hFile = INVALID_HANDLE_VALUE;
    UINT64 cbFileSize = 0;
    UINT64 cbFileWritten = 0;
    VBGLR3HGCMMSG Msg;
    int rc = VINF_SUCCESS;

    *pcFiles = 0;
    while (cObjects || cbToRecv)
    {
        UINT32 uMsg;
        UINT32 cParms;
        BYTE *pb;
        DWORD cb;

        rc = vboxDnDGetNextMsg(hDevice, idClient, &uMsg, &cParms);
        if (RT_FAILURE(rc))
            break;

        if (uMsg == HOST_DND_FN_HG_SND_DIR)
        {
            if (!VbglR3HgcmMsgInit(&Msg, idClient, uMsg, 4, VBOXDND_CALL_TIMEOUT_MS, MAX_PATH * 4))
            {
                rc = VERR_NO_MEMORY;
                break;
            }
            VbglR3HgcmMsgSetU32(&Msg, 0, 0);                                                       /* uContext */
            pb = VbglR3HgcmMsgSetBuf(&Msg, 1, VBOXGUEST98_HGCM_PARM_BUFFER_OUT, MAX_PATH * 4, NULL); /* pvName */
            VbglR3HgcmMsgSetU32(&Msg, 2, MAX_PATH * 4);                                            /* cbName */
            VbglR3HgcmMsgSetU32(&Msg, 3, 0);                                                       /* fMode */
            rc = VbglR3HgcmMsgCall(hDevice, &Msg);
            if (RT_SUCCESS(rc))
            {
                cb = VbglR3HgcmMsgGetBufSize(&Msg, 1);
                if (cb > MAX_PATH * 4)
                    rc = VERR_TOO_MUCH_DATA;
                else
                    rc = vboxDnDBuildPath(pszDropDir, (const char *)pb, vboxDnDStrNLen(pb, cb), szPath);
                if (RT_SUCCESS(rc) && !CreateDirectory(szPath, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
                    rc = VERR_OPEN_FAILED;
                if (RT_SUCCESS(rc) && cObjects)
                    cObjects--;
            }
            VbglR3HgcmMsgFree(&Msg);
        }
        else if (uMsg == HOST_DND_FN_HG_SND_FILE_HDR)
        {
            if (hFile != INVALID_HANDLE_VALUE)
            {
                rc = VERR_WRONG_ORDER; /* The previous file isn't complete. */
                break;
            }
            if (!VbglR3HgcmMsgInit(&Msg, idClient, uMsg, 6, VBOXDND_CALL_TIMEOUT_MS, MAX_PATH * 4))
            {
                rc = VERR_NO_MEMORY;
                break;
            }
            VbglR3HgcmMsgSetU32(&Msg, 0, 0);                                                       /* uContext */
            pb = VbglR3HgcmMsgSetBuf(&Msg, 1, VBOXGUEST98_HGCM_PARM_BUFFER_OUT, MAX_PATH * 4, NULL); /* pvName */
            VbglR3HgcmMsgSetU32(&Msg, 2, MAX_PATH * 4);                                            /* cbName */
            VbglR3HgcmMsgSetU32(&Msg, 3, 0);                                                       /* uFlags */
            VbglR3HgcmMsgSetU32(&Msg, 4, 0);                                                       /* fMode */
            VbglR3HgcmMsgSetU64(&Msg, 5, 0);                                                       /* cbTotal */
            rc = VbglR3HgcmMsgCall(hDevice, &Msg);
            if (RT_SUCCESS(rc))
            {
                cb = VbglR3HgcmMsgGetBufSize(&Msg, 1);
                cbFileSize = VbglR3HgcmMsgGetU64(&Msg, 5);
                cbFileWritten = 0;
                if (cb > MAX_PATH * 4 || cbFileSize > cbToRecv)
                    rc = VERR_TOO_MUCH_DATA;
                else
                    rc = vboxDnDBuildPath(pszDropDir, (const char *)pb, vboxDnDStrNLen(pb, cb), szPath);
                if (RT_SUCCESS(rc))
                {
                    hFile = CreateFile(szPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                    if (hFile == INVALID_HANDLE_VALUE)
                        rc = VERR_OPEN_FAILED;
                    else
                        (*pcFiles)++;
                }
            }
            VbglR3HgcmMsgFree(&Msg);
        }
        else if (uMsg == HOST_DND_FN_HG_SND_FILE_DATA)
        {
            if (hFile == INVALID_HANDLE_VALUE)
            {
                rc = VERR_WRONG_ORDER;
                break;
            }
            if (!VbglR3HgcmMsgInit(&Msg, idClient, uMsg, 5, VBOXDND_CALL_TIMEOUT_MS, VBOX_DND_CHUNK_SIZE))
            {
                rc = VERR_NO_MEMORY;
                break;
            }
            VbglR3HgcmMsgSetU32(&Msg, 0, 0);                                                           /* uContext */
            pb = VbglR3HgcmMsgSetBuf(&Msg, 1, VBOXGUEST98_HGCM_PARM_BUFFER_OUT, VBOX_DND_CHUNK_SIZE, NULL); /* pvData */
            VbglR3HgcmMsgSetU32(&Msg, 2, 0);                                                           /* cbData */
            VbglR3HgcmMsgSetBuf(&Msg, 3, VBOXGUEST98_HGCM_PARM_BUFFER_OUT, 0, NULL);                   /* pvChecksum */
            VbglR3HgcmMsgSetU32(&Msg, 4, 0);                                                           /* cbChecksum */
            rc = VbglR3HgcmMsgCall(hDevice, &Msg);
            if (RT_SUCCESS(rc))
            {
                DWORD cbWritten = 0;
                cb = VbglR3HgcmMsgGetU32(&Msg, 2);
                if (cb > VBOX_DND_CHUNK_SIZE || cb > cbFileSize - cbFileWritten)
                    rc = VERR_TOO_MUCH_DATA;
                else if (cb && (!WriteFile(hFile, pb, cb, &cbWritten, NULL) || cbWritten != cb))
                    rc = VERR_WRITE_ERROR;
                else
                {
                    cbFileWritten += cb;
                    cbToRecv      -= cb;
                }
            }
            VbglR3HgcmMsgFree(&Msg);
        }
        else if (uMsg == HOST_DND_FN_CANCEL)
        {
            vboxDnDRecvSimple(hDevice, idClient, uMsg);
            rc = VERR_CANCELLED;
        }
        else
            rc = VERR_WRONG_ORDER;
        if (RT_FAILURE(rc))
            break;

        /* A file is complete when all its bytes are here (at once for empty files). */
        if (hFile != INVALID_HANDLE_VALUE && cbFileWritten == cbFileSize)
        {
            CloseHandle(hFile);
            hFile = INVALID_HANDLE_VALUE;
            if (cObjects)
                cObjects--;
        }
    }

    if (hFile != INVALID_HANDLE_VALUE)
    {
        CloseHandle(hFile);
        DeleteFile(szPath); /* Incomplete. */
    }
    return rc;
}

/**
 * Receives the dropped data, after HOST_DND_FN_HG_SND_DATA_HDR arrived
 * (vbglR3DnDHGRecvDataMain), and hands it to the data object.
 */
static int vboxDnDRecvData(HANDLE hDevice, UINT32 idClient)
{
    char szDropDir[MAX_PATH];
    char *pszMetaFmt = NULL;
    BYTE *pbMeta = NULL;
    void *pvData = NULL;
    DWORD cbData = 0;
    UINT64 cbTotal;
    UINT64 cObjects;
    UINT32 cbMeta;
    UINT32 cbMetaRecv = 0;
    UINT32 cFiles = 0;
    VBGLR3HGCMMSG Msg;
    BYTE *pb;
    int rc;

    /*
     * The header.
     */
    if (!VbglR3HgcmMsgInit(&Msg, idClient, HOST_DND_FN_HG_SND_DATA_HDR, 12, VBOXDND_CALL_TIMEOUT_MS, VBOX_DND_CHUNK_SIZE))
        return VERR_NO_MEMORY;
    VbglR3HgcmMsgSetU32(&Msg, 0, 0);                                                           /* uContext */
    VbglR3HgcmMsgSetU32(&Msg, 1, 0);                                                           /* uFlags */
    VbglR3HgcmMsgSetU32(&Msg, 2, 0);                                                           /* uScreenId */
    VbglR3HgcmMsgSetU64(&Msg, 3, 0);                                                           /* cbTotal */
    VbglR3HgcmMsgSetU32(&Msg, 4, 0);                                                           /* cbMeta */
    pb = VbglR3HgcmMsgSetBuf(&Msg, 5, VBOXGUEST98_HGCM_PARM_BUFFER_OUT, VBOX_DND_CHUNK_SIZE, NULL); /* pvMetaFmt */
    VbglR3HgcmMsgSetU32(&Msg, 6, 0);                                                           /* cbMetaFmt */
    VbglR3HgcmMsgSetU64(&Msg, 7, 0);                                                           /* cObjects */
    VbglR3HgcmMsgSetU32(&Msg, 8, 0);                                                           /* enmCompression */
    VbglR3HgcmMsgSetU32(&Msg, 9, 0);                                                           /* enmChecksumType */
    VbglR3HgcmMsgSetBuf(&Msg, 10, VBOXGUEST98_HGCM_PARM_BUFFER_OUT, 0, NULL);                  /* pvChecksum */
    VbglR3HgcmMsgSetU32(&Msg, 11, 0);                                                          /* cbChecksum */
    rc = VbglR3HgcmMsgCall(hDevice, &Msg);
    if (RT_SUCCESS(rc))
    {
        cbTotal  = VbglR3HgcmMsgGetU64(&Msg, 3);
        cbMeta   = VbglR3HgcmMsgGetU32(&Msg, 4);
        cObjects = VbglR3HgcmMsgGetU64(&Msg, 7);
        pb[VBOX_DND_CHUNK_SIZE - 1] = '\0';
        pszMetaFmt = _strdup((const char *)pb);
        if (!pszMetaFmt)
            rc = VERR_NO_MEMORY;
        else if (cbMeta > VBOXDND_MAX_META_SIZE || cbMeta > cbTotal)
            rc = VERR_TOO_MUCH_DATA;
    }
    VbglR3HgcmMsgFree(&Msg);

    /*
     * The meta data: the text, or the URI list of the transfer roots.
     */
    if (RT_SUCCESS(rc))
    {
        pbMeta = (BYTE *)malloc(cbMeta + 1);
        if (!pbMeta)
            rc = VERR_NO_MEMORY;
    }
    while (RT_SUCCESS(rc) && cbMetaRecv < cbMeta)
    {
        DWORD cbChunk = cbMeta - cbMetaRecv < VBOX_DND_CHUNK_SIZE ? cbMeta - cbMetaRecv : VBOX_DND_CHUNK_SIZE;
        DWORD cbGot;

        if (!VbglR3HgcmMsgInit(&Msg, idClient, HOST_DND_FN_HG_SND_DATA, 5, VBOXDND_CALL_TIMEOUT_MS, cbChunk))
        {
            rc = VERR_NO_MEMORY;
            break;
        }
        VbglR3HgcmMsgSetU32(&Msg, 0, 0);                                                       /* uContext */
        pb = VbglR3HgcmMsgSetBuf(&Msg, 1, VBOXGUEST98_HGCM_PARM_BUFFER_OUT, cbChunk, NULL);    /* pvData */
        VbglR3HgcmMsgSetU32(&Msg, 2, 0);                                                       /* cbData */
        VbglR3HgcmMsgSetBuf(&Msg, 3, VBOXGUEST98_HGCM_PARM_BUFFER_OUT, 0, NULL);               /* pvChecksum */
        VbglR3HgcmMsgSetU32(&Msg, 4, 0);                                                       /* cbChecksum */
        rc = VbglR3HgcmMsgCall(hDevice, &Msg);
        if (RT_SUCCESS(rc))
        {
            cbGot = VbglR3HgcmMsgGetU32(&Msg, 2);
            if (!cbGot || cbGot > cbChunk)
                rc = VERR_TOO_MUCH_DATA;
            else
            {
                memcpy(pbMeta + cbMetaRecv, pb, cbGot);
                cbMetaRecv += cbGot;
            }
        }
        VbglR3HgcmMsgFree(&Msg);
    }
    if (pbMeta)
        pbMeta[cbMetaRecv] = '\0';

    /*
     * Files and directories, or text.
     */
    if (RT_SUCCESS(rc))
    {
        if (!lstrcmpi(pszMetaFmt, "text/uri-list"))
        {
            rc = vboxDnDCreateDropDir(szDropDir, sizeof(szDropDir));
            if (RT_SUCCESS(rc))
                rc = vboxDnDRecvUriData(hDevice, idClient, szDropDir, cbTotal - cbMeta, cObjects, &cFiles);
            if (RT_SUCCESS(rc))
                rc = vboxDnDBuildHDrop((char *)pbMeta, szDropDir, &pvData, &cbData);
            if (RT_SUCCESS(rc))
                VBoxTrayLogRel("DnD: received %lu file(s), %lu objects, %lu KB in \"%s\"\n", cFiles,
                               (unsigned long)cObjects, (unsigned long)((cbTotal - cbMeta) / 1024), szDropDir);
        }
        else
        {
            rc = vboxDnDBuildText((const char *)pbMeta, cbMetaRecv, &pvData, &cbData);
            if (RT_SUCCESS(rc))
                VBoxTrayLogRel("DnD: received %lu bytes of text (%s)\n", cbMetaRecv, pszMetaFmt);
        }
    }
    free(pbMeta);
    free(pszMetaFmt);

    if (RT_SUCCESS(rc))
    {
        EnterCriticalSection(&g_DnDCritSect);
        if (g_pDnDDataObj)
        {
            VBoxDnDDataObjectSetData(g_pDnDDataObj, pvData, cbData);
            pvData = NULL;
        }
        else
            rc = VERR_NOT_FOUND; /* The drag was cancelled meanwhile. */
        LeaveCriticalSection(&g_DnDCritSect);
        free(pvData);
    }
    else if (rc != VERR_CANCELLED)
    {
        VBoxTrayLogRel("DnD: receiving the data failed, rc=%d\n", rc);
        vboxDnDSendProgress(hDevice, idClient, DND_PROGRESS_ERROR, 100, rc);
    }
    return rc;
}


/*********************************************************************************************************************************
*   Sending data to the host (worker thread)                                                                                     *
*********************************************************************************************************************************/

/**
 * Converts text in the ANSI code page to UTF-8.
 *
 * @returns Zero terminated UTF-8 (malloc'ed), NULL on failure.
 */
static char *vboxDnDAnsiToUtf8(const char *psz)
{
    WCHAR *pwsz;
    BYTE *pbRet;
    DWORD off = 0;
    int cwc;
    int i;

    cwc = MultiByteToWideChar(CP_ACP, 0, psz, -1, NULL, 0);
    if (cwc <= 0)
        return NULL;
    pwsz = (WCHAR *)malloc(cwc * sizeof(WCHAR));
    pbRet = (BYTE *)malloc(cwc * 3); /* Up to 3 bytes per UTF-16 unit, 4 per surrogate pair. */
    if (!pwsz || !pbRet || !MultiByteToWideChar(CP_ACP, 0, psz, -1, pwsz, cwc))
    {
        free(pwsz);
        free(pbRet);
        return NULL;
    }

    for (i = 0; i < cwc; i++)
    {
        DWORD uc = pwsz[i];
        if (   uc >= 0xd800 && uc <= 0xdbff
            && i + 1 < cwc && pwsz[i + 1] >= 0xdc00 && pwsz[i + 1] <= 0xdfff)
        {
            uc = 0x10000 + ((uc - 0xd800) << 10) + (pwsz[i + 1] - 0xdc00);
            i++;
        }
        if (uc < 0x80)
            pbRet[off++] = (BYTE)uc;
        else if (uc < 0x800)
        {
            pbRet[off++] = (BYTE)(0xc0 | (uc >> 6));
            pbRet[off++] = (BYTE)(0x80 | (uc & 0x3f));
        }
        else if (uc < 0x10000)
        {
            pbRet[off++] = (BYTE)(0xe0 | (uc >> 12));
            pbRet[off++] = (BYTE)(0x80 | ((uc >> 6) & 0x3f));
            pbRet[off++] = (BYTE)(0x80 | (uc & 0x3f));
        }
        else
        {
            pbRet[off++] = (BYTE)(0xf0 | (uc >> 18));
            pbRet[off++] = (BYTE)(0x80 | ((uc >> 12) & 0x3f));
            pbRet[off++] = (BYTE)(0x80 | ((uc >> 6) & 0x3f));
            pbRet[off++] = (BYTE)(0x80 | (uc & 0x3f));
        }
    }
    free(pwsz);
    return (char *)pbRet; /* The terminator was converted too. */
}

/**
 * Adds a directory or file to the transfer list.
 *
 * @param   pszRel      Its path in the transfer (ANSI, '\' separated).
 */
static int vboxDnDGhListAdd(VBOXDNDGHLIST *pList, const char *pszAbs, const char *pszRel, BOOL fDir, UINT64 cb)
{
    VBOXDNDGHOBJ *pObj;
    char *psz;

    if (pList->cObjs == pList->cAlloc)
    {
        VBOXDNDGHOBJ *paNew = (VBOXDNDGHOBJ *)realloc(pList->paObjs, (pList->cAlloc + 64) * sizeof(VBOXDNDGHOBJ));
        if (!paNew)
            return VERR_NO_MEMORY;
        pList->paObjs  = paNew;
        pList->cAlloc += 64;
    }

    pObj = &pList->paObjs[pList->cObjs];
    pObj->pszAbs = _strdup(pszAbs);
    pObj->pszRel = vboxDnDAnsiToUtf8(pszRel);
    if (!pObj->pszAbs || !pObj->pszRel)
    {
        free(pObj->pszAbs);
        free(pObj->pszRel);
        return VERR_NO_MEMORY;
    }
    /* After the conversion, since a DBCS trail byte can look like a backslash. */
    for (psz = pObj->pszRel; *psz; psz++)
        if (*psz == '\\')
            *psz = '/';
    pObj->fDir = fDir;
    pObj->cb   = cb;
    pList->cObjs++;
    if (!fDir)
        pList->cbFiles += cb;
    return VINF_SUCCESS;
}

/**
 * Adds a file, or a directory and everything in it, to the transfer list.
 *
 * @param   pFd         What FindFirstFile/FindNextFile found for it.
 */
static int vboxDnDGhListAddTree(VBOXDNDGHLIST *pList, const char *pszAbs, const char *pszRel, const WIN32_FIND_DATA *pFd)
{
    char szAbs[MAX_PATH];
    char szRel[MAX_PATH];
    WIN32_FIND_DATA Fd;
    HANDLE hFind;
    int rc;

    if (!(pFd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return vboxDnDGhListAdd(pList, pszAbs, pszRel, FALSE, ((UINT64)pFd->nFileSizeHigh << 32) | pFd->nFileSizeLow);

    rc = vboxDnDGhListAdd(pList, pszAbs, pszRel, TRUE, 0);
    if (RT_FAILURE(rc))
        return rc;
    if (lstrlen(pszAbs) + 4 >= MAX_PATH)
        return VERR_FILENAME_TOO_LONG;
    wsprintf(szAbs, "%s\\*.*", pszAbs);
    hFind = FindFirstFile(szAbs, &Fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return VINF_SUCCESS; /* Empty. */
    do
    {
        if (!lstrcmp(Fd.cFileName, ".") || !lstrcmp(Fd.cFileName, ".."))
            continue;
        if (   lstrlen(pszAbs) + 1 + lstrlen(Fd.cFileName) >= MAX_PATH
            || lstrlen(pszRel) + 1 + lstrlen(Fd.cFileName) >= MAX_PATH)
        {
            rc = VERR_FILENAME_TOO_LONG;
            break;
        }
        wsprintf(szAbs, "%s\\%s", pszAbs, Fd.cFileName);
        wsprintf(szRel, "%s\\%s", pszRel, Fd.cFileName);
        rc = vboxDnDGhListAddTree(pList, szAbs, szRel, &Fd);
    } while (RT_SUCCESS(rc) && FindNextFile(hFind, &Fd));
    FindClose(hFind);
    return rc;
}

static void vboxDnDGhListFree(VBOXDNDGHLIST *pList)
{
    DWORD i;
    for (i = 0; i < pList->cObjs; i++)
    {
        free(pList->paObjs[i].pszAbs);
        free(pList->paObjs[i].pszRel);
    }
    free(pList->paObjs);
    memset(pList, 0, sizeof(*pList));
}

/**
 * Appends a transfer root to the URI list: file:///<name>, %XX escaped, as
 * DnDTransferListGetRoots makes them.
 *
 * @param   ppszMeta    The list (malloc'ed, reallocated).
 * @param   pcchMeta    Its length.
 * @param   pszName     The root's name (ANSI).
 */
static int vboxDnDGhMetaAddRoot(char **ppszMeta, DWORD *pcchMeta, const char *pszName)
{
    static const char s_szHex[] = "0123456789ABCDEF";
    char *pszUtf8 = vboxDnDAnsiToUtf8(pszName);
    char *pszNew;
    BYTE *pb;
    DWORD off;

    if (!pszUtf8)
        return VERR_NO_MEMORY;
    pszNew = (char *)realloc(*ppszMeta, *pcchMeta + sizeof("file:///\r\n") + lstrlen(pszUtf8) * 3);
    if (!pszNew)
    {
        free(pszUtf8);
        return VERR_NO_MEMORY;
    }
    *ppszMeta = pszNew;

    off = *pcchMeta;
    memcpy(&pszNew[off], "file:///", 8);
    off += 8;
    for (pb = (BYTE *)pszUtf8; *pb; pb++)
    {
        if (   (*pb >= 'a' && *pb <= 'z') || (*pb >= 'A' && *pb <= 'Z') || (*pb >= '0' && *pb <= '9')
            || *pb == '-' || *pb == '.' || *pb == '_' || *pb == '~')
            pszNew[off++] = (char)*pb;
        else
        {
            pszNew[off++] = '%';
            pszNew[off++] = s_szHex[*pb >> 4];
            pszNew[off++] = s_szHex[*pb & 0xf];
        }
    }
    memcpy(&pszNew[off], VBOX_DND_FORMATS_SEPARATOR, 2);
    off += 2;
    pszNew[off] = '\0';
    *pcchMeta = off;
    free(pszUtf8);
    return VINF_SUCCESS;
}

/**
 * Sends dropped guest files and directories to the host
 * (vbglR3DnDGHSendTransferData): the URI list of the roots as meta data,
 * then every directory and file below them.
 *
 * @param   pszPaths    The dropped paths, CF_HDROP style (each zero
 *                      terminated, an empty one ends the list).
 */
static int vboxDnDGhSendFiles(HANDLE hDevice, UINT32 idClient, const char *pszPaths)
{
    VBOXDNDGHLIST List;
    char *pszMeta = NULL;
    DWORD cchMeta = 0;
    const char *psz;
    DWORD i;
    int rc = VINF_SUCCESS;

    memset(&List, 0, sizeof(List));
    for (psz = pszPaths; *psz && RT_SUCCESS(rc); psz += lstrlen(psz) + 1)
    {
        WIN32_FIND_DATA Fd;
        HANDLE hFind = FindFirstFile(psz, &Fd);
        if (hFind == INVALID_HANDLE_VALUE)
        {
            VBoxTrayLogRel("DnD: \"%s\" not found, not sent\n", psz);
            continue; /* A drive or such. */
        }
        FindClose(hFind);
        rc = vboxDnDGhListAddTree(&List, psz, Fd.cFileName, &Fd);
        if (RT_SUCCESS(rc))
            rc = vboxDnDGhMetaAddRoot(&pszMeta, &cchMeta, Fd.cFileName);
    }
    if (RT_SUCCESS(rc) && !List.cObjs)
        rc = VERR_NO_DATA;

    /* The meta data includes the terminator, like upstream's. */
    if (RT_SUCCESS(rc))
        rc = vboxDnDGhSendMeta(hDevice, idClient, "text/uri-list", pszMeta, cchMeta + 1, cchMeta + 1 + List.cbFiles,
                               List.cObjs);
    for (i = 0; RT_SUCCESS(rc) && i < List.cObjs; i++)
    {
        if (List.paObjs[i].fDir)
            rc = vboxDnDGhSendDir(hDevice, idClient, List.paObjs[i].pszRel);
        else
            rc = vboxDnDGhSendFile(hDevice, idClient, List.paObjs[i].pszAbs, List.paObjs[i].pszRel, List.paObjs[i].cb);
        if (RT_FAILURE(rc))
            VBoxTrayLogRel("DnD: sending \"%s\" failed, rc=%d\n", List.paObjs[i].pszAbs, rc);
    }
    if (RT_SUCCESS(rc))
        VBoxTrayLogRel("DnD: sent %lu objects, %lu KB to the host\n", List.cObjs, (unsigned long)(List.cbFiles / 1024));

    free(pszMeta);
    vboxDnDGhListFree(&List);
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
    WaitForSingleObject(g_hDnDGhDropped, VBOXDND_GH_DROP_WAIT_MS);

    EnterCriticalSection(&g_DnDCritSect);
    pszData = g_pszDnDGhData;
    fFiles  = g_fDnDGhFiles;
    g_pszDnDGhData = NULL;
    LeaveCriticalSection(&g_DnDCritSect);

    VBoxTrayLogRel("DnD: dropped on the host, which wants %s\n", pszFormat);
    if (!pszData)
        rc = VERR_NO_DATA;
    else if (fFiles)
        rc = vboxDnDGhSendFiles(hDevice, idClient, pszData);
    else
    {
        DWORD cb = lstrlen(pszData) + 1;
        rc = vboxDnDGhSendMeta(hDevice, idClient, NULL, pszData, cb, cb, 0);
        if (RT_SUCCESS(rc))
            VBoxTrayLogRel("DnD: sent %lu bytes of text to the host\n", cb);
    }
    free(pszData);

    if (RT_FAILURE(rc))
    {
        VBoxTrayLogRel("DnD: sending to the host failed, rc=%d\n", rc);
        vboxDnDSendError(hDevice, idClient, rc); /* So the host doesn't wait for the data. */
    }
    return rc;
}


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
    if (!g_fDnDButtonDown)
    {
        fFlags |= MOUSEEVENTF_LEFTDOWN;
        g_fDnDButtonDown = TRUE;
    }
    mouse_event(fFlags, (DWORD)MulDiv(x, 65535, cx > 1 ? cx - 1 : 1), (DWORD)MulDiv(y, 65535, cy > 1 ? cy - 1 : 1), 0, 0);
}

/**
 * Releases the left button if we pressed it.
 */
static void vboxDnDMouseRelease(void)
{
    if (g_fDnDButtonDown)
    {
        g_fDnDButtonDown = FALSE;
        mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
    }
}

/**
 * Releases the OLE objects of the drag, once DoDragDrop isn't using them.
 */
static void vboxDnDReleaseObjects(void)
{
    IDataObject *pDataObj;

    EnterCriticalSection(&g_DnDCritSect);
    pDataObj = g_pDnDDataObj;
    g_pDnDDataObj = NULL;
    LeaveCriticalSection(&g_DnDCritSect);
    if (pDataObj)
        pDataObj->lpVtbl->Release(pDataObj);
    if (g_pDnDDropSource)
    {
        g_pDnDDropSource->lpVtbl->Release(g_pDnDDropSource);
        g_pDnDDropSource = NULL;
    }
    g_pDnDFormat = NULL;
}

/**
 * Ends the current host drag: cancels DoDragDrop if it runs, releases the
 * button and hides the proxy window.
 */
static void vboxDnDAbort(void)
{
    EnterCriticalSection(&g_DnDCritSect);
    if (g_pDnDDataObj)
        VBoxDnDDataObjectAbort(g_pDnDDataObj);
    LeaveCriticalSection(&g_DnDCritSect);
    if (g_pDnDDropSource)
        VBoxDnDDropSourceAbort(g_pDnDDropSource); /* Before releasing the button, or it would drop. */

    vboxDnDMouseRelease();
    ShowWindow(g_hwndDnD, SW_HIDE);
    g_fDnDActive = FALSE;
    if (!g_fDnDDragging)
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

    if (   g_fDnDDragging
        && g_pDnDDropSource
        && (VBoxDnDDropSourceGetEffect(g_pDnDDropSource) & DROPEFFECT_COPY))
        uAction = VBOX_DND_ACTION_COPY;
    vboxDnDSendAck(g_hDnDUiDevice, g_idDnDUiClient, uAction);
}

/**
 * The pointer moved (also right after the enter).
 */
static void vboxDnDOnMove(UINT32 x, UINT32 y)
{
    if (g_fDnDActive)
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
    if (g_fDnDDragging)
    {
        /* A new drag while the old one still runs (its leave got lost):
           end the old one, and take this one when DoDragDrop returns. */
        vboxDnDAbort();
        free(g_pDnDPendingEnter);
        g_pDnDPendingEnter = pEnter;
        vboxDnDAck();
        return;
    }
    if (g_fDnDActive)
        vboxDnDAbort();
    vboxDnDGhReset(); /* A host drag replaces a guest one. */

    g_pDnDFormat = vboxDnDPickFormat(pEnter->szFormats);
    if (!g_pDnDFormat || !(pEnter->fAllActions & VBOX_DND_ACTION_COPY))
        VBoxTrayLogRel("DnD: nothing the guest can take (actions %#lx, formats %s)\n", pEnter->fAllActions,
                       pEnter->szFormats);
    else
    {
        IDataObject *pDataObj = VBoxDnDDataObjectCreate(g_pDnDFormat->cf);
        g_pDnDDropSource = VBoxDnDDropSourceCreate();
        if (pDataObj && g_pDnDDropSource)
        {
            EnterCriticalSection(&g_DnDCritSect);
            g_pDnDDataObj = pDataObj;
            LeaveCriticalSection(&g_DnDCritSect);

            VBoxTrayLogRel("DnD: host drag entered, using %s\n", g_pDnDFormat->pszMime);
            SetWindowPos(g_hwndDnD, HWND_TOPMOST, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
                         SWP_SHOWWINDOW | SWP_NOACTIVATE | SWP_NOREDRAW);
            g_fDnDActive = TRUE;
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

    if (!g_fDnDDragging || !g_pDnDFormat)
    {
        /* Nothing to drop on (like upstream, the host isn't asked for data). */
        VBoxTrayLogRel("DnD: dropped, but no guest drag is in progress\n");
        vboxDnDAbort();
        return;
    }

    EnterCriticalSection(&g_DnDCritSect);
    pDataObj = g_pDnDDataObj;
    if (pDataObj)
        VBoxDnDDataObjectSetDropping(pDataObj);
    LeaveCriticalSection(&g_DnDCritSect);

    VBoxTrayLogRel("DnD: dropped, requesting %s\n", g_pDnDFormat->pszMime);
    rc = vboxDnDSendReqData(g_hDnDUiDevice, g_idDnDUiClient, g_pDnDFormat->pszMime);
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
    IDropSource *pDropSource = g_pDnDDropSource;
    DWORD dwEffect = DROPEFFECT_NONE;
    HRESULT hr;

    EnterCriticalSection(&g_DnDCritSect);
    pDataObj = g_pDnDDataObj;
    LeaveCriticalSection(&g_DnDCritSect);
    if (!g_fDnDActive || g_fDnDDragging || !pDataObj || !pDropSource)
        return;

    /* Out of the way, so the windows below get the drag. */
    ShowWindow(g_hwndDnD, SW_HIDE);
    g_fDnDDragging = TRUE;
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
    g_fDnDDragging = FALSE;
    vboxDnDAbort(); /* Releases the button if still down, and the objects. */

    if (g_pDnDPendingEnter)
    {
        VBOXDNDENTER *pEnter = g_pDnDPendingEnter;
        g_pDnDPendingEnter = NULL;
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

    EnterCriticalSection(&g_DnDCritSect);
    pszFormat = g_pszDnDGhFormat;
    LeaveCriticalSection(&g_DnDCritSect);

    rc = vboxDnDSendAckPending(g_hDnDUiDevice, g_idDnDUiClient, pszFormat ? VBOX_DND_ACTION_COPY : VBOX_DND_ACTION_IGNORE,
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

    KillTimer(g_hwndDnD, VBOXDND_GH_TIMER_HIDE);
    KillTimer(g_hwndDnD, VBOXDND_GH_TIMER_EXPIRE);
    if (g_fDnDGhActive && !g_fDnDGhOver) /* Else when the guest drag ends (vboxDnDGhDragDone). */
    {
        ShowWindow(g_hwndDnD, SW_HIDE);
        g_fDnDGhActive = FALSE;
    }

    EnterCriticalSection(&g_DnDCritSect);
    g_pszDnDGhFormat = NULL;
    pszData = g_pszDnDGhData;
    g_pszDnDGhData = NULL;
    ResetEvent(g_hDnDGhDropped);
    LeaveCriticalSection(&g_DnDCritSect);
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

    if (g_fDnDActive || g_fDnDDragging || !g_pDnDDropTarget)
    {
        /* A host drag is in the guest, or no way to catch one: answer "none". */
        vboxDnDSendAckPending(g_hDnDUiDevice, g_idDnDUiClient, VBOX_DND_ACTION_IGNORE, "");
        return;
    }

    if (!g_fDnDGhActive)
    {
        cx = GetSystemMetrics(SM_CXSCREEN);
        cy = GetSystemMetrics(SM_CYSCREEN);
        SetWindowPos(g_hwndDnD, HWND_TOPMOST, 0, 0, cx, cy, SWP_SHOWWINDOW | SWP_NOACTIVATE | SWP_NOREDRAW);
        g_fDnDGhActive = TRUE;

        /* A move first, so the drag notices the window under the pointer. */
        GetCursorPos(&Pt);
        mouse_event(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE, (DWORD)MulDiv(Pt.x, 65535, cx > 1 ? cx - 1 : 1),
                    (DWORD)MulDiv(Pt.y, 65535, cy > 1 ? cy - 1 : 1), 0, 0);
        mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
    }
    /* Take the window away again if no guest drag shows up. */
    if (!g_pszDnDGhFormat)
        SetTimer(g_hwndDnD, VBOXDND_GH_TIMER_HIDE, VBOXDND_GH_HIDE_MS, NULL);

    vboxDnDGhAnswer();
}

/**
 * Copies the paths of a CF_HDROP into a list of zero terminated strings
 * ending with an empty one.
 */
static char *vboxDnDGhCopyHDrop(HDROP hDrop)
{
    UINT cFiles = DragQueryFile(hDrop, 0xffffffff, NULL, 0);
    DWORD cb = 1;
    DWORD off = 0;
    char *pszRet;
    UINT i;

    for (i = 0; i < cFiles; i++)
        cb += DragQueryFile(hDrop, i, NULL, 0) + 1;
    pszRet = (char *)malloc(cb);
    if (!pszRet)
        return NULL;
    for (i = 0; i < cFiles; i++)
        off += DragQueryFile(hDrop, i, &pszRet[off], cb - off) + 1;
    pszRet[off] = '\0';
    return pszRet;
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
                pszData = vboxDnDGhCopyHDrop((HDROP)Medium.hGlobal);
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
                        pszData = vboxDnDAnsiToUtf8(pszAnsi);
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

    if (!g_fDnDGhActive)
        return;
    g_fDnDGhOver = TRUE;
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

    EnterCriticalSection(&g_DnDCritSect);
    free(g_pszDnDGhData);
    g_pszDnDGhData   = pszData;
    g_fDnDGhFiles    = cfFormat == CF_HDROP;
    g_pszDnDGhFormat = pszFormat;
    SetEvent(g_hDnDGhDropped);
    LeaveCriticalSection(&g_DnDCritSect);
    SetTimer(g_hwndDnD, VBOXDND_GH_TIMER_EXPIRE, VBOXDND_GH_EXPIRE_MS, NULL);

    KillTimer(g_hwndDnD, VBOXDND_GH_TIMER_HIDE);
    VBoxTrayLogRel("DnD: guest drag caught for the host, offering %s\n", pszFormat);
    /* The host's next query gets it (vboxDnDGhAnswer). */
}

/**
 * The guest drag is over: the proxy window isn't needed any more.  Until
 * then it stays up, or the drag would drop on the window below it.
 */
static void vboxDnDGhDragDone(void)
{
    g_fDnDGhOver = FALSE;
    if (g_fDnDGhActive)
    {
        ShowWindow(g_hwndDnD, SW_HIDE);
        g_fDnDGhActive = FALSE;
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

    EnterCriticalSection(&g_DnDCritSect);
    fTaken = g_pszDnDGhFormat != NULL;
    LeaveCriticalSection(&g_DnDCritSect);
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
            if (g_fDnDActive || g_fDnDDragging)
                VBoxTrayLogRel("DnD: host drag left the guest or was cancelled\n");
            vboxDnDAbort();
            return 0;

        case WM_VBOXDND_DROP:
            vboxDnDOnDrop();
            return 0;

        case WM_VBOXDND_RECEIVED:
            if (wParam && g_fDnDDragging)
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
            if (wParam && g_fDnDGhOver)
                mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
            vboxDnDGhReset();
            return 0;

        case WM_VBOXDND_CANCEL:
            VBoxTrayLogRel("DnD: the host cancelled the operation\n");
            vboxDnDAbort();
            vboxDnDGhReset();
            return 0;

        case WM_TIMER:
            if (wParam == VBOXDND_GH_TIMER_HIDE)
            {
                KillTimer(hwnd, VBOXDND_GH_TIMER_HIDE);
                if (!g_pszDnDGhFormat)
                    vboxDnDGhReset(); /* No guest drag came. */
            }
            else if (wParam == VBOXDND_GH_TIMER_EXPIRE)
                vboxDnDGhReset();
            return 0;

        case WM_VBOXDND_QUIT:
            free(g_pDnDPendingEnter);
            g_pDnDPendingEnter = NULL;
            vboxDnDAbort();
            vboxDnDGhReset();
            if (g_pDnDDropTarget)
            {
                RevokeDragDrop(hwnd);
                g_pDnDDropTarget->lpVtbl->Release(g_pDnDDropTarget);
                g_pDnDDropTarget = NULL;
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
        g_hwndDnD = CreateWindowEx(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_TRANSPARENT, VBOXDND_WND_CLASS, "VBoxTray DnD",
                                   WS_POPUP, 0, 0, 1, 1, NULL, NULL, WndCls.hInstance, NULL);
        if (!g_hwndDnD)
            VBoxTrayLogRel("DnD: creating the proxy window failed, error %lu\n", GetLastError());
        else
        {
            g_hDnDUiDevice = VbglR3Open();
            rc = g_hDnDUiDevice != INVALID_HANDLE_VALUE ? vboxDnDConnect(g_hDnDUiDevice, &g_idDnDUiClient) : VERR_NOT_FOUND;
            if (RT_SUCCESS(rc))
            {
                fOk = TRUE;

                /* Catches guest drags for the host; host to guest works without it. */
                g_pDnDDropTarget = VBoxDnDDropTargetCreate();
                if (g_pDnDDropTarget)
                {
                    HRESULT hr = RegisterDragDrop(g_hwndDnD, g_pDnDDropTarget);
                    if (FAILED(hr))
                    {
                        VBoxTrayLogRel("DnD: RegisterDragDrop failed, hr=%#lx; no guest to host drag and drop\n", hr);
                        g_pDnDDropTarget->lpVtbl->Release(g_pDnDDropTarget);
                        g_pDnDDropTarget = NULL;
                    }
                }
            }
            else
            {
                VBoxTrayLogRel("DnD: connecting the window thread failed, rc=%d\n", rc);
                DestroyWindow(g_hwndDnD);
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

    g_hwndDnD = NULL;
    if (g_idDnDUiClient)
    {
        VbglR3HgcmDisconnect(g_hDnDUiDevice, g_idDnDUiClient);
        g_idDnDUiClient = 0;
    }
    VbglR3Close(g_hDnDUiDevice);
    g_hDnDUiDevice = INVALID_HANDLE_VALUE;
    UnregisterClass(VBOXDND_WND_CLASS, GetModuleHandle(NULL));
    if (SUCCEEDED(hrOle))
        OleUninitialize();
    return 0;
}


/*********************************************************************************************************************************
*   Worker thread                                                                                                                *
*********************************************************************************************************************************/

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
            rc = vboxDnDRecvAction(hDevice, idClient, uMsg, &x, &y, &pEnter);
            if (RT_SUCCESS(rc) && !PostMessage(g_hwndDnD, WM_VBOXDND_ENTER, 0, (LPARAM)pEnter))
                free(pEnter);
            break;

        case HOST_DND_FN_HG_EVT_MOVE:
            rc = vboxDnDRecvAction(hDevice, idClient, uMsg, &x, &y, NULL);
            if (RT_SUCCESS(rc))
                PostMessage(g_hwndDnD, WM_VBOXDND_MOVE, x, y);
            break;

        case HOST_DND_FN_HG_EVT_DROPPED:
            rc = vboxDnDRecvAction(hDevice, idClient, uMsg, &x, &y, NULL);
            if (RT_SUCCESS(rc))
                PostMessage(g_hwndDnD, WM_VBOXDND_DROP, 0, 0);
            break;

        case HOST_DND_FN_HG_EVT_LEAVE:
        case HOST_DND_FN_CANCEL:
            rc = vboxDnDRecvSimple(hDevice, idClient, uMsg);
            PostMessage(g_hwndDnD, uMsg == HOST_DND_FN_CANCEL ? WM_VBOXDND_CANCEL : WM_VBOXDND_LEAVE, 0, 0);
            /* The host waits 3 s for an answer to the leave (upstream sends none,
               so the host reports "Leaving VM window failed (VERR_TIMEOUT)"). */
            if (RT_SUCCESS(rc) && uMsg == HOST_DND_FN_HG_EVT_LEAVE)
                vboxDnDSendAck(hDevice, idClient, VBOX_DND_ACTION_IGNORE);
            break;

        case HOST_DND_FN_HG_SND_DATA_HDR:
            rc = vboxDnDRecvData(hDevice, idClient);
            PostMessage(g_hwndDnD, rc == VERR_CANCELLED ? WM_VBOXDND_LEAVE : WM_VBOXDND_RECEIVED, RT_SUCCESS(rc), 0);
            if (rc == VERR_CANCELLED)
                rc = VINF_SUCCESS;
            break;

        case HOST_DND_FN_GH_REQ_PENDING:
            /* The window thread answers, once it knows. */
            rc = vboxDnDRecvGhPending(hDevice, idClient);
            if (RT_SUCCESS(rc))
                PostMessage(g_hwndDnD, WM_VBOXDND_GH_PENDING, 0, 0);
            break;

        case HOST_DND_FN_GH_EVT_DROPPED:
            rc = vboxDnDRecvGhDropped(hDevice, idClient, szFormat, sizeof(szFormat));
            if (RT_SUCCESS(rc))
                vboxDnDGhSend(hDevice, idClient, szFormat); /* Failures were reported to the host. */
            PostMessage(g_hwndDnD, WM_VBOXDND_GH_RESET, TRUE /* dropped on the host */, 0);
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
    InitializeCriticalSection(&g_DnDCritSect);
    g_hDnDGhDropped = CreateEvent(NULL, TRUE, FALSE, NULL);
}

/**
 * Wakes up the worker: disconnecting completes its pending
 * GUEST_DND_FN_GET_NEXT_HOST_MSG.
 */
void VBoxDnDStop(void)
{
    UINT32 idClient;
    HANDLE hDevice;

    EnterCriticalSection(&g_DnDCritSect);
    idClient = g_idDnDClient;
    hDevice  = g_hDnDDevice;
    g_idDnDClient = 0;
    LeaveCriticalSection(&g_DnDCritSect);
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
    unsigned cErrors = 0;
    unsigned idThread;
    int rc;

    /* VBoxDnDStop disconnects while a call waits, hence overlapped. */
    hDevice = VbglR3OpenEx(TRUE);
    if (hDevice == INVALID_HANDLE_VALUE)
        return 1;
    rc = vboxDnDConnect(hDevice, &idClient);
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

    EnterCriticalSection(&g_DnDCritSect);
    g_hDnDDevice  = hDevice;
    g_idDnDClient = idClient;
    LeaveCriticalSection(&g_DnDCritSect);
    VBoxTrayLogRel("DnD: connected, client %lu\n", idClient);

    while (!g_fVBoxTrayShutdown)
    {
        UINT32 uMsg = 0;
        UINT32 cParms = 0;

        rc = vboxDnDGetNextMsg(hDevice, idClient, &uMsg, &cParms);
        if (RT_SUCCESS(rc))
            rc = vboxDnDProcessHostMsg(hDevice, idClient, uMsg);
        else if (g_fVBoxTrayShutdown || rc == VERR_INVALID_HANDLE)
            break; /* Disconnected by VBoxDnDStop. */
        else if (rc == VERR_CANCELLED)
        {
            /* The host cancelled the current operation (HOST_DND_FN_CANCEL wakes waiters up this way). */
            PostMessage(g_hwndDnD, WM_VBOXDND_CANCEL, 0, 0);
            rc = VINF_SUCCESS;
        }

        if (RT_SUCCESS(rc))
            cErrors = 0;
        else
        {
            PostMessage(g_hwndDnD, WM_VBOXDND_LEAVE, 0, 0); /* Don't leave the proxy window blocking the screen. */
            if (++cErrors > VBOXDND_MAX_ERRORS)
            {
                VBoxTrayLogRel("DnD: too many failures (last rc=%d), giving up\n", rc);
                break;
            }
            Sleep(1000);
        }
    }

    PostMessage(g_hwndDnD, WM_VBOXDND_QUIT, 0, 0);
    WaitForSingleObject(hUiThread, 3000);
    CloseHandle(hUiThread);

    EnterCriticalSection(&g_DnDCritSect);
    idClient = g_idDnDClient;
    g_idDnDClient = 0;
    g_hDnDDevice  = INVALID_HANDLE_VALUE;
    LeaveCriticalSection(&g_DnDCritSect);
    if (idClient)
        VbglR3HgcmDisconnect(hDevice, idClient);
    VbglR3Close(hDevice);
    return 0;
}
