/** @file
 * VBoxTray - Shared clipboard (text only), legacy HGCM protocol.
 *
 * Two threads share one HGCM client (and therefore one overlapped handle):
 *  - The worker thread (VBoxClipboardThread) blocks in
 *    VBOX_SHCL_GUEST_FN_MSG_OLD_GET_WAIT and handles the host messages.
 *  - The VBoxTray window thread is in the clipboard viewer chain and reports
 *    guest clipboard changes to the host (VBoxClipboardWndProc).
 * The guest clipboard itself is only touched by the window thread; the
 * worker asks it with SendMessage.
 *
 * Host data is fetched as soon as the host reports new text (no delayed
 * rendering), so the window thread never has to wait for the host while
 * another application holds the clipboard open.
 *
 * Windows 98 applications use CF_TEXT (ANSI); the host speaks UTF-16, so the
 * text is converted with the active code page.  CF_UNICODETEXT is also
 * offered for the few Unicode aware applications.
 */

#include "VBoxTray.h"

#include <stdlib.h>
#include <string.h>


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/

/** Worker -> window: the HGCM client is connected, report the guest formats. */
#define WM_VBOXCLIP_CONNECTED       (WM_APP + 10)
/** Worker -> window: put text on the guest clipboard.
 *  wParam: size in bytes, lParam: UTF-16 text (NULL empties the clipboard). */
#define WM_VBOXCLIP_SET             (WM_APP + 11)
/** Worker -> window: get the guest clipboard text.  lParam: VBOXCLIPDATA to fill. */
#define WM_VBOXCLIP_GET             (WM_APP + 12)

/** First guess for the size of the host data. */
#define VBOXCLIP_READ_SIZE          16384
/** Timeout for the calls that don't wait for host messages (ms). */
#define VBOXCLIP_CALL_TIMEOUT_MS    10000


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/

/** Text passed between the threads; pwsz is malloc'ed by the producer. */
typedef struct VBOXCLIPDATA
{
    WCHAR *pwsz;
    /** Size in bytes, including the terminator. */
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

        /* Too small: retry once with the size the host asked for. */
        if (cbActual > VBOXGUEST98_MAX_HGCM_DATA_SIZE)
            return VERR_TOO_MUCH_DATA;
        cbBuf = cbActual;
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
    if (   IsClipboardFormatAvailable(CF_TEXT)
        || IsClipboardFormatAvailable(CF_OEMTEXT)
        || IsClipboardFormatAvailable(CF_UNICODETEXT))
        return VBOX_SHCL_FMT_UNICODETEXT;
    return VBOX_SHCL_FMT_NONE;
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
 * Gets the guest clipboard text as UTF-16.
 *
 * @param   hwnd    Our window.
 * @param   pData   Where to return the text, pwsz is NULL if there is none.
 */
static void vboxClipGetGuestText(HWND hwnd, VBOXCLIPDATA *pData)
{
    HANDLE hMem;

    pData->pwsz = NULL;
    pData->cb   = 0;
    if (!OpenClipboard(hwnd))
        return;

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
            pData->pwsz = (WCHAR *)malloc((cwc + 1) * sizeof(WCHAR));
            if (pData->pwsz)
            {
                MultiByteToWideChar(CP_ACP, 0, psz, cch, pData->pwsz, cwc);
                pData->pwsz[cwc] = 0;
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
                pData->pwsz = (WCHAR *)malloc((cwc + 1) * sizeof(WCHAR));
                if (pData->pwsz)
                {
                    memcpy(pData->pwsz, pwsz, cwc * sizeof(WCHAR));
                    pData->pwsz[cwc] = 0;
                    pData->cb = (cwc + 1) * sizeof(WCHAR);
                }
                GlobalUnlock(hMem);
            }
        }
    }

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
 * Puts host text on the guest clipboard, or empties it.
 *
 * @param   hwnd    Our window, becomes the clipboard owner.
 * @param   pwszIn  UTF-16 text, NULL to empty the clipboard.
 * @param   cb      Size of the text in bytes.
 */
static void vboxClipSetGuestText(HWND hwnd, const WCHAR *pwszIn, DWORD cb)
{
    WCHAR *pwszCrLf = NULL;
    const WCHAR *pwsz = pwszIn;
    HGLOBAL hMem;
    char *pszAnsi;
    int cwc = 0;
    int cch;

    if (!OpenClipboard(hwnd))
        return;
    EmptyClipboard();

    if (pwsz)
    {
        while ((DWORD)cwc < cb / sizeof(WCHAR) && pwsz[cwc])
            cwc++;
        pwszCrLf = vboxClipToCrLf(pwszIn, cwc, &cwc);
        if (pwszCrLf)
            pwsz = pwszCrLf;
        else
            cwc = 0; /* Out of memory; leave the clipboard empty rather than half converted. */

        cch = WideCharToMultiByte(CP_ACP, 0, pwsz, cwc, NULL, 0, NULL, NULL);
        pszAnsi = (char *)malloc(cch + 1);
        if (pszAnsi)
        {
            WideCharToMultiByte(CP_ACP, 0, pwsz, cwc, pszAnsi, cch, NULL, NULL);
            pszAnsi[cch] = '\0';
            hMem = vboxClipAllocGlobal(pszAnsi, cch + 1);
            if (hMem && !SetClipboardData(CF_TEXT, hMem))
                GlobalFree(hMem);
            free(pszAnsi);
        }

        hMem = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, (cwc + 1) * sizeof(WCHAR));
        if (hMem)
        {
            WCHAR *pwszDst = (WCHAR *)GlobalLock(hMem);
            if (pwszDst)
            {
                memcpy(pwszDst, pwsz, cwc * sizeof(WCHAR));
                pwszDst[cwc] = 0;
                GlobalUnlock(hMem);
            }
            if (!pwszDst || !SetClipboardData(CF_UNICODETEXT, hMem))
                GlobalFree(hMem);
        }
        free(pwszCrLf);
    }

    CloseClipboard();
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

        case WM_VBOXCLIP_SET:
            vboxClipSetGuestText(hwnd, (const WCHAR *)lParam, (DWORD)wParam);
            return TRUE;

        case WM_VBOXCLIP_GET:
            vboxClipGetGuestText(hwnd, (VBOXCLIPDATA *)lParam);
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
    void *pv;
    UINT32 cb;

    switch (uMsg)
    {
        case VBOX_SHCL_HOST_MSG_FORMATS_REPORT:
            if (fFormats & VBOX_SHCL_FMT_UNICODETEXT)
            {
                if (RT_SUCCESS(vboxClipReadData(hDevice, idClient, VBOX_SHCL_FMT_UNICODETEXT, &pv, &cb)))
                {
                    SendMessage(g_hwndVBoxTray, WM_VBOXCLIP_SET, (WPARAM)cb, (LPARAM)pv);
                    free(pv);
                }
            }
            else
                SendMessage(g_hwndVBoxTray, WM_VBOXCLIP_SET, 0, (LPARAM)NULL); /* Nothing we can show. */
            break;

        case VBOX_SHCL_HOST_MSG_READ_DATA:
            Data.pwsz = NULL;
            Data.cb   = 0;
            if (fFormats & VBOX_SHCL_FMT_UNICODETEXT)
                SendMessage(g_hwndVBoxTray, WM_VBOXCLIP_GET, 0, (LPARAM)&Data);
            /* Always answer, the host waits for it. */
            vboxClipWriteData(hDevice, idClient, VBOX_SHCL_FMT_UNICODETEXT, Data.pwsz, Data.pwsz ? Data.cb : 0);
            free(Data.pwsz);
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
