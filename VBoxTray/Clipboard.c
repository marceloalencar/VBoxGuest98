/** @file
 * VBoxTray - Shared clipboard (text, images, HTML), legacy HGCM protocol.
 *
 * Two threads share one HGCM client (and therefore one overlapped handle):
 *  - The worker thread (VBoxClipboardThread) blocks in
 *    VBOX_SHCL_GUEST_FN_MSG_OLD_GET_WAIT and handles the host messages.
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

#include "VBoxTray.h"

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
/** Next window in the clipboard viewer chain (window thread only). */
static HWND g_hwndNextViewer = NULL;
/** The "HTML Format" (CF_HTML) clipboard format. */
static UINT g_cfHtml = 0;


/*********************************************************************************************************************************
*   HGCM calls                                                                                                                   *
*********************************************************************************************************************************/

/**
 * Allocates a zeroed HGCM call with room for cParms parameters and cbData
 * bytes of data placed right after them.
 */
static VBOXGUEST98HGCMCALL *vboxClipAllocCall(UINT32 idClient, UINT32 uFunction, UINT32 cParms, UINT32 cMsTimeout,
                                             DWORD cbData, DWORD *pcbCall)
{
    DWORD cbCall = sizeof(VBOXGUEST98HGCMCALL) + cParms * sizeof(VBOXGUEST98HGCMPARM) + cbData;
    VBOXGUEST98HGCMCALL *pCall = (VBOXGUEST98HGCMCALL *)calloc(1, cbCall);
    if (pCall)
    {
        pCall->u32ClientID = idClient;
        pCall->u32Function = uFunction;
        pCall->cMsTimeout  = cMsTimeout;
        pCall->cParms      = cParms;
    }
    *pcbCall = cbCall;
    return pCall;
}

/** Offset of the data area of a call made by vboxClipAllocCall. */
#define VBOXCLIP_DATA_OFF(cParms)   (sizeof(VBOXGUEST98HGCMCALL) + (cParms) * sizeof(VBOXGUEST98HGCMPARM))

/**
 * Waits for the next host message.
 */
static int vboxClipGetHostMsg(HANDLE hDevice, UINT32 idClient, UINT32 *puMsg, UINT32 *pfFormats)
{
    VBOXGUEST98HGCMPARM *paParms;
    VBOXGUEST98HGCMCALL *pCall;
    DWORD cbCall;
    int rc;

    pCall = vboxClipAllocCall(idClient, VBOX_SHCL_GUEST_FN_MSG_OLD_GET_WAIT, 2, VBOXGUEST98_WAIT_INFINITE, 0, &cbCall);
    if (!pCall)
        return VERR_NO_MEMORY;
    paParms = VBOXGUEST98_HGCM_CALL_PARMS(pCall);
    paParms[0].enmType = VBOXGUEST98_HGCM_PARM_32BIT;
    paParms[1].enmType = VBOXGUEST98_HGCM_PARM_32BIT;

    rc = VbglR3HgcmCall(hDevice, pCall, cbCall);
    if (RT_SUCCESS(rc))
    {
        *puMsg     = (UINT32)paParms[0].u64Value;
        *pfFormats = (UINT32)paParms[1].u64Value;
    }
    free(pCall);
    return rc;
}

/**
 * Tells the host which formats the guest clipboard has.
 */
static int vboxClipReportFormats(HANDLE hDevice, UINT32 idClient, UINT32 fFormats)
{
    VBOXGUEST98HGCMCALL *pCall;
    DWORD cbCall;
    int rc;

    pCall = vboxClipAllocCall(idClient, VBOX_SHCL_GUEST_FN_REPORT_FORMATS, 1, VBOXCLIP_CALL_TIMEOUT_MS, 0, &cbCall);
    if (!pCall)
        return VERR_NO_MEMORY;
    VBOXGUEST98_HGCM_CALL_PARMS(pCall)[0].enmType  = VBOXGUEST98_HGCM_PARM_32BIT;
    VBOXGUEST98_HGCM_CALL_PARMS(pCall)[0].u64Value = fFormats;

    rc = VbglR3HgcmCall(hDevice, pCall, cbCall);
    free(pCall);
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
    VBOXGUEST98HGCMPARM *paParms;
    VBOXGUEST98HGCMCALL *pCall;
    UINT32 cbBuf = VBOXCLIP_READ_SIZE;
    UINT32 cbActual;
    DWORD cbCall;
    int iTry;
    int rc = VERR_BUFFER_OVERFLOW;

    *ppv = NULL;
    *pcb = 0;
    for (iTry = 0; iTry < 2; iTry++)
    {
        pCall = vboxClipAllocCall(idClient, VBOX_SHCL_GUEST_FN_DATA_READ, 3, VBOXCLIP_CALL_TIMEOUT_MS, cbBuf, &cbCall);
        if (!pCall)
            return VERR_NO_MEMORY;
        paParms = VBOXGUEST98_HGCM_CALL_PARMS(pCall);
        paParms[0].enmType  = VBOXGUEST98_HGCM_PARM_32BIT;
        paParms[0].u64Value = fFormat;
        paParms[1].enmType  = VBOXGUEST98_HGCM_PARM_BUFFER_OUT;
        paParms[1].cb       = cbBuf;
        paParms[1].off      = VBOXCLIP_DATA_OFF(3);
        paParms[2].enmType  = VBOXGUEST98_HGCM_PARM_32BIT;

        rc = VbglR3HgcmCall(hDevice, pCall, cbCall);
        cbActual = (UINT32)paParms[2].u64Value;
        if (RT_SUCCESS(rc) && rc != VINF_BUFFER_OVERFLOW && cbActual <= cbBuf)
        {
            *ppv = malloc(cbActual + sizeof(WCHAR));
            if (*ppv)
            {
                memcpy(*ppv, (BYTE *)pCall + VBOXCLIP_DATA_OFF(3), cbActual);
                memset((BYTE *)*ppv + cbActual, 0, sizeof(WCHAR));
                *pcb = cbActual;
            }
            else
                rc = VERR_NO_MEMORY;
            free(pCall);
            return rc;
        }
        free(pCall);
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
 * Sends guest clipboard data to the host (answer to VBOX_SHCL_HOST_MSG_READ_DATA).
 */
static int vboxClipWriteData(HANDLE hDevice, UINT32 idClient, UINT32 fFormat, const void *pv, UINT32 cb)
{
    VBOXGUEST98HGCMPARM *paParms;
    VBOXGUEST98HGCMCALL *pCall;
    DWORD cbCall;
    int rc;

    if (cb > VBOXGUEST98_MAX_HGCM_DATA_SIZE)
        cb = 0; /* Too big for the driver, send nothing rather than leaving the host waiting. */

    pCall = vboxClipAllocCall(idClient, VBOX_SHCL_GUEST_FN_DATA_WRITE, 2, VBOXCLIP_CALL_TIMEOUT_MS, cb, &cbCall);
    if (!pCall)
        return VERR_NO_MEMORY;
    paParms = VBOXGUEST98_HGCM_CALL_PARMS(pCall);
    paParms[0].enmType  = VBOXGUEST98_HGCM_PARM_32BIT;
    paParms[0].u64Value = fFormat;
    paParms[1].enmType  = VBOXGUEST98_HGCM_PARM_BUFFER_IN;
    paParms[1].cb       = cb;
    paParms[1].off      = VBOXCLIP_DATA_OFF(2);
    if (cb)
        memcpy((BYTE *)pCall + VBOXCLIP_DATA_OFF(2), pv, cb);

    rc = VbglR3HgcmCall(hDevice, pCall, cbCall);
    free(pCall);
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
    if (GetClipboardOwner() == hwnd)
        return;

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
 * The host reported new clipboard content: replace the guest clipboard
 * content with announcements of the formats we can render
 * (ShClWinClearAndAnnounceFormats).  The data is fetched only when an
 * application asks for it (WM_RENDERFORMAT).
 *
 * @param   hwnd        Our window, becomes the clipboard owner.
 * @param   fFormats    The host formats (VBOX_SHCL_FMT_XXX).
 */
static void vboxClipAnnounce(HWND hwnd, UINT32 fFormats)
{
    if (!vboxClipOpen(hwnd))
    {
        VBoxTrayLogRel("Clipboard: cannot open the clipboard for the host formats %#lx\n", fFormats);
        return;
    }
    EmptyClipboard();
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
}

/**
 * Wakes up the worker thread by disconnecting the client: the host then
 * completes the pending VBOX_SHCL_GUEST_FN_MSG_OLD_GET_WAIT.
 */
void VBoxClipboardStop(void)
{
    EnterCriticalSection(&g_ClipCritSect);
    if (g_idClipClient)
    {
        VbglR3HgcmDisconnect(g_hClipDevice, g_idClipClient);
        g_idClipClient = 0;
    }
    LeaveCriticalSection(&g_ClipCritSect);
}

/**
 * Handles one host message.
 */
static void vboxClipProcessHostMsg(HANDLE hDevice, UINT32 idClient, UINT32 uMsg, UINT32 fFormats)
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
            vboxClipWriteData(hDevice, idClient, fFormat, Data.pv, Data.pv ? Data.cb : 0);
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
    VBoxTrayLogRel("Clipboard: connected, client %lu\n", idClient);

    EnterCriticalSection(&g_ClipCritSect);
    g_hClipDevice  = hDevice;
    g_idClipClient = idClient;
    LeaveCriticalSection(&g_ClipCritSect);
    PostMessage(g_hwndVBoxTray, WM_VBOXCLIP_CONNECTED, 0, 0);

    while (!g_fVBoxTrayShutdown)
    {
        rc = vboxClipGetHostMsg(hDevice, idClient, &uMsg, &fFormats);
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
        vboxClipProcessHostMsg(hDevice, idClient, uMsg, fFormats);
    }

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
