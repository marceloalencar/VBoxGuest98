/** @file
 * VBoxTray - Drag and drop from the host to the guest (files and text),
 * VBoxDragAndDropSvc HGCM protocol v3, as upstream VBoxDnD.cpp.
 *
 * Two threads, each with its own HGCM client (like upstream):
 *  - The worker (VBoxDnDThread) blocks in GUEST_DND_FN_GET_NEXT_HOST_MSG,
 *    fetches the host messages and passes them on to the proxy window.  It
 *    also receives the dropped data: files and directories are written to
 *    %TEMP%\VirtualBox Dropped Files\<time stamp>.
 *  - The proxy window thread (vboxDnDUiThread) runs the OLE drag: when the
 *    host drag enters the guest, a topmost window covers the screen and the
 *    left button is pressed on it with mouse_event; the button down starts
 *    DoDragDrop, and the pointer follows the host's moves.  On the drop, the
 *    data is requested from the host and the button released once it is
 *    here, so the drop target gets it right away (CF_HDROP for files,
 *    CF_TEXT for text).
 *
 * Only copying is offered.  Guest to host drag and drop is not implemented:
 * the host's "is a drag pending" queries are answered with "no".
 */

#include "DnD.h"
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

/** HGCM call being built: header, parameters, then the buffers. */
typedef struct VBOXDNDMSG
{
    VBOXGUEST98HGCMCALL *pCall;
    DWORD                cbCall;
    /** Offset of the next unused data byte. */
    DWORD                offNext;
} VBOXDNDMSG;

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


/*********************************************************************************************************************************
*   HGCM calls                                                                                                                   *
*********************************************************************************************************************************/

/**
 * Starts an HGCM call with cParms parameters and cbData bytes of buffers.
 */
static BOOL vboxDnDMsgInit(VBOXDNDMSG *pMsg, UINT32 idClient, UINT32 uFunction, UINT32 cParms, UINT32 cMsTimeout,
                           DWORD cbData)
{
    DWORD offData = sizeof(VBOXGUEST98HGCMCALL) + cParms * sizeof(VBOXGUEST98HGCMPARM);

    pMsg->cbCall  = offData + cbData;
    pMsg->offNext = offData;
    pMsg->pCall   = (VBOXGUEST98HGCMCALL *)calloc(1, pMsg->cbCall);
    if (!pMsg->pCall)
        return FALSE;
    pMsg->pCall->u32ClientID = idClient;
    pMsg->pCall->u32Function = uFunction;
    pMsg->pCall->cMsTimeout  = cMsTimeout;
    pMsg->pCall->cParms      = cParms;
    return TRUE;
}

static void vboxDnDMsgSetU32(VBOXDNDMSG *pMsg, UINT32 iParm, UINT32 u32)
{
    VBOXGUEST98HGCMPARM *pParm = &VBOXGUEST98_HGCM_CALL_PARMS(pMsg->pCall)[iParm];
    pParm->enmType  = VBOXGUEST98_HGCM_PARM_32BIT;
    pParm->u64Value = u32;
}

static void vboxDnDMsgSetU64(VBOXDNDMSG *pMsg, UINT32 iParm, UINT64 u64)
{
    VBOXGUEST98HGCMPARM *pParm = &VBOXGUEST98_HGCM_CALL_PARMS(pMsg->pCall)[iParm];
    pParm->enmType  = VBOXGUEST98_HGCM_PARM_64BIT;
    pParm->u64Value = u64;
}

/**
 * Sets a buffer parameter, placing the buffer in the call's data area.
 *
 * @returns The buffer in the call.
 * @param   pvIn    Data to copy in (NULL for output buffers).
 */
static BYTE *vboxDnDMsgSetBuf(VBOXDNDMSG *pMsg, UINT32 iParm, UINT32 enmType, DWORD cb, const void *pvIn)
{
    VBOXGUEST98HGCMPARM *pParm = &VBOXGUEST98_HGCM_CALL_PARMS(pMsg->pCall)[iParm];
    BYTE *pb = (BYTE *)pMsg->pCall + pMsg->offNext;

    pParm->enmType = enmType;
    pParm->cb      = cb;
    pParm->off     = pMsg->offNext;
    if (pvIn && cb)
        memcpy(pb, pvIn, cb);
    pMsg->offNext += cb;
    return pb;
}

static UINT32 vboxDnDMsgGetU32(VBOXDNDMSG *pMsg, UINT32 iParm)
{
    return (UINT32)VBOXGUEST98_HGCM_CALL_PARMS(pMsg->pCall)[iParm].u64Value;
}

static UINT64 vboxDnDMsgGetU64(VBOXDNDMSG *pMsg, UINT32 iParm)
{
    return VBOXGUEST98_HGCM_CALL_PARMS(pMsg->pCall)[iParm].u64Value;
}

/** Returns the size the host put in a buffer parameter. */
static UINT32 vboxDnDMsgGetBufSize(VBOXDNDMSG *pMsg, UINT32 iParm)
{
    return VBOXGUEST98_HGCM_CALL_PARMS(pMsg->pCall)[iParm].cb;
}

static int vboxDnDMsgCall(HANDLE hDevice, VBOXDNDMSG *pMsg)
{
    return VbglR3HgcmCall(hDevice, pMsg->pCall, pMsg->cbCall);
}

static void vboxDnDMsgFree(VBOXDNDMSG *pMsg)
{
    free(pMsg->pCall);
    pMsg->pCall = NULL;
}

/**
 * Connects to the service and does the protocol handshake (VbglR3DnDConnect).
 */
static int vboxDnDConnect(HANDLE hDevice, UINT32 *pidClient)
{
    VBOXDNDMSG Msg;
    int rc;

    rc = VbglR3HgcmConnect(hDevice, VBOX_DND_SERVICE_NAME, pidClient);
    if (RT_FAILURE(rc))
        return rc;

    if (!vboxDnDMsgInit(&Msg, *pidClient, GUEST_DND_FN_CONNECT, 3, VBOXDND_CALL_TIMEOUT_MS, 0))
        rc = VERR_NO_MEMORY;
    else
    {
        vboxDnDMsgSetU32(&Msg, 0, 0);                           /* uContext */
        vboxDnDMsgSetU32(&Msg, 1, VBOX_DND_PROTOCOL_VERSION);   /* uProtocol */
        vboxDnDMsgSetU32(&Msg, 2, 0);                           /* uFlags */
        rc = vboxDnDMsgCall(hDevice, &Msg);
        vboxDnDMsgFree(&Msg);
    }

    /* Reporting the features fails on older hosts, which is fine. */
    if (RT_SUCCESS(rc) && vboxDnDMsgInit(&Msg, *pidClient, GUEST_DND_FN_REPORT_FEATURES, 2, VBOXDND_CALL_TIMEOUT_MS, 0))
    {
        vboxDnDMsgSetU64(&Msg, 0, VBOX_DND_GF_NONE);
        vboxDnDMsgSetU64(&Msg, 1, VBOX_DND_GF_1_MUST_BE_ONE);
        vboxDnDMsgCall(hDevice, &Msg);
        vboxDnDMsgFree(&Msg);
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
 * @returns VBox status code; VERR_CANCELLED if the host wants us to quit.
 */
static int vboxDnDGetNextMsg(HANDLE hDevice, UINT32 idClient, UINT32 *puMsg, UINT32 *pcParms)
{
    VBOXDNDMSG Msg;
    int rc;

    do
    {
        if (!vboxDnDMsgInit(&Msg, idClient, GUEST_DND_FN_GET_NEXT_HOST_MSG, 3, VBOXGUEST98_WAIT_INFINITE, 0))
            return VERR_NO_MEMORY;
        vboxDnDMsgSetU32(&Msg, 0, 0);   /* uMsg */
        vboxDnDMsgSetU32(&Msg, 1, 0);   /* cParms */
        vboxDnDMsgSetU32(&Msg, 2, 1);   /* fBlock */
        rc = vboxDnDMsgCall(hDevice, &Msg);
        if (RT_SUCCESS(rc))
        {
            *puMsg   = vboxDnDMsgGetU32(&Msg, 0);
            *pcParms = vboxDnDMsgGetU32(&Msg, 1);
        }
        vboxDnDMsgFree(&Msg);
    } while (rc == VERR_INTERRUPTED && !g_fVBoxTrayShutdown);
    return rc;
}

/**
 * Fetches a message that only has the context ID (leave, cancel).
 */
static int vboxDnDRecvSimple(HANDLE hDevice, UINT32 idClient, UINT32 uMsg)
{
    VBOXDNDMSG Msg;
    int rc;

    if (!vboxDnDMsgInit(&Msg, idClient, uMsg, 1, VBOXDND_CALL_TIMEOUT_MS, 0))
        return VERR_NO_MEMORY;
    vboxDnDMsgSetU32(&Msg, 0, 0);
    rc = vboxDnDMsgCall(hDevice, &Msg);
    vboxDnDMsgFree(&Msg);
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
    VBOXDNDMSG Msg;
    BYTE *pbFormats;
    UINT32 cbFormats;
    int rc;

    if (!vboxDnDMsgInit(&Msg, idClient, uMsg, 8, VBOXDND_CALL_TIMEOUT_MS, VBOX_DND_CHUNK_SIZE))
        return VERR_NO_MEMORY;
    vboxDnDMsgSetU32(&Msg, 0, 0);   /* uContext */
    vboxDnDMsgSetU32(&Msg, 1, 0);   /* uScreenId */
    vboxDnDMsgSetU32(&Msg, 2, 0);   /* uX */
    vboxDnDMsgSetU32(&Msg, 3, 0);   /* uY */
    vboxDnDMsgSetU32(&Msg, 4, 0);   /* uDefAction */
    vboxDnDMsgSetU32(&Msg, 5, 0);   /* uAllActions */
    pbFormats = vboxDnDMsgSetBuf(&Msg, 6, VBOXGUEST98_HGCM_PARM_BUFFER_OUT, VBOX_DND_CHUNK_SIZE, NULL);
    vboxDnDMsgSetU32(&Msg, 7, 0);   /* cbFormats */

    rc = vboxDnDMsgCall(hDevice, &Msg);
    if (RT_SUCCESS(rc))
    {
        *px = vboxDnDMsgGetU32(&Msg, 2);
        *py = vboxDnDMsgGetU32(&Msg, 3);
        if (ppEnter)
        {
            cbFormats = vboxDnDMsgGetBufSize(&Msg, 6);
            if (cbFormats > VBOX_DND_CHUNK_SIZE)
                cbFormats = VBOX_DND_CHUNK_SIZE;
            *ppEnter = (VBOXDNDENTER *)malloc(sizeof(VBOXDNDENTER) + cbFormats);
            if (*ppEnter)
            {
                (*ppEnter)->x           = *px;
                (*ppEnter)->y           = *py;
                (*ppEnter)->fAllActions = vboxDnDMsgGetU32(&Msg, 5);
                memcpy((*ppEnter)->szFormats, pbFormats, cbFormats);
                (*ppEnter)->szFormats[cbFormats] = '\0';
            }
            else
                rc = VERR_NO_MEMORY;
        }
    }
    vboxDnDMsgFree(&Msg);
    return rc;
}

/**
 * Tells the host the action the guest would do at the current position.
 */
static int vboxDnDSendAck(HANDLE hDevice, UINT32 idClient, UINT32 uAction)
{
    VBOXDNDMSG Msg;
    int rc;

    if (!vboxDnDMsgInit(&Msg, idClient, GUEST_DND_FN_HG_ACK_OP, 2, VBOXDND_CALL_TIMEOUT_MS, 0))
        return VERR_NO_MEMORY;
    vboxDnDMsgSetU32(&Msg, 0, 0);
    vboxDnDMsgSetU32(&Msg, 1, uAction);
    rc = vboxDnDMsgCall(hDevice, &Msg);
    vboxDnDMsgFree(&Msg);
    return rc;
}

/**
 * Asks the host for the dropped data in a format.
 */
static int vboxDnDSendReqData(HANDLE hDevice, UINT32 idClient, const char *pszFormat)
{
    DWORD cbFormat = lstrlen(pszFormat) + 1;
    VBOXDNDMSG Msg;
    int rc;

    if (!vboxDnDMsgInit(&Msg, idClient, GUEST_DND_FN_HG_REQ_DATA, 3, VBOXDND_CALL_TIMEOUT_MS, cbFormat))
        return VERR_NO_MEMORY;
    vboxDnDMsgSetU32(&Msg, 0, 0);
    vboxDnDMsgSetBuf(&Msg, 1, VBOXGUEST98_HGCM_PARM_BUFFER_IN, cbFormat, pszFormat);
    vboxDnDMsgSetU32(&Msg, 2, cbFormat);
    rc = vboxDnDMsgCall(hDevice, &Msg);
    vboxDnDMsgFree(&Msg);
    return rc;
}

/**
 * Reports the transfer status to the host.
 */
static int vboxDnDSendProgress(HANDLE hDevice, UINT32 idClient, UINT32 uStatus, UINT32 uPercent, int rcErr)
{
    VBOXDNDMSG Msg;
    int rc;

    if (!vboxDnDMsgInit(&Msg, idClient, GUEST_DND_FN_HG_EVT_PROGRESS, 4, VBOXDND_CALL_TIMEOUT_MS, 0))
        return VERR_NO_MEMORY;
    vboxDnDMsgSetU32(&Msg, 0, 0);
    vboxDnDMsgSetU32(&Msg, 1, uStatus);
    vboxDnDMsgSetU32(&Msg, 2, uPercent);
    vboxDnDMsgSetU32(&Msg, 3, (UINT32)rcErr);
    rc = vboxDnDMsgCall(hDevice, &Msg);
    vboxDnDMsgFree(&Msg);
    return rc;
}

/**
 * Answers the host's "is a guest drag pending" query with no.
 */
static int vboxDnDAnswerPending(HANDLE hDevice, UINT32 idClient)
{
    static const char s_szNoFormats[] = "";
    VBOXDNDMSG Msg;
    int rc;

    /* Fetch the query (uContext, uScreenId). */
    if (!vboxDnDMsgInit(&Msg, idClient, HOST_DND_FN_GH_REQ_PENDING, 2, VBOXDND_CALL_TIMEOUT_MS, 0))
        return VERR_NO_MEMORY;
    vboxDnDMsgSetU32(&Msg, 0, 0);
    vboxDnDMsgSetU32(&Msg, 1, 0);
    rc = vboxDnDMsgCall(hDevice, &Msg);
    vboxDnDMsgFree(&Msg);
    if (RT_FAILURE(rc))
        return rc;

    if (!vboxDnDMsgInit(&Msg, idClient, GUEST_DND_FN_GH_ACK_PENDING, 5, VBOXDND_CALL_TIMEOUT_MS, sizeof(s_szNoFormats)))
        return VERR_NO_MEMORY;
    vboxDnDMsgSetU32(&Msg, 0, 0);                           /* uContext */
    vboxDnDMsgSetU32(&Msg, 1, VBOX_DND_ACTION_IGNORE);      /* uDefAction */
    vboxDnDMsgSetU32(&Msg, 2, VBOX_DND_ACTION_IGNORE);      /* uAllActions */
    vboxDnDMsgSetBuf(&Msg, 3, VBOXGUEST98_HGCM_PARM_BUFFER_IN, sizeof(s_szNoFormats), s_szNoFormats);
    vboxDnDMsgSetU32(&Msg, 4, sizeof(s_szNoFormats));       /* cbFormats */
    rc = vboxDnDMsgCall(hDevice, &Msg);
    vboxDnDMsgFree(&Msg);
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
    VBOXDNDMSG Msg;
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
            if (!vboxDnDMsgInit(&Msg, idClient, uMsg, 4, VBOXDND_CALL_TIMEOUT_MS, MAX_PATH * 4))
            {
                rc = VERR_NO_MEMORY;
                break;
            }
            vboxDnDMsgSetU32(&Msg, 0, 0);                                                       /* uContext */
            pb = vboxDnDMsgSetBuf(&Msg, 1, VBOXGUEST98_HGCM_PARM_BUFFER_OUT, MAX_PATH * 4, NULL); /* pvName */
            vboxDnDMsgSetU32(&Msg, 2, MAX_PATH * 4);                                            /* cbName */
            vboxDnDMsgSetU32(&Msg, 3, 0);                                                       /* fMode */
            rc = vboxDnDMsgCall(hDevice, &Msg);
            if (RT_SUCCESS(rc))
            {
                cb = vboxDnDMsgGetBufSize(&Msg, 1);
                if (cb > MAX_PATH * 4)
                    rc = VERR_TOO_MUCH_DATA;
                else
                    rc = vboxDnDBuildPath(pszDropDir, (const char *)pb, vboxDnDStrNLen(pb, cb), szPath);
                if (RT_SUCCESS(rc) && !CreateDirectory(szPath, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
                    rc = VERR_OPEN_FAILED;
                if (RT_SUCCESS(rc) && cObjects)
                    cObjects--;
            }
            vboxDnDMsgFree(&Msg);
        }
        else if (uMsg == HOST_DND_FN_HG_SND_FILE_HDR)
        {
            if (hFile != INVALID_HANDLE_VALUE)
            {
                rc = VERR_WRONG_ORDER; /* The previous file isn't complete. */
                break;
            }
            if (!vboxDnDMsgInit(&Msg, idClient, uMsg, 6, VBOXDND_CALL_TIMEOUT_MS, MAX_PATH * 4))
            {
                rc = VERR_NO_MEMORY;
                break;
            }
            vboxDnDMsgSetU32(&Msg, 0, 0);                                                       /* uContext */
            pb = vboxDnDMsgSetBuf(&Msg, 1, VBOXGUEST98_HGCM_PARM_BUFFER_OUT, MAX_PATH * 4, NULL); /* pvName */
            vboxDnDMsgSetU32(&Msg, 2, MAX_PATH * 4);                                            /* cbName */
            vboxDnDMsgSetU32(&Msg, 3, 0);                                                       /* uFlags */
            vboxDnDMsgSetU32(&Msg, 4, 0);                                                       /* fMode */
            vboxDnDMsgSetU64(&Msg, 5, 0);                                                       /* cbTotal */
            rc = vboxDnDMsgCall(hDevice, &Msg);
            if (RT_SUCCESS(rc))
            {
                cb = vboxDnDMsgGetBufSize(&Msg, 1);
                cbFileSize = vboxDnDMsgGetU64(&Msg, 5);
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
            vboxDnDMsgFree(&Msg);
        }
        else if (uMsg == HOST_DND_FN_HG_SND_FILE_DATA)
        {
            if (hFile == INVALID_HANDLE_VALUE)
            {
                rc = VERR_WRONG_ORDER;
                break;
            }
            if (!vboxDnDMsgInit(&Msg, idClient, uMsg, 5, VBOXDND_CALL_TIMEOUT_MS, VBOX_DND_CHUNK_SIZE))
            {
                rc = VERR_NO_MEMORY;
                break;
            }
            vboxDnDMsgSetU32(&Msg, 0, 0);                                                           /* uContext */
            pb = vboxDnDMsgSetBuf(&Msg, 1, VBOXGUEST98_HGCM_PARM_BUFFER_OUT, VBOX_DND_CHUNK_SIZE, NULL); /* pvData */
            vboxDnDMsgSetU32(&Msg, 2, 0);                                                           /* cbData */
            vboxDnDMsgSetBuf(&Msg, 3, VBOXGUEST98_HGCM_PARM_BUFFER_OUT, 0, NULL);                   /* pvChecksum */
            vboxDnDMsgSetU32(&Msg, 4, 0);                                                           /* cbChecksum */
            rc = vboxDnDMsgCall(hDevice, &Msg);
            if (RT_SUCCESS(rc))
            {
                DWORD cbWritten = 0;
                cb = vboxDnDMsgGetU32(&Msg, 2);
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
            vboxDnDMsgFree(&Msg);
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
    VBOXDNDMSG Msg;
    BYTE *pb;
    int rc;

    /*
     * The header.
     */
    if (!vboxDnDMsgInit(&Msg, idClient, HOST_DND_FN_HG_SND_DATA_HDR, 12, VBOXDND_CALL_TIMEOUT_MS, VBOX_DND_CHUNK_SIZE))
        return VERR_NO_MEMORY;
    vboxDnDMsgSetU32(&Msg, 0, 0);                                                           /* uContext */
    vboxDnDMsgSetU32(&Msg, 1, 0);                                                           /* uFlags */
    vboxDnDMsgSetU32(&Msg, 2, 0);                                                           /* uScreenId */
    vboxDnDMsgSetU64(&Msg, 3, 0);                                                           /* cbTotal */
    vboxDnDMsgSetU32(&Msg, 4, 0);                                                           /* cbMeta */
    pb = vboxDnDMsgSetBuf(&Msg, 5, VBOXGUEST98_HGCM_PARM_BUFFER_OUT, VBOX_DND_CHUNK_SIZE, NULL); /* pvMetaFmt */
    vboxDnDMsgSetU32(&Msg, 6, 0);                                                           /* cbMetaFmt */
    vboxDnDMsgSetU64(&Msg, 7, 0);                                                           /* cObjects */
    vboxDnDMsgSetU32(&Msg, 8, 0);                                                           /* enmCompression */
    vboxDnDMsgSetU32(&Msg, 9, 0);                                                           /* enmChecksumType */
    vboxDnDMsgSetBuf(&Msg, 10, VBOXGUEST98_HGCM_PARM_BUFFER_OUT, 0, NULL);                  /* pvChecksum */
    vboxDnDMsgSetU32(&Msg, 11, 0);                                                          /* cbChecksum */
    rc = vboxDnDMsgCall(hDevice, &Msg);
    if (RT_SUCCESS(rc))
    {
        cbTotal  = vboxDnDMsgGetU64(&Msg, 3);
        cbMeta   = vboxDnDMsgGetU32(&Msg, 4);
        cObjects = vboxDnDMsgGetU64(&Msg, 7);
        pb[VBOX_DND_CHUNK_SIZE - 1] = '\0';
        pszMetaFmt = _strdup((const char *)pb);
        if (!pszMetaFmt)
            rc = VERR_NO_MEMORY;
        else if (cbMeta > VBOXDND_MAX_META_SIZE || cbMeta > cbTotal)
            rc = VERR_TOO_MUCH_DATA;
    }
    vboxDnDMsgFree(&Msg);

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

        if (!vboxDnDMsgInit(&Msg, idClient, HOST_DND_FN_HG_SND_DATA, 5, VBOXDND_CALL_TIMEOUT_MS, cbChunk))
        {
            rc = VERR_NO_MEMORY;
            break;
        }
        vboxDnDMsgSetU32(&Msg, 0, 0);                                                       /* uContext */
        pb = vboxDnDMsgSetBuf(&Msg, 1, VBOXGUEST98_HGCM_PARM_BUFFER_OUT, cbChunk, NULL);    /* pvData */
        vboxDnDMsgSetU32(&Msg, 2, 0);                                                       /* cbData */
        vboxDnDMsgSetBuf(&Msg, 3, VBOXGUEST98_HGCM_PARM_BUFFER_OUT, 0, NULL);               /* pvChecksum */
        vboxDnDMsgSetU32(&Msg, 4, 0);                                                       /* cbChecksum */
        rc = vboxDnDMsgCall(hDevice, &Msg);
        if (RT_SUCCESS(rc))
        {
            cbGot = vboxDnDMsgGetU32(&Msg, 2);
            if (!cbGot || cbGot > cbChunk)
                rc = VERR_TOO_MUCH_DATA;
            else
            {
                memcpy(pbMeta + cbMetaRecv, pb, cbGot);
                cbMetaRecv += cbGot;
            }
        }
        vboxDnDMsgFree(&Msg);
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
*   Proxy window (window thread)                                                                                                 *
*********************************************************************************************************************************/

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

        case WM_VBOXDND_QUIT:
            free(g_pDnDPendingEnter);
            g_pDnDPendingEnter = NULL;
            vboxDnDAbort();
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
                fOk = TRUE;
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
    UINT32 x = 0;
    UINT32 y = 0;
    int rc;

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
            PostMessage(g_hwndDnD, WM_VBOXDND_LEAVE, 0, 0);
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
            rc = vboxDnDAnswerPending(hDevice, idClient);
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
            VBoxTrayLogRel("DnD: the host asked to quit\n");
            break;
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
