/** @file
 * VBoxTray - Shared clipboard (text, images, HTML).
 *
 * Like upstream (VbglR3ClipboardConnectEx), the guest reports
 * VBOX_SHCL_GF_0_CONTEXT_ID: with a host that has it too the messages come
 * with VBOX_SHCL_GUEST_FN_MSG_PEEK_WAIT + MSG_GET and the data the host
 * reads is answered with its context ID; otherwise the legacy protocol
 * (VBOX_SHCL_GUEST_FN_MSG_OLD_GET_WAIT) is used.  With a host that also
 * does file transfers (VBOX_SHCL_GF_0_TRANSFERS), files copied on one side
 * can be pasted on the other (ClipFile.c).
 *
 * Two threads share one HGCM client (and therefore one overlapped handle):
 *  - The worker thread (VBoxClipboardThread) waits for the host messages
 *    and handles them.
 *  - The VBoxTray window thread is in the clipboard viewer chain and reports
 *    guest clipboard changes to the host (VBoxClipboardWndProc).
 * The guest clipboard itself is only touched by the window thread; the
 * worker asks it with messages.
 *
 * Host content uses delayed rendering, like upstream: the formats the host
 * reports are announced on the guest clipboard without data, and the data
 * is fetched from the host when an application pastes (WM_RENDERFORMAT, on
 * the window thread, while the application waits).
 *
 * Windows 98 applications use CF_TEXT (ANSI); the host speaks UTF-16, so the
 * text is converted with the active code page.  CF_UNICODETEXT is also
 * offered for the few Unicode aware applications.
 *
 * Images travel as packed DIBs (BITMAPINFOHEADER, colors, bits) both ways,
 * the guest side being CF_DIB; Windows converts to and from CF_BITMAP for
 * the applications that use that.
 */

#include "DnD.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/

/** Worker -> window: the HGCM client is connected, report the guest formats. */
#define WM_VBOXCLIP_CONNECTED       (WM_APP + 10)
/** Worker -> window: announce host formats on the guest clipboard.
 *  wParam: the formats (VBOX_SHCL_FMT_XXX). */
#define WM_VBOXCLIP_ANNOUNCE        (WM_APP + 11)
/** Worker -> window: get guest clipboard data.  wParam: the format
 *  (one VBOX_SHCL_FMT_XXX bit), lParam: VBOXCLIPDATA to fill. */
#define WM_VBOXCLIP_GET             (WM_APP + 12)
/** Worker -> window: get the paths of the files copied in the guest.
 *  lParam: char * to set (VBoxClipboardGetFiles). */
#define WM_VBOXCLIP_GET_FILES       (WM_APP + 13)

/** First guess for the size of the host data. */
#define VBOXCLIP_READ_SIZE          16384
/** Timeout for the calls that don't wait for host messages (ms). */
#define VBOXCLIP_CALL_TIMEOUT_MS    10000
/** Name of the Windows HTML clipboard format. */
#define VBOXCLIP_CF_HTML_NAME       "HTML Format"
/** Attempts and pause between them when another application has the
 *  clipboard open. */
#define VBOXCLIP_OPEN_TRIES         10
#define VBOXCLIP_OPEN_WAIT_MS       20


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/

/** Guest data for the host (WM_VBOXCLIP_GET), in the host's layout (UTF-16
 *  text with its terminator, a packed DIB, or a UTF-8 HTML fragment with its
 *  terminator); pv is malloc'ed by the window thread. */
typedef struct VBOXCLIPDATA
{
    void *pv;
    /** Size in bytes. */
    DWORD cb;
} VBOXCLIPDATA;


/*********************************************************************************************************************************
*   Global Variables                                                                                                             *
*********************************************************************************************************************************/

/** Protects g_hClipDevice and g_idClipClient. */
static CRITICAL_SECTION g_ClipCritSect;
/** The handle the HGCM client belongs to. */
static HANDLE g_hClipDevice = INVALID_HANDLE_VALUE;
/** The HGCM client ID, 0 if not connected. */
static UINT32 g_idClipClient = 0;
/** Both sides have VBOX_SHCL_GF_0_CONTEXT_ID: the new protocol is used. */
static BOOL g_fClipContextId = FALSE;
/** Next window in the clipboard viewer chain (window thread only). */
static HWND g_hwndNextViewer = NULL;
/** The "HTML Format" (CF_HTML) clipboard format. */
static UINT g_cfHtml = 0;


/*********************************************************************************************************************************
*   HGCM calls                                                                                                                   *
*********************************************************************************************************************************/

/**
 * Reports our features and gets the host's (VBOX_SHCL_GUEST_FN_REPORT_FEATURES).
 *
 * @returns The host features, 0 for an older host.
 */
static UINT64 vboxClipReportFeatures(HANDLE hDevice, UINT32 idClient)
{
    VBGLR3HGCMMSG Msg;
    UINT64 fHostFeatures = 0;
    int rc;

    if (!VbglR3HgcmMsgInit(&Msg, idClient, VBOX_SHCL_GUEST_FN_REPORT_FEATURES, 2, VBOXCLIP_CALL_TIMEOUT_MS, 0))
        return 0;
    VbglR3HgcmMsgSetU64(&Msg, 0, VBOX_SHCL_GF_0_CONTEXT_ID | VBOX_SHCL_GF_0_TRANSFERS);
    VbglR3HgcmMsgSetU64(&Msg, 1, VBOX_SHCL_GF_1_MUST_BE_ONE);
    rc = VbglR3HgcmMsgCall(hDevice, &Msg);
    if (RT_SUCCESS(rc) && !(VbglR3HgcmMsgGetU64(&Msg, 1) & VBOX_SHCL_GF_1_MUST_BE_ONE))
        fHostFeatures = VbglR3HgcmMsgGetU64(&Msg, 0);
    VbglR3HgcmMsgFree(&Msg);
    return fHostFeatures;
}

/**
 * Makes a call without parameters (VBOX_SHCL_GUEST_FN_MSG_CANCEL).
 */
static void vboxClipCancel(HANDLE hDevice, UINT32 idClient)
{
    VBGLR3HGCMMSG Msg;

    if (!VbglR3HgcmMsgInit(&Msg, idClient, VBOX_SHCL_GUEST_FN_MSG_CANCEL, 0, VBOXCLIP_CALL_TIMEOUT_MS, 0))
        return;
    VbglR3HgcmMsgCall(hDevice, &Msg);
    VbglR3HgcmMsgFree(&Msg);
}

/**
 * Fetches a peeked message whose parameters are [0] the message ID in and
 * a 32 or 64-bit value out, [1] a 32-bit value out (MSG_GET).
 */
static int vboxClipFetch(HANDLE hDevice, UINT32 idClient, UINT32 uMsg, BOOL f64, UINT64 *pu64, UINT32 *pu32)
{
    VBGLR3HGCMMSG Msg;
    int rc;

    if (!VbglR3HgcmMsgInit(&Msg, idClient, VBOX_SHCL_GUEST_FN_MSG_GET, 2, VBOXCLIP_CALL_TIMEOUT_MS, 0))
        return VERR_NO_MEMORY;
    if (f64)
        VbglR3HgcmMsgSetU64(&Msg, 0, uMsg);
    else
        VbglR3HgcmMsgSetU32(&Msg, 0, uMsg);
    VbglR3HgcmMsgSetU32(&Msg, 1, 0);
    do
        rc = VbglR3HgcmMsgCall(hDevice, &Msg);
    while (rc == VERR_INTERRUPTED);
    if (RT_SUCCESS(rc))
    {
        *pu64 = VbglR3HgcmMsgGetU64(&Msg, 0);
        *pu32 = VbglR3HgcmMsgGetU32(&Msg, 1);
    }
    VbglR3HgcmMsgFree(&Msg);
    return rc;
}

/**
 * Waits for the next host message.
 *
 * @returns VBox status code.
 * @param   puMsg       Where to return the message (VBOX_SHCL_HOST_MSG_XXX).
 * @param   pfFormats   Where to return its formats or format.
 * @param   pidContext  Where to return the context ID of a READ_DATA_CID
 *                      (0 otherwise), for the answer.
 */
static int vboxClipGetHostMsg(HANDLE hDevice, UINT32 idClient, UINT32 *puMsg, UINT32 *pfFormats, UINT64 *pidContext)
{
    VBGLR3HGCMMSG Msg;
    UINT64 u64 = 0;
    UINT32 uMsg;
    int rc;

    *pidContext = 0;
    *pfFormats  = 0;
    if (!g_fClipContextId)
    {
        if (!VbglR3HgcmMsgInit(&Msg, idClient, VBOX_SHCL_GUEST_FN_MSG_OLD_GET_WAIT, 2, VBOXGUEST98_WAIT_INFINITE, 0))
            return VERR_NO_MEMORY;
        VbglR3HgcmMsgSetU32(&Msg, 0, 0);
        VbglR3HgcmMsgSetU32(&Msg, 1, 0);

        rc = VbglR3HgcmMsgCall(hDevice, &Msg);
        if (RT_SUCCESS(rc))
        {
            *puMsg     = VbglR3HgcmMsgGetU32(&Msg, 0);
            *pfFormats = VbglR3HgcmMsgGetU32(&Msg, 1);
        }
        VbglR3HgcmMsgFree(&Msg);
        return rc;
    }

    /* The new protocol: peek, then fetch what the message carries. */
    if (!VbglR3HgcmMsgInit(&Msg, idClient, VBOX_SHCL_GUEST_FN_MSG_PEEK_WAIT, 2, VBOXGUEST98_WAIT_INFINITE, 0))
        return VERR_NO_MEMORY;
    VbglR3HgcmMsgSetU64(&Msg, 0, 0);
    VbglR3HgcmMsgSetU32(&Msg, 1, 0);
    rc = VbglR3HgcmMsgCall(hDevice, &Msg);
    uMsg = (UINT32)VbglR3HgcmMsgGetU64(&Msg, 0);
    VbglR3HgcmMsgFree(&Msg);
    if (rc == VERR_INTERRUPTED)
        vboxClipCancel(hDevice, idClient); /* Like upstream, or the next wait gets VERR_RESOURCE_BUSY. */
    if (RT_FAILURE(rc))
        return rc;

    *puMsg = uMsg;
    switch (uMsg)
    {
        case VBOX_SHCL_HOST_MSG_FORMATS_REPORT:
            /* Upstream sets parameter 0 as 32-bit here (vbglR3ClipboardFormatsReportRecv). */
            rc = vboxClipFetch(hDevice, idClient, uMsg, FALSE, &u64, pfFormats);
            break;
        case VBOX_SHCL_HOST_MSG_READ_DATA_CID:
            rc = vboxClipFetch(hDevice, idClient, uMsg, TRUE, pidContext, pfFormats);
            break;
        case VBOX_SHCL_HOST_MSG_READ_DATA:
            rc = vboxClipFetch(hDevice, idClient, uMsg, FALSE, &u64, pfFormats);
            break;
        case VBOX_SHCL_HOST_MSG_TRANSFER_STATUS:
            VBoxClipFileOnStatus(hDevice, idClient);
            break;
        case VBOX_SHCL_HOST_MSG_TRANSFER_ROOT_LIST_HDR_READ:
        case VBOX_SHCL_HOST_MSG_TRANSFER_ROOT_LIST_ENTRY_READ:
        case VBOX_SHCL_HOST_MSG_TRANSFER_LIST_OPEN:
        case VBOX_SHCL_HOST_MSG_TRANSFER_LIST_CLOSE:
        case VBOX_SHCL_HOST_MSG_TRANSFER_LIST_HDR_READ:
        case VBOX_SHCL_HOST_MSG_TRANSFER_LIST_ENTRY_READ:
        case VBOX_SHCL_HOST_MSG_TRANSFER_OBJ_OPEN:
        case VBOX_SHCL_HOST_MSG_TRANSFER_OBJ_CLOSE:
        case VBOX_SHCL_HOST_MSG_TRANSFER_OBJ_READ:
            rc = VBoxClipFileOnHostMsg(hDevice, idClient, uMsg);
            break;
        default:
            /* QUIT and CANCELED carry nothing to fetch. */
            break;
    }
    return rc;
}

/**
 * Tells the host which formats the guest clipboard has.
 */
static int vboxClipReportFormats(HANDLE hDevice, UINT32 idClient, UINT32 fFormats)
{
    VBGLR3HGCMMSG Msg;
    int rc;

    if (!VbglR3HgcmMsgInit(&Msg, idClient, VBOX_SHCL_GUEST_FN_REPORT_FORMATS, 1, VBOXCLIP_CALL_TIMEOUT_MS, 0))
        return VERR_NO_MEMORY;
    VbglR3HgcmMsgSetU32(&Msg, 0, fFormats);

    rc = VbglR3HgcmMsgCall(hDevice, &Msg);
    VbglR3HgcmMsgFree(&Msg);
    return rc;
}

/**
 * Reads the host clipboard data in the given format.
 *
 * @returns VBox status code.
 * @param   ppv     Where to return the data (malloc'ed, followed by two
 *                  zero bytes so text is always terminated).
 * @param   pcb     Where to return the size of the data.
 */
static int vboxClipReadData(HANDLE hDevice, UINT32 idClient, UINT32 fFormat, void **ppv, UINT32 *pcb)
{
    VBGLR3HGCMMSG Msg;
    BYTE *pbBuf;
    UINT32 cbBuf = VBOXCLIP_READ_SIZE;
    UINT32 cbActual;
    int iTry;
    int rc = VERR_BUFFER_OVERFLOW;

    *ppv = NULL;
    *pcb = 0;
    for (iTry = 0; iTry < 2; iTry++)
    {
        if (!VbglR3HgcmMsgInit(&Msg, idClient, VBOX_SHCL_GUEST_FN_DATA_READ, 3, VBOXCLIP_CALL_TIMEOUT_MS, cbBuf))
            return VERR_NO_MEMORY;
        VbglR3HgcmMsgSetU32(&Msg, 0, fFormat);
        pbBuf = VbglR3HgcmMsgSetBuf(&Msg, 1, VBOXGUEST98_HGCM_PARM_BUFFER_OUT, cbBuf, NULL);
        VbglR3HgcmMsgSetU32(&Msg, 2, 0);

        rc = VbglR3HgcmMsgCall(hDevice, &Msg);
        cbActual = VbglR3HgcmMsgGetU32(&Msg, 2);
        if (RT_SUCCESS(rc) && rc != VINF_BUFFER_OVERFLOW && cbActual <= cbBuf)
        {
            *ppv = malloc(cbActual + sizeof(WCHAR));
            if (*ppv)
            {
                memcpy(*ppv, pbBuf, cbActual);
                memset((BYTE *)*ppv + cbActual, 0, sizeof(WCHAR));
                *pcb = cbActual;
            }
            else
                rc = VERR_NO_MEMORY;
            VbglR3HgcmMsgFree(&Msg);
            return rc;
        }
        VbglR3HgcmMsgFree(&Msg);
        if (RT_FAILURE(rc))
            return rc;

        /* Too small: retry once with the size the host asked for, plus one, since
           the host also reports VINF_BUFFER_OVERFLOW for a buffer of exactly the
           data's size (its check is cbActual >= cbData). */
        if (cbActual >= VBOXGUEST98_MAX_HGCM_DATA_SIZE)
            return VERR_TOO_MUCH_DATA;
        cbBuf = cbActual + 1;
        rc = VERR_BUFFER_OVERFLOW;
    }
    return rc;
}

/**
 * Sends guest clipboard data to the host (answer to
 * VBOX_SHCL_HOST_MSG_READ_DATA or _READ_DATA_CID).
 *
 * @param   idContext   The context ID of a READ_DATA_CID.
 */
static int vboxClipWriteData(HANDLE hDevice, UINT32 idClient, UINT64 idContext, UINT32 fFormat, const void *pv,
                             UINT32 cb)
{
    VBGLR3HGCMMSG Msg;
    UINT32 iParm = 0;
    int rc;

    if (cb > VBOXGUEST98_MAX_HGCM_DATA_SIZE)
        cb = 0; /* Too big for the driver, send nothing rather than leaving the host waiting. */

    if (!VbglR3HgcmMsgInit(&Msg, idClient, VBOX_SHCL_GUEST_FN_DATA_WRITE, g_fClipContextId ? 3 : 2,
                           VBOXCLIP_CALL_TIMEOUT_MS, cb))
        return VERR_NO_MEMORY;
    if (g_fClipContextId)
        VbglR3HgcmMsgSetU64(&Msg, iParm++, idContext);
    VbglR3HgcmMsgSetU32(&Msg, iParm++, fFormat);
    VbglR3HgcmMsgSetBuf(&Msg, iParm, VBOXGUEST98_HGCM_PARM_BUFFER_IN, cb, pv);

    rc = VbglR3HgcmMsgCall(hDevice, &Msg);
    VbglR3HgcmMsgFree(&Msg);
    return rc;
}


/*********************************************************************************************************************************
*   Guest clipboard (window thread)                                                                                              *
*********************************************************************************************************************************/

/**
 * Returns the formats the guest clipboard can provide to the host.
 */
static UINT32 vboxClipGuestFormats(void)
{
    UINT32 fFormats = VBOX_SHCL_FMT_NONE;

    if (   IsClipboardFormatAvailable(CF_TEXT)
        || IsClipboardFormatAvailable(CF_OEMTEXT)
        || IsClipboardFormatAvailable(CF_UNICODETEXT))
        fFormats |= VBOX_SHCL_FMT_UNICODETEXT;
    if (IsClipboardFormatAvailable(CF_DIB) || IsClipboardFormatAvailable(CF_BITMAP))
        fFormats |= VBOX_SHCL_FMT_BITMAP;
    if (g_cfHtml && IsClipboardFormatAvailable(g_cfHtml))
        fFormats |= VBOX_SHCL_FMT_HTML;
    if (IsClipboardFormatAvailable(CF_HDROP) && VBoxClipFileEnabled())
        fFormats |= VBOX_SHCL_FMT_URI_LIST;
    return fFormats;
}

/**
 * Returns the value of a CF_HTML header field ("StartFragment:" etc.), -1
 * if it is missing.
 */
static long vboxClipCfHtmlValue(const char *pszCfHtml, const char *pszField)
{
    const char *psz = strstr(pszCfHtml, pszField);
    if (!psz)
        return -1;
    psz += strlen(pszField);
    if (*psz < '0' || *psz > '9')
        return -1;
    return strtol(psz, NULL, 10);
}

/**
 * Checks whether text is CF_HTML (has the header) rather than plain HTML
 * (ShClWinIsCFHTML).
 */
static BOOL vboxClipIsCfHtml(const char *psz)
{
    return strstr(psz, "Version:") && strstr(psz, "StartHTML:");
}

/**
 * Returns the size of a packed DIB, from its header.
 *
 * @returns Size in bytes, 0 if the header makes no sense.
 * @param   pHdr    The DIB.
 * @param   cbMax   The size of the memory it is in.
 */
static DWORD vboxClipDibSize(const BITMAPINFOHEADER *pHdr, DWORD cbMax)
{
    UINT64 cbHdr;
    UINT64 cbBits;
    DWORD cColors;

    if (   cbMax < sizeof(BITMAPINFOHEADER)
        || pHdr->biSize < sizeof(BITMAPINFOHEADER) /* Not the OS/2 BITMAPCOREHEADER. */
        || pHdr->biSize > cbMax
        || pHdr->biWidth <= 0
        || pHdr->biHeight == 0
        || pHdr->biPlanes != 1
        || pHdr->biBitCount == 0
        || pHdr->biBitCount > 32)
        return 0;

    cbHdr = pHdr->biSize;
    if (pHdr->biSize == sizeof(BITMAPINFOHEADER) && pHdr->biCompression == BI_BITFIELDS)
        cbHdr += 3 * sizeof(DWORD); /* The color masks follow the header. */
    cColors = pHdr->biClrUsed;
    if (!cColors && pHdr->biBitCount <= 8)
        cColors = 1U << pHdr->biBitCount;
    cbHdr += (UINT64)cColors * sizeof(RGBQUAD);

    if (pHdr->biCompression == BI_RGB || pHdr->biCompression == BI_BITFIELDS)
        cbBits = (((UINT64)pHdr->biWidth * pHdr->biBitCount + 31) / 32) * 4
               * (UINT64)(pHdr->biHeight < 0 ? -pHdr->biHeight : pHdr->biHeight);
    else
        cbBits = pHdr->biSizeImage; /* RLE. */

    if (cbHdr + cbBits > cbMax)
        return 0;
    return (DWORD)(cbHdr + cbBits);
}

/**
 * Reports the guest clipboard formats to the host, unless we put the current
 * content there ourselves (which would echo the host's own data back).
 */
static void vboxClipReportGuestFormats(HWND hwnd)
{
    if (GetClipboardOwner() == hwnd || VBoxClipFileIsCurrent())
        return;
    VBoxClipFileForget();

    EnterCriticalSection(&g_ClipCritSect);
    if (g_idClipClient)
        vboxClipReportFormats(g_hClipDevice, g_idClipClient, vboxClipGuestFormats());
    LeaveCriticalSection(&g_ClipCritSect);
}

/**
 * Gets the guest clipboard text as UTF-16.  The clipboard is open.
 *
 * @param   pData   Where to return the text (pv stays NULL if there is none).
 */
static void vboxClipGetGuestText(VBOXCLIPDATA *pData)
{
    WCHAR *pwszRet;
    HANDLE hMem;

    hMem = GetClipboardData(CF_TEXT);
    if (hMem)
    {
        /* Convert ANSI -> UTF-16, never reading past the memory block. */
        const char *psz = (const char *)GlobalLock(hMem);
        if (psz)
        {
            DWORD cbMax = GlobalSize(hMem);
            int cch = 0;
            int cwc;
            while ((DWORD)cch < cbMax && psz[cch])
                cch++;
            cwc = MultiByteToWideChar(CP_ACP, 0, psz, cch, NULL, 0);
            pwszRet = (WCHAR *)malloc((cwc + 1) * sizeof(WCHAR));
            if (pwszRet)
            {
                MultiByteToWideChar(CP_ACP, 0, psz, cch, pwszRet, cwc);
                pwszRet[cwc] = 0;
                pData->pv = pwszRet;
                pData->cb = (cwc + 1) * sizeof(WCHAR);
            }
            GlobalUnlock(hMem);
        }
    }
    else
    {
        hMem = GetClipboardData(CF_UNICODETEXT);
        if (hMem)
        {
            const WCHAR *pwsz = (const WCHAR *)GlobalLock(hMem);
            if (pwsz)
            {
                DWORD cwcMax = GlobalSize(hMem) / sizeof(WCHAR);
                DWORD cwc = 0;
                while (cwc < cwcMax && pwsz[cwc])
                    cwc++;
                pwszRet = (WCHAR *)malloc((cwc + 1) * sizeof(WCHAR));
                if (pwszRet)
                {
                    memcpy(pwszRet, pwsz, cwc * sizeof(WCHAR));
                    pwszRet[cwc] = 0;
                    pData->pv = pwszRet;
                    pData->cb = (cwc + 1) * sizeof(WCHAR);
                }
                GlobalUnlock(hMem);
            }
        }
    }
}

/**
 * Gets the guest clipboard image as a packed DIB.  The clipboard is open;
 * Windows makes CF_DIB from CF_BITMAP when needed.
 *
 * @param   pData   Where to return the DIB (pv stays NULL if there is none).
 */
static void vboxClipGetGuestDib(VBOXCLIPDATA *pData)
{
    HANDLE hMem = GetClipboardData(CF_DIB);
    const BITMAPINFOHEADER *pHdr;

    if (!hMem)
        return;
    pHdr = (const BITMAPINFOHEADER *)GlobalLock(hMem);
    if (pHdr)
    {
        DWORD cb = vboxClipDibSize(pHdr, GlobalSize(hMem));
        if (cb > VBOXGUEST98_MAX_HGCM_DATA_SIZE)
            VBoxTrayLogRel("Clipboard: guest image too big (%lu KB)\n", cb / 1024);
        else if (cb)
        {
            pData->pv = malloc(cb);
            if (pData->pv)
            {
                memcpy(pData->pv, pHdr, cb);
                pData->cb = cb;
            }
        }
        GlobalUnlock(hMem);
    }
}

/**
 * Gets the guest clipboard HTML as a UTF-8 fragment, the way the host wants
 * it (ShClWinConvertCFHTMLToMIME).  The clipboard is open.
 *
 * @param   pData   Where to return the fragment (pv stays NULL if there is none).
 */
static void vboxClipGetGuestHtml(VBOXCLIPDATA *pData)
{
    HANDLE hMem = GetClipboardData(g_cfHtml);
    const char *pch;
    char *pszCfHtml;
    DWORD cch = 0;
    DWORD cbMax;
    long offStart;
    long offEnd;

    if (!hMem)
        return;
    pch = (const char *)GlobalLock(hMem);
    if (!pch)
        return;
    cbMax = GlobalSize(hMem);
    while (cch < cbMax && pch[cch])
        cch++;
    pszCfHtml = (char *)malloc(cch + 1);
    if (pszCfHtml)
    {
        memcpy(pszCfHtml, pch, cch);
        pszCfHtml[cch] = '\0';
    }
    GlobalUnlock(hMem);
    if (!pszCfHtml)
        return;

    /* The fragment between the offsets; without a usable header, all of it. */
    offStart = vboxClipCfHtmlValue(pszCfHtml, "StartFragment:");
    offEnd   = vboxClipCfHtmlValue(pszCfHtml, "EndFragment:");
    if (offStart > 0 && offEnd >= offStart && (DWORD)offEnd <= cch)
    {
        memmove(pszCfHtml, &pszCfHtml[offStart], offEnd - offStart);
        pszCfHtml[offEnd - offStart] = '\0';
        cch = offEnd - offStart;
    }
    pData->pv = pszCfHtml;
    pData->cb = cch + 1;
}

/**
 * Gets guest clipboard data in a host format.
 *
 * @param   hwnd        Our window.
 * @param   fFormat     One VBOX_SHCL_FMT_XXX bit.
 * @param   pData       Where to return the data, pv is NULL if there is none.
 */
static void vboxClipGetGuest(HWND hwnd, UINT32 fFormat, VBOXCLIPDATA *pData)
{
    pData->pv = NULL;
    pData->cb = 0;
    if (!OpenClipboard(hwnd))
        return;
    if (fFormat == VBOX_SHCL_FMT_UNICODETEXT)
        vboxClipGetGuestText(pData);
    else if (fFormat == VBOX_SHCL_FMT_BITMAP)
        vboxClipGetGuestDib(pData);
    else if (fFormat == VBOX_SHCL_FMT_HTML && g_cfHtml)
        vboxClipGetGuestHtml(pData);
    CloseClipboard();
}

/**
 * Allocates a clipboard memory block and copies data into it.
 */
static HGLOBAL vboxClipAllocGlobal(const void *pv, DWORD cb)
{
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, cb);
    if (hMem)
    {
        void *pvDst = GlobalLock(hMem);
        if (pvDst)
        {
            memcpy(pvDst, pv, cb);
            GlobalUnlock(hMem);
        }
        else
        {
            GlobalFree(hMem);
            hMem = NULL;
        }
    }
    return hMem;
}

/**
 * Converts the line endings of a text to CRLF, which is all Windows 98
 * applications understand.  Lone LF (Linux hosts) and lone CR (old Mac
 * text) become CRLF; CRLF stays as is.
 *
 * @returns malloc'ed zero terminated text, NULL if out of memory.
 * @param   pwszSrc     The text.
 * @param   cwcSrc      Length of the text (without terminator).
 * @param   pcwcDst     Where to return the length of the result.
 */
static WCHAR *vboxClipToCrLf(const WCHAR *pwszSrc, int cwcSrc, int *pcwcDst)
{
    WCHAR *pwszDst = (WCHAR *)malloc((2 * cwcSrc + 1) * sizeof(WCHAR)); /* Worst case: all lone LFs. */
    int iSrc;
    int iDst = 0;

    if (!pwszDst)
        return NULL;
    for (iSrc = 0; iSrc < cwcSrc; iSrc++)
    {
        WCHAR wc = pwszSrc[iSrc];
        if (wc == L'\r' || wc == L'\n')
        {
            pwszDst[iDst++] = L'\r';
            pwszDst[iDst++] = L'\n';
            if (wc == L'\r' && iSrc + 1 < cwcSrc && pwszSrc[iSrc + 1] == L'\n')
                iSrc++;
        }
        else
            pwszDst[iDst++] = wc;
    }
    pwszDst[iDst] = 0;
    *pcwcDst = iDst;
    return pwszDst;
}

/**
 * Renders host text as CF_TEXT (ANSI, active code page) or CF_UNICODETEXT,
 * with CRLF line ends.  The clipboard is open (WM_RENDERFORMAT).
 *
 * @param   cf      CF_TEXT or CF_UNICODETEXT.
 * @param   pwszIn  The host's UTF-16 text.
 * @param   cb      Its size in bytes.
 */
static void vboxClipRenderText(UINT cf, const WCHAR *pwszIn, DWORD cb)
{
    WCHAR *pwszCrLf;
    HGLOBAL hMem = NULL;
    char *pszAnsi;
    int cwc = 0;
    int cch;

    while ((DWORD)cwc < cb / sizeof(WCHAR) && pwszIn[cwc])
        cwc++;
    pwszCrLf = vboxClipToCrLf(pwszIn, cwc, &cwc);
    if (!pwszCrLf)
        return;

    if (cf == CF_TEXT)
    {
        cch = WideCharToMultiByte(CP_ACP, 0, pwszCrLf, cwc, NULL, 0, NULL, NULL);
        pszAnsi = (char *)malloc(cch + 1);
        if (pszAnsi)
        {
            WideCharToMultiByte(CP_ACP, 0, pwszCrLf, cwc, pszAnsi, cch, NULL, NULL);
            pszAnsi[cch] = '\0';
            hMem = vboxClipAllocGlobal(pszAnsi, cch + 1);
            free(pszAnsi);
        }
    }
    else
        hMem = vboxClipAllocGlobal(pwszCrLf, (cwc + 1) * sizeof(WCHAR));
    if (hMem && !SetClipboardData(cf, hMem))
        GlobalFree(hMem);
    free(pwszCrLf);
}

/**
 * Renders a host image (packed DIB) as CF_DIB.  The clipboard is open.
 */
static void vboxClipRenderDib(const void *pv, DWORD cb)
{
    const BITMAPINFOHEADER *pHdr = (const BITMAPINFOHEADER *)pv;
    DWORD cbDib = vboxClipDibSize(pHdr, cb);
    HGLOBAL hMem;

    if (!cbDib)
    {
        VBoxTrayLogRel("Clipboard: unusable host image (%lu bytes)\n", cb);
        return;
    }
    hMem = vboxClipAllocGlobal(pv, cbDib);
    if (hMem && !SetClipboardData(CF_DIB, hMem))
        GlobalFree(hMem);
    VBoxTrayLogRel("Clipboard: host image %ldx%ld, %u bpp, %lu KB\n", pHdr->biWidth,
                   pHdr->biHeight < 0 ? -pHdr->biHeight : pHdr->biHeight, pHdr->biBitCount, cbDib / 1024);
}

/**
 * Renders host HTML as CF_HTML.  The clipboard is open.  The host sends a
 * UTF-8 fragment, wrapped here in the CF_HTML header like upstream's
 * ShClWinConvertMIMEToCFHTML (offsets zero padded, so they are constant).
 */
static void vboxClipRenderHtml(const char *pch, DWORD cb)
{
    static const char s_szFormat[] =
        /*   0 */ "Version:1.0\r\n"
        /*  13 */ "StartHTML:000000101\r\n"
        /*  34 */ "EndHTML:%09lu\r\n"
        /*  53 */ "StartFragment:000000137\r\n"
        /*  78 */ "EndFragment:%09lu\r\n"
        /* 101 */ "<html>\r\n"
        /* 109 */ "<body>\r\n"
        /* 117 */ "<!--StartFragment-->"
        /* 137 */ "%s"
        /* 137 */ "<!--EndFragment-->\r\n"
        /* 157 */ "</body>\r\n"
        /* 166 */ "</html>\r\n";
    /* Length without the fragment (175): less the "%s", plus 4 for each "%09lu" printing 9 digits. */
    DWORD const cchHeader = sizeof(s_szFormat) - 1 - 2 + 2 * 4;
    char *pszFragment;
    char *pszCfHtml;
    HGLOBAL hMem;
    DWORD cch = 0;

    while (cch < cb && pch[cch])
        cch++;
    pszFragment = (char *)malloc(cch + 1);
    if (!pszFragment)
        return;
    memcpy(pszFragment, pch, cch);
    pszFragment[cch] = '\0';

    if (vboxClipIsCfHtml(pszFragment))
        pszCfHtml = pszFragment; /* Already CF_HTML (older hosts pass it through). */
    else
    {
        pszCfHtml = (char *)malloc(cchHeader + cch + 1);
        if (pszCfHtml)
            sprintf(pszCfHtml, s_szFormat, (unsigned long)(cchHeader + cch), (unsigned long)(cchHeader + cch - 38),
                    pszFragment);
        free(pszFragment);
        if (!pszCfHtml)
            return;
    }

    hMem = vboxClipAllocGlobal(pszCfHtml, lstrlen(pszCfHtml) + 1);
    if (hMem && !SetClipboardData(g_cfHtml, hMem))
        GlobalFree(hMem);
    free(pszCfHtml);
}

/**
 * WM_RENDERFORMAT: an application wants host data we announced.  Fetches it
 * from the host now (the application waits) and puts it on the clipboard,
 * which the application has open.
 *
 * @param   cf      The clipboard format asked for.
 */
static void vboxClipRenderFormat(UINT cf)
{
    UINT32 fFormat;
    UINT32 idClient;
    HANDLE hDevice;
    void *pv;
    UINT32 cb;
    int rc;

    if (cf == CF_TEXT || cf == CF_UNICODETEXT)
        fFormat = VBOX_SHCL_FMT_UNICODETEXT;
    else if (cf == CF_DIB)
        fFormat = VBOX_SHCL_FMT_BITMAP;
    else if (cf == g_cfHtml && g_cfHtml)
        fFormat = VBOX_SHCL_FMT_HTML;
    else
        return;

    EnterCriticalSection(&g_ClipCritSect);
    hDevice  = g_hClipDevice;
    idClient = g_idClipClient;
    LeaveCriticalSection(&g_ClipCritSect);
    if (!idClient)
        return;

    rc = vboxClipReadData(hDevice, idClient, fFormat, &pv, &cb);
    if (RT_FAILURE(rc))
    {
        VBoxTrayLogRel("Clipboard: reading host data (format %#lx) failed, rc=%d\n", fFormat, rc);
        return;
    }
    if (fFormat == VBOX_SHCL_FMT_UNICODETEXT)
        vboxClipRenderText(cf, (const WCHAR *)pv, cb);
    else if (fFormat == VBOX_SHCL_FMT_BITMAP)
        vboxClipRenderDib(pv, cb);
    else
        vboxClipRenderHtml((const char *)pv, cb);
    free(pv);
}

/**
 * Opens the clipboard, retrying a little as another application may have
 * it open for a moment (like upstream's ShClWinOpen).
 */
static BOOL vboxClipOpen(HWND hwnd)
{
    unsigned i;
    for (i = 0; i < VBOXCLIP_OPEN_TRIES; i++)
    {
        if (OpenClipboard(hwnd))
            return TRUE;
        Sleep(VBOXCLIP_OPEN_WAIT_MS);
    }
    return FALSE;
}

/**
 * Gets the paths of the files copied in the guest (CF_HDROP), each zero
 * terminated, then an empty one.
 *
 * @param   ppszFiles   Where to return them (malloc'ed), NULL if there are none.
 */
static void vboxClipGetGuestFiles(HWND hwnd, char **ppszFiles)
{
    HANDLE hMem;

    *ppszFiles = NULL;
    if (!vboxClipOpen(hwnd))
        return;
    hMem = GetClipboardData(CF_HDROP);
    if (hMem)
        *ppszFiles = VBoxDnDCopyHDrop((HDROP)hMem);
    CloseClipboard();
}

/**
 * The host reported new clipboard content: replace the guest clipboard
 * content with announcements of the formats we can render
 * (ShClWinClearAndAnnounceFormats).  The data is fetched only when an
 * application asks for it (WM_RENDERFORMAT).  Files go on the clipboard as
 * an OLE data object instead (ClipFile.c), like upstream, alone.
 *
 * @param   hwnd        Our window, becomes the clipboard owner.
 * @param   fFormats    The host formats (VBOX_SHCL_FMT_XXX).
 */
static void vboxClipAnnounce(HWND hwnd, UINT32 fFormats)
{
    if ((fFormats & VBOX_SHCL_FMT_URI_LIST) && VBoxClipFileEnabled())
    {
        HANDLE hDevice;
        UINT32 idClient;

        EnterCriticalSection(&g_ClipCritSect);
        hDevice  = g_hClipDevice;
        idClient = g_idClipClient;
        LeaveCriticalSection(&g_ClipCritSect);
        if (idClient && VBoxClipFileAnnounce(hDevice, idClient))
            return;
    }

    if (!vboxClipOpen(hwnd))
    {
        VBoxTrayLogRel("Clipboard: cannot open the clipboard for the host formats %#lx\n", fFormats);
        return;
    }
    EmptyClipboard();
    VBoxClipFileForget();
    if (fFormats & VBOX_SHCL_FMT_UNICODETEXT)
    {
        /* Windows 98 does not make one from the other, so offer both. */
        SetClipboardData(CF_TEXT, NULL);
        SetClipboardData(CF_UNICODETEXT, NULL);
    }
    if (fFormats & VBOX_SHCL_FMT_BITMAP)
        SetClipboardData(CF_DIB, NULL);
    if ((fFormats & VBOX_SHCL_FMT_HTML) && g_cfHtml)
        SetClipboardData(g_cfHtml, NULL);
    CloseClipboard();
}

/**
 * WM_RENDERALLFORMATS: VBoxTray is going away and its announcements with
 * it.  Like upstream, the clipboard is emptied rather than fetching
 * everything from the host.
 */
static void vboxClipRenderAll(HWND hwnd)
{
    if (OpenClipboard(hwnd))
    {
        if (GetClipboardOwner() == hwnd)
            EmptyClipboard();
        CloseClipboard();
    }
}

/**
 * Clipboard part of the VBoxTray window procedure.
 *
 * @returns TRUE if the message was handled and *plResult is the result.
 *          WM_CREATE and WM_DESTROY return FALSE so VBoxTray handles them too.
 */
BOOL VBoxClipboardWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT *plResult)
{
    *plResult = 0;
    switch (uMsg)
    {
        case WM_CREATE:
            /* Sends WM_DRAWCLIPBOARD right away, ignored as we're not connected yet. */
            g_hwndNextViewer = SetClipboardViewer(hwnd);
            return FALSE;

        case WM_DESTROY:
            ChangeClipboardChain(hwnd, g_hwndNextViewer);
            g_hwndNextViewer = NULL;
            /* Out of the chain first: emptying mustn't reach the host. */
            VBoxClipFileTerm();
            return FALSE;

        case WM_CHANGECBCHAIN:
            if ((HWND)wParam == g_hwndNextViewer)
                g_hwndNextViewer = (HWND)lParam;
            else if (g_hwndNextViewer)
                SendMessage(g_hwndNextViewer, uMsg, wParam, lParam);
            return TRUE;

        case WM_DRAWCLIPBOARD:
            vboxClipReportGuestFormats(hwnd);
            if (g_hwndNextViewer)
                SendMessage(g_hwndNextViewer, uMsg, wParam, lParam);
            return TRUE;

        case WM_VBOXCLIP_CONNECTED:
            vboxClipReportGuestFormats(hwnd);
            return TRUE;

        case WM_VBOXCLIP_ANNOUNCE:
            vboxClipAnnounce(hwnd, (UINT32)wParam);
            return TRUE;

        case WM_RENDERFORMAT:
            vboxClipRenderFormat((UINT)wParam);
            return TRUE;

        case WM_RENDERALLFORMATS:
            vboxClipRenderAll(hwnd);
            return TRUE;

        case WM_VBOXCLIP_GET:
            vboxClipGetGuest(hwnd, (UINT32)wParam, (VBOXCLIPDATA *)lParam);
            return TRUE;

        case WM_VBOXCLIP_GET_FILES:
            vboxClipGetGuestFiles(hwnd, (char **)lParam);
            return TRUE;

        default:
            return FALSE;
    }
}


/*********************************************************************************************************************************
*   Service                                                                                                                      *
*********************************************************************************************************************************/

/**
 * Initializes the clipboard service, before any thread runs.
 */
void VBoxClipboardInit(void)
{
    InitializeCriticalSection(&g_ClipCritSect);
    g_cfHtml = RegisterClipboardFormat(VBOXCLIP_CF_HTML_NAME);
    VBoxClipFileInit();
}

/**
 * Wakes up the worker thread: its pending MSG_PEEK_WAIT is cancelled (the
 * service doesn't complete it on a disconnect) and the client disconnected,
 * which also completes a legacy MSG_OLD_GET_WAIT.
 */
void VBoxClipboardStop(void)
{
    EnterCriticalSection(&g_ClipCritSect);
    if (g_idClipClient)
    {
        if (g_fClipContextId)
            vboxClipCancel(g_hClipDevice, g_idClipClient);
        VbglR3HgcmDisconnect(g_hClipDevice, g_idClipClient);
        g_idClipClient = 0;
    }
    LeaveCriticalSection(&g_ClipCritSect);
}

/**
 * Gets the paths of the files copied in the guest from the window thread
 * (worker thread, for ClipFile.c).
 *
 * @returns Each zero terminated, then an empty one (malloc'ed); NULL if
 *          there are none.
 */
char *VBoxClipboardGetFiles(void)
{
    char *pszFiles = NULL;
    SendMessage(g_hwndVBoxTray, WM_VBOXCLIP_GET_FILES, 0, (LPARAM)&pszFiles);
    return pszFiles;
}

/**
 * Handles one host message.
 */
static void vboxClipProcessHostMsg(HANDLE hDevice, UINT32 idClient, UINT32 uMsg, UINT32 fFormats, UINT64 idContext)
{
    VBOXCLIPDATA Data;
    UINT32 fFormat;

    switch (uMsg)
    {
        case VBOX_SHCL_HOST_MSG_FORMATS_REPORT:
            /* Announced to the guest clipboard; the data comes when an application
               asks for it.  Like upstream, an empty report changes nothing. */
            if (fFormats != VBOX_SHCL_FMT_NONE)
                PostMessage(g_hwndVBoxTray, WM_VBOXCLIP_ANNOUNCE, (WPARAM)fFormats, 0);
            break;

        case VBOX_SHCL_HOST_MSG_READ_DATA:
        case VBOX_SHCL_HOST_MSG_READ_DATA_CID:
            /* The host asks for one format at a time. */
            if (fFormats & VBOX_SHCL_FMT_UNICODETEXT)
                fFormat = VBOX_SHCL_FMT_UNICODETEXT;
            else if (fFormats & VBOX_SHCL_FMT_BITMAP)
                fFormat = VBOX_SHCL_FMT_BITMAP;
            else
                fFormat = fFormats & VBOX_SHCL_FMT_HTML;
            Data.pv = NULL;
            Data.cb = 0;
            if (fFormat)
                SendMessage(g_hwndVBoxTray, WM_VBOXCLIP_GET, fFormat, (LPARAM)&Data);
            else
                fFormat = VBOX_SHCL_FMT_UNICODETEXT;
            /* Always answer, the host waits for it. */
            vboxClipWriteData(hDevice, idClient, idContext, fFormat, Data.pv, Data.pv ? Data.cb : 0);
            free(Data.pv);
            break;

        default:
            break;
    }
}

/**
 * Clipboard worker thread.
 *
 * @returns 0 on normal exit, 1 if the service is not available.
 * @param   pvUser      Unused.
 */
unsigned __stdcall VBoxClipboardThread(void *pvUser)
{
    HANDLE hDevice;
    UINT32 idClient = 0;
    UINT32 uMsg;
    UINT32 fFormats;
    UINT64 idContext;
    UINT64 fHostFeatures;
    int rcLast = VINF_SUCCESS;
    int rc;

    /* The window thread uses the client too, hence overlapped. */
    hDevice = VbglR3OpenEx(TRUE);
    if (hDevice == INVALID_HANDLE_VALUE)
        hDevice = VbglR3OpenEx(FALSE);
    if (hDevice == INVALID_HANDLE_VALUE)
        return 1;

    rc = VbglR3HgcmConnect(hDevice, VBOX_SHCL_SERVICE_NAME, &idClient);
    if (RT_FAILURE(rc))
    {
        VBoxTrayLogRel("Clipboard: connecting to the service failed, rc=%d\n", rc);
        VbglR3Close(hDevice);
        return 1;
    }
    fHostFeatures = vboxClipReportFeatures(hDevice, idClient);
    g_fClipContextId = (fHostFeatures & VBOX_SHCL_HF_0_CONTEXT_ID) != 0;
    VBoxClipFileSetEnabled(hDevice, idClient, g_fClipContextId && (fHostFeatures & VBOX_SHCL_HF_0_TRANSFERS));
    VBoxTrayLogRel("Clipboard: connected, client %lu%s%s\n", idClient,
                   g_fClipContextId ? ", context IDs" : ", legacy protocol",
                   VBoxClipFileEnabled() ? ", file transfers" : "");

    EnterCriticalSection(&g_ClipCritSect);
    g_hClipDevice  = hDevice;
    g_idClipClient = idClient;
    LeaveCriticalSection(&g_ClipCritSect);
    PostMessage(g_hwndVBoxTray, WM_VBOXCLIP_CONNECTED, 0, 0);

    while (!g_fVBoxTrayShutdown)
    {
        rc = vboxClipGetHostMsg(hDevice, idClient, &uMsg, &fFormats, &idContext);
        if (RT_FAILURE(rc))
        {
            /* VERR_INVALID_HANDLE: disconnected by VBoxClipboardStop. */
            if (g_fVBoxTrayShutdown || rc == VERR_INVALID_HANDLE)
                break;
            if (rc != rcLast)
                VBoxTrayLogRel("Clipboard: waiting for host messages failed, rc=%d\n", rc);
            rcLast = rc;
            Sleep(1000);
            continue;
        }
        rcLast = VINF_SUCCESS;
        if (uMsg == VBOX_SHCL_HOST_MSG_QUIT)
        {
            VBoxTrayLogRel("Clipboard: the host asked to quit\n");
            break;
        }
        if (uMsg == VBOX_SHCL_HOST_MSG_CANCELED)
            continue; /* VBoxClipboardStop, or the wait was interrupted. */
        vboxClipProcessHostMsg(hDevice, idClient, uMsg, fFormats, idContext);
    }

    VBoxClipFileWorkerDone();
    EnterCriticalSection(&g_ClipCritSect);
    idClient = g_idClipClient;
    g_idClipClient = 0;
    g_hClipDevice  = INVALID_HANDLE_VALUE;
    LeaveCriticalSection(&g_ClipCritSect);
    if (idClient)
        VbglR3HgcmDisconnect(hDevice, idClient);
    VbglR3Close(hDevice);
    return 0;
}
