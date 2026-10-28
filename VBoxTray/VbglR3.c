/** @file
 * VBoxTray - Access to the VBoxGst driver.
 *
 * Each service thread normally opens its own handle: handles opened without
 * FILE_FLAG_OVERLAPPED serialize their I/O, so a thread blocked in
 * IOCTL_VBOXGUEST98_WAIT_FOR_EVENTS would stall everyone sharing the handle.
 * A service whose threads must share one session (HGCM client IDs belong to
 * a session) opens the handle overlapped with VbglR3OpenEx.  All I/O
 * controls pass an OVERLAPPED structure so both kinds of handles work.
 */

#include <windows.h>
#include <winioctl.h>
#include "VBoxTray.h"

#include <stdlib.h>
#include <string.h>


/**
 * Opens the VBoxGuest device by name, like the official Guest Additions.
 *
 * @returns Device handle, INVALID_HANDLE_VALUE on failure.
 */
HANDLE VbglR3Open(void)
{
    return VbglR3OpenEx(FALSE);
}

/**
 * Opens the VBoxGuest device, optionally for overlapped I/O so several
 * threads can have I/O controls in progress on the handle at once.
 *
 * @returns Device handle, INVALID_HANDLE_VALUE on failure.
 * @param   fOverlapped     Whether to open with FILE_FLAG_OVERLAPPED.
 */
HANDLE VbglR3OpenEx(BOOL fOverlapped)
{
    return CreateFileA(VBOXGUEST98_DEVICE_NAME, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | (fOverlapped ? FILE_FLAG_OVERLAPPED : 0), NULL);
}

/**
 * Closes a handle returned by VbglR3Open.
 */
void VbglR3Close(HANDLE hDevice)
{
    if (hDevice != INVALID_HANDLE_VALUE)
        CloseHandle(hDevice);
}

/**
 * Issues an I/O control with the same buffer for input and output.
 *
 * @returns VBox status code (of the I/O control itself).
 */
static int vbglR3DoIoCtl(HANDLE hDevice, DWORD uCmd, void *pvBuf, DWORD cbIn, DWORD cbOut)
{
    OVERLAPPED Overlapped;
    DWORD cbReturned = 0;
    DWORD dwErr;
    BOOL fOk;

    /* Required for overlapped handles, harmless for the others.  The driver
       completes everything synchronously, but play by the rules. */
    memset(&Overlapped, 0, sizeof(Overlapped));
    Overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!Overlapped.hEvent)
        return VERR_NO_MEMORY;

    fOk = DeviceIoControl(hDevice, uCmd, pvBuf, cbIn, pvBuf, cbOut, &cbReturned, &Overlapped);
    if (!fOk && GetLastError() == ERROR_IO_PENDING)
        fOk = GetOverlappedResult(hDevice, &Overlapped, &cbReturned, TRUE);
    dwErr = fOk ? NO_ERROR : GetLastError();
    CloseHandle(Overlapped.hEvent);
    if (fOk)
        return VINF_SUCCESS;

    switch (dwErr)
    {
        case ERROR_ACCESS_DENIED:       return VERR_PERMISSION_DENIED;
        case ERROR_INVALID_PARAMETER:   return VERR_INVALID_PARAMETER;
        case ERROR_NOT_ENOUGH_MEMORY:   return VERR_NO_MEMORY;
        default:                        return VERR_GENERAL_FAILURE;
    }
}

/**
 * Writes text to the release log (VBox.log on the host).
 *
 * @returns VBox status code.
 * @param   hDevice     The device handle.
 * @param   pch         The text, lines ending with a newline.
 * @param   cch         Its length, at most VBOXGUEST98_MAX_LOG_SIZE.
 */
int VbglR3WriteLog(HANDLE hDevice, const char *pch, DWORD cch)
{
    return vbglR3DoIoCtl(hDevice, IOCTL_VBOXGUEST98_WRITE_LOG, (void *)pch, cch, 0);
}

/**
 * Performs a raw VMMDev request.
 *
 * @returns VBox status code, the request status on success.
 */
static int vbglR3GRPerform(HANDLE hDevice, VMMDevRequestHeader *pReq)
{
    int rc = vbglR3DoIoCtl(hDevice, IOCTL_VBOXGUEST98_VMMDEV_REQUEST, pReq, pReq->size, pReq->size);
    return RT_SUCCESS(rc) ? pReq->rc : rc;
}

/**
 * Initializes a raw VMMDev request.
 */
static void vbglR3GRInit(VMMDevRequestHeader *pReq, UINT32 cbReq, VMMDevRequestType enmType)
{
    memset(pReq, 0, cbReq);
    pReq->size        = cbReq;
    pReq->version     = VMMDEV_REQUEST_HEADER_VERSION;
    pReq->requestType = enmType;
    pReq->rc          = VERR_GENERAL_FAILURE;
}

/**
 * Changes the events this handle receives.
 */
int VbglR3ChangeFilterMask(HANDLE hDevice, UINT32 fOrMask, UINT32 fNotMask)
{
    VBOXGUEST98CHANGEMASK Mask;
    Mask.fOrMask  = fOrMask;
    Mask.fNotMask = fNotMask;
    return vbglR3DoIoCtl(hDevice, IOCTL_VBOXGUEST98_CHANGE_FILTER_MASK, &Mask, sizeof(Mask), 0);
}

/**
 * Waits for events (which must be in the filter mask).
 *
 * @returns VINF_SUCCESS, VERR_TIMEOUT, VERR_INTERRUPTED or an error.
 * @param   hDevice     The device handle.
 * @param   fMask       Events to wait for.
 * @param   cMsTimeout  Timeout in milliseconds.
 * @param   pfEvents    Where to return the events that occurred.
 */
int VbglR3WaitForEvents(HANDLE hDevice, UINT32 fMask, UINT32 cMsTimeout, UINT32 *pfEvents)
{
    VBOXGUEST98WAITEVENTS Wait;
    int rc;

    Wait.cMsTimeout = cMsTimeout;
    Wait.fEvents    = fMask;
    Wait.rc         = VERR_GENERAL_FAILURE;
    rc = vbglR3DoIoCtl(hDevice, IOCTL_VBOXGUEST98_WAIT_FOR_EVENTS, &Wait, sizeof(Wait), sizeof(Wait));
    if (RT_SUCCESS(rc))
        rc = Wait.rc;
    *pfEvents = RT_SUCCESS(rc) ? Wait.fEvents : 0;
    return rc;
}

/**
 * Sets the mouse features (VMMDEV_MOUSE_GUEST_MASK bits) this handle needs.
 */
int VbglR3SetMouseStatus(HANDLE hDevice, UINT32 fFeatures)
{
    VBOXGUEST98MOUSESTATUS Status;
    Status.fFeatures = fFeatures;
    return vbglR3DoIoCtl(hDevice, IOCTL_VBOXGUEST98_SET_MOUSE_STATUS, &Status, sizeof(Status), 0);
}

/**
 * Gets the host mouse features and the absolute pointer position (0..0xffff).
 */
int VbglR3GetMouseStatus(HANDLE hDevice, UINT32 *pfFeatures, INT32 *px, INT32 *py)
{
    VMMDevReqMouseStatus Req;
    int rc;

    vbglR3GRInit(&Req.header, sizeof(Req), VMMDevReq_GetMouseStatus);
    rc = vbglR3GRPerform(hDevice, &Req.header);
    if (RT_SUCCESS(rc))
    {
        *pfFeatures = Req.mouseFeatures;
        *px         = Req.pointerXPos;
        *py         = Req.pointerYPos;
    }
    return rc;
}

/**
 * Gets the host time in milliseconds since 1970-01-01 00:00 UTC.
 */
int VbglR3GetHostTime(HANDLE hDevice, UINT64 *pmsUnixEpoch)
{
    VMMDevReqHostTime Req;
    int rc;

    vbglR3GRInit(&Req.header, sizeof(Req), VMMDevReq_GetHostTime);
    rc = vbglR3GRPerform(hDevice, &Req.header);
    if (RT_SUCCESS(rc))
        *pmsUnixEpoch = Req.time;
    return rc;
}

/**
 * Gets the VM session ID, which changes after each start, reset or restore of
 * the VM (VbglR3QuerySessionId).
 */
int VbglR3QuerySessionId(HANDLE hDevice, UINT64 *pidSession)
{
    VMMDevReqSessionId Req;
    int rc;

    vbglR3GRInit(&Req.header, sizeof(Req), VMMDevReq_GetSessionId);
    rc = vbglR3GRPerform(hDevice, &Req.header);
    if (RT_SUCCESS(rc))
        *pidSession = Req.idSession;
    return rc;
}

/**
 * Reports the status of a guest facility to the host.
 */
int VbglR3ReportFacilityStatus(HANDLE hDevice, VBoxGuestFacilityType enmFacility, VBoxGuestFacilityStatus enmStatus)
{
    VMMDevReportGuestStatus Req;

    vbglR3GRInit(&Req.header, sizeof(Req), VMMDevReq_ReportGuestStatus);
    Req.guestStatus.facility = enmFacility;
    Req.guestStatus.status   = enmStatus;
    Req.guestStatus.flags    = 0;
    return vbglR3GRPerform(hDevice, &Req.header);
}

/**
 * Connects to an HGCM service.
 *
 * @returns VBox status code.
 * @param   hDevice         The device handle (the client belongs to its session).
 * @param   pszService      The service name.
 * @param   pidClient       Where to return the client ID.
 */
int VbglR3HgcmConnect(HANDLE hDevice, const char *pszService, UINT32 *pidClient)
{
    VBOXGUEST98HGCMCONNECT Connect;
    int rc;

    memset(&Connect, 0, sizeof(Connect));
    lstrcpyn(Connect.szServiceName, pszService, sizeof(Connect.szServiceName));
    Connect.rc = VERR_GENERAL_FAILURE;
    rc = vbglR3DoIoCtl(hDevice, IOCTL_VBOXGUEST98_HGCM_CONNECT, &Connect, sizeof(Connect), sizeof(Connect));
    if (RT_SUCCESS(rc))
        rc = Connect.rc;
    *pidClient = RT_SUCCESS(rc) ? Connect.u32ClientID : 0;
    return rc;
}

/**
 * Disconnects from an HGCM service.  This also completes calls of the client
 * still waiting in the host.
 */
int VbglR3HgcmDisconnect(HANDLE hDevice, UINT32 idClient)
{
    VBOXGUEST98HGCMDISCONNECT Disconnect;
    int rc;

    Disconnect.u32ClientID = idClient;
    Disconnect.rc          = VERR_GENERAL_FAILURE;
    rc = vbglR3DoIoCtl(hDevice, IOCTL_VBOXGUEST98_HGCM_DISCONNECT, &Disconnect, sizeof(Disconnect), sizeof(Disconnect));
    return RT_SUCCESS(rc) ? Disconnect.rc : rc;
}

/**
 * Performs an HGCM call.  The parameters and output buffers are updated in place.
 *
 * @returns VBox status code (the host result if the call itself went through).
 * @param   hDevice     The device handle.
 * @param   pCall       The call header, followed by the parameters and the data.
 * @param   cbCall      Total size of the buffer at pCall.
 */
int VbglR3HgcmCall(HANDLE hDevice, VBOXGUEST98HGCMCALL *pCall, DWORD cbCall)
{
    int rc;
    pCall->rc = VERR_GENERAL_FAILURE;
    rc = vbglR3DoIoCtl(hDevice, IOCTL_VBOXGUEST98_HGCM_CALL, pCall, cbCall, cbCall);
    return RT_SUCCESS(rc) ? pCall->rc : rc;
}

/**
 * Starts an HGCM call with cParms parameters and cbData bytes of buffers.
 *
 * @returns FALSE if out of memory.
 */
BOOL VbglR3HgcmMsgInit(VBGLR3HGCMMSG *pMsg, UINT32 idClient, UINT32 uFunction, UINT32 cParms, UINT32 cMsTimeout,
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

void VbglR3HgcmMsgSetU32(VBGLR3HGCMMSG *pMsg, UINT32 iParm, UINT32 u32)
{
    VBOXGUEST98HGCMPARM *pParm = &VBOXGUEST98_HGCM_CALL_PARMS(pMsg->pCall)[iParm];
    pParm->enmType  = VBOXGUEST98_HGCM_PARM_32BIT;
    pParm->u64Value = u32;
}

void VbglR3HgcmMsgSetU64(VBGLR3HGCMMSG *pMsg, UINT32 iParm, UINT64 u64)
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
BYTE *VbglR3HgcmMsgSetBuf(VBGLR3HGCMMSG *pMsg, UINT32 iParm, UINT32 enmType, DWORD cb, const void *pvIn)
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

UINT32 VbglR3HgcmMsgGetU32(VBGLR3HGCMMSG *pMsg, UINT32 iParm)
{
    return (UINT32)VBOXGUEST98_HGCM_CALL_PARMS(pMsg->pCall)[iParm].u64Value;
}

UINT64 VbglR3HgcmMsgGetU64(VBGLR3HGCMMSG *pMsg, UINT32 iParm)
{
    return VBOXGUEST98_HGCM_CALL_PARMS(pMsg->pCall)[iParm].u64Value;
}

/** Returns the size the host put in a buffer parameter. */
UINT32 VbglR3HgcmMsgGetBufSize(VBGLR3HGCMMSG *pMsg, UINT32 iParm)
{
    return VBOXGUEST98_HGCM_CALL_PARMS(pMsg->pCall)[iParm].cb;
}

int VbglR3HgcmMsgCall(HANDLE hDevice, VBGLR3HGCMMSG *pMsg)
{
    return VbglR3HgcmCall(hDevice, pMsg->pCall, pMsg->cbCall);
}

void VbglR3HgcmMsgFree(VBGLR3HGCMMSG *pMsg)
{
    free(pMsg->pCall);
    pMsg->pCall = NULL;
}

/**
 * Queries the statistics reporting interval wanted by the host, acknowledging
 * VMMDEV_EVENT_STATISTICS_INTERVAL_CHANGE_REQUEST (as upstream VbglR3StatQueryInterval).
 *
 * @returns VBox status code.
 * @param   hDevice         The device handle.
 * @param   pcMsInterval    Where to return the interval in milliseconds, 0 if
 *                          the host doesn't want statistics.
 */
int VbglR3StatQueryInterval(HANDLE hDevice, DWORD *pcMsInterval)
{
    VMMDevGetStatisticsChangeRequest Req;
    int rc;

    vbglR3GRInit(&Req.header, sizeof(Req), VMMDevReq_GetStatisticsChangeRequest);
    Req.eventAck        = VMMDEV_EVENT_STATISTICS_INTERVAL_CHANGE_REQUEST;
    Req.u32StatInterval = 1;
    rc = vbglR3GRPerform(hDevice, &Req.header);
    if (RT_SUCCESS(rc))
        *pcMsInterval = Req.u32StatInterval <= 0xffffffffU / 1000 ? Req.u32StatInterval * 1000 : 0xffffffffU;
    return rc;
}

/**
 * Reports guest statistics to the host.
 */
int VbglR3StatReport(HANDLE hDevice, const VBoxGuestStatistics *pStats)
{
    VMMDevReportGuestStats Req;

    vbglR3GRInit(&Req.header, sizeof(Req), VMMDevReq_ReportGuestStats);
    Req.guestStats = *pStats;
    return vbglR3GRPerform(hDevice, &Req.header);
}

/**
 * Changes the guest capabilities (VMMDEV_GUEST_SUPPORTS_XXX) this handle
 * reports.  They are withdrawn when the handle is closed.
 */
int VbglR3ChangeGuestCapabilities(HANDLE hDevice, UINT32 fOrMask, UINT32 fNotMask)
{
    VBOXGUEST98CHANGEMASK Mask;
    Mask.fOrMask  = fOrMask;
    Mask.fNotMask = fNotMask;
    return vbglR3DoIoCtl(hDevice, IOCTL_VBOXGUEST98_CHANGE_GUEST_CAPABILITIES, &Mask, sizeof(Mask), 0);
}

/**
 * Gets the display resolution the host wants (as upstream VbglR3GetDisplayChangeRequest).
 *
 * @returns VBox status code.
 * @param   hDevice     The device handle.
 * @param   pcx         Where to return the width, 0 = keep.
 * @param   pcy         Where to return the height, 0 = keep.
 * @param   pcBits      Where to return the bits per pixel, 0 = keep.
 * @param   piDisplay   Where to return the display number (0 = primary).
 * @param   pfEnabled   Where to return whether the display is enabled.
 * @param   fAck        Whether this answers VMMDEV_EVENT_DISPLAY_CHANGE_REQUEST.
 */
int VbglR3GetDisplayChangeRequest(HANDLE hDevice, UINT32 *pcx, UINT32 *pcy, UINT32 *pcBits, UINT32 *piDisplay,
                                  BOOL *pfEnabled, BOOL fAck)
{
    VMMDevDisplayChangeRequestEx Req;
    int rc;

    vbglR3GRInit(&Req.header, sizeof(Req), VMMDevReq_GetDisplayChangeRequestEx);
    if (fAck)
        Req.eventAck = VMMDEV_EVENT_DISPLAY_CHANGE_REQUEST;
    rc = vbglR3GRPerform(hDevice, &Req.header);
    if (RT_SUCCESS(rc))
    {
        *pcx       = Req.xres;
        *pcy       = Req.yres;
        *pcBits    = Req.bpp;
        *piDisplay = Req.display;
        *pfEnabled = Req.fEnabled;
    }
    return rc;
}

/**
 * Gets the seamless mode the host wants (as upstream VbglR3SeamlessGetLastEvent).
 *
 * @returns VBox status code.
 * @param   hDevice     The device handle.
 * @param   penmMode    Where to return the mode (VMMDevSeamlessMode).
 * @param   fAck        Whether this answers VMMDEV_EVENT_SEAMLESS_MODE_CHANGE_REQUEST.
 */
int VbglR3SeamlessGetMode(HANDLE hDevice, UINT32 *penmMode, BOOL fAck)
{
    VMMDevSeamlessChangeRequest Req;
    int rc;

    vbglR3GRInit(&Req.header, sizeof(Req), VMMDevReq_GetSeamlessChangeRequest);
    if (fAck)
        Req.eventAck = VMMDEV_EVENT_SEAMLESS_MODE_CHANGE_REQUEST;
    rc = vbglR3GRPerform(hDevice, &Req.header);
    if (RT_SUCCESS(rc))
        *penmMode = Req.mode;
    return rc;
}

/**
 * Sends the visible region for seamless mode (as upstream VbglR3SeamlessSendRects).
 *
 * @returns VBox status code.
 * @param   hDevice     The device handle.
 * @param   paRects     The rectangles, in guest screen pixels.
 * @param   cRects      Number of rectangles, at least 1; the request must fit
 *                      in VBOXGUEST98_MAX_VMMDEV_REQUEST_SIZE.
 */
int VbglR3SeamlessSendRects(HANDLE hDevice, const RTRECT *paRects, UINT32 cRects)
{
    VMMDevVideoSetVisibleRegion *pReq;
    UINT32 cbReq;
    int rc;

    if (!cRects || cRects > (VBOXGUEST98_MAX_VMMDEV_REQUEST_SIZE - VMMDEV_VISIBLE_REGION_SIZE(0)) / sizeof(RTRECT))
        return VERR_INVALID_PARAMETER;
    cbReq = VMMDEV_VISIBLE_REGION_SIZE(cRects);
    pReq = (VMMDevVideoSetVisibleRegion *)malloc(cbReq);
    if (!pReq)
        return VERR_NO_MEMORY;
    vbglR3GRInit(&pReq->header, cbReq, VMMDevReq_VideoSetVisibleRegion);
    pReq->cRect = cRects;
    memcpy(pReq->Rect, paRects, cRects * sizeof(RTRECT));
    rc = vbglR3GRPerform(hDevice, &pReq->header);
    free(pReq);
    return rc;
}
