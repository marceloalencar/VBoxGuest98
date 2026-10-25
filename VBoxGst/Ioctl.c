/** @file
 * VBoxGst - Device control (IRP_MJ_DEVICE_CONTROL) and internal device
 * control (IRP_MJ_INTERNAL_DEVICE_CONTROL) dispatching.
 *
 * All I/O controls are METHOD_BUFFERED and processed synchronously at
 * PASSIVE_LEVEL in the context of the caller.  See VBoxGuest98.h.
 */

#include "VBoxGst.h"


/**
 * Checks whether a raw VMMDev request may be issued by a client.
 *
 * Requests that change state the driver manages on behalf of all sessions,
 * or that complete asynchronously, have dedicated I/O controls.
 */
static BOOLEAN vgdrvIsVMMDevRequestAllowed(VMMDevRequestType enmType)
{
    switch (enmType)
    {
        case VMMDevReq_InvalidRequest:
        case VMMDevReq_AcknowledgeEvents:
        case VMMDevReq_CtlGuestFilterMask:
        case VMMDevReq_SetGuestCapabilities:
        case VMMDevReq_ReportGuestCapabilities:
        case VMMDevReq_SetMouseStatus:
        case VMMDevReq_ReportGuestInfo:
        case VMMDevReq_ReportGuestInfo2:
        case VMMDevReq_HGCMConnect:
        case VMMDevReq_HGCMDisconnect:
        case VMMDevReq_HGCMCall32:
        case VMMDevReq_HGCMCall64:
        case VMMDevReq_HGCMCancel:
        case VMMDevReq_HGCMCancel2:
        case VMMDevReq_GuestHeartbeat:
        case VMMDevReq_HeartbeatConfigure:
            return FALSE;
        default:
            return TRUE;
    }
}

/**
 * IOCTL_VBOXGUEST98_GET_VERSION_INFO.
 */
static NTSTATUS vgdrvIoCtlGetVersionInfo(PVBOXGUESTDEVEXT pDevExt, PVOID pvBuf, ULONG cbOut, ULONG_PTR *pcbRet)
{
    VBOXGUEST98VERSIONINFO *pInfo = (VBOXGUEST98VERSIONINFO *)pvBuf;

    if (cbOut < sizeof(*pInfo))
        return STATUS_BUFFER_TOO_SMALL;

    RtlZeroMemory(pInfo, sizeof(*pInfo));
    pInfo->u32InterfaceVersion  = VBOXGUEST98_IOCTL_VERSION;
    pInfo->uDriverMajor         = VBOX_VERSION_MAJOR;
    pInfo->uDriverMinor         = VBOX_VERSION_MINOR;
    pInfo->uDriverBuild         = VBOX_VERSION_BUILD;
    pInfo->uDriverRevision      = VBOX_SVN_REV;
    pInfo->uHostMajor           = g_vbgldata.hostVersion.major;
    pInfo->uHostMinor           = g_vbgldata.hostVersion.minor;
    pInfo->uHostBuild           = g_vbgldata.hostVersion.build;
    pInfo->uHostRevision        = g_vbgldata.hostVersion.revision;
    pInfo->fHostFeatures        = pDevExt->fHostFeatures;
    pInfo->enmOsType            = (UINT32)pDevExt->enmOsType;
    pInfo->fFixedEvents         = pDevExt->fFixedEvents;
    pInfo->cMsHeartbeatInterval = pDevExt->cMsHeartbeatInterval;
    pInfo->rcHeartbeat          = pDevExt->rcHeartbeat;

    *pcbRet = sizeof(*pInfo);
    return STATUS_SUCCESS;
}

/**
 * IOCTL_VBOXGUEST98_VMMDEV_REQUEST.
 *
 * The request is copied into physically contiguous memory, performed and
 * copied back.  The status is returned in the request header.
 */
static NTSTATUS vgdrvIoCtlVMMDevRequest(PVBOXGUESTSESSION pSession, PVOID pvBuf, ULONG cbIn, ULONG cbOut,
                                        ULONG_PTR *pcbRet)
{
    VMMDevRequestHeader *pReqIn = (VMMDevRequestHeader *)pvBuf;
    VMMDevRequestHeader *pReq;
    ULONG cbReq;
    int rc;

    if (cbIn < sizeof(VMMDevRequestHeader))
        return STATUS_INVALID_PARAMETER;
    cbReq = pReqIn->size;
    if (   cbReq < sizeof(VMMDevRequestHeader)
        || cbReq > cbIn
        || cbReq > VBOXGUEST98_MAX_VMMDEV_REQUEST_SIZE
        || pReqIn->version != VMMDEV_REQUEST_HEADER_VERSION)
        return STATUS_INVALID_PARAMETER;
    if (cbOut < cbReq)
        return STATUS_BUFFER_TOO_SMALL;
    if (!vgdrvIsVMMDevRequestAllowed(pReqIn->requestType))
        return STATUS_ACCESS_DENIED;

    rc = VbglR0GRAlloc(&pReq, cbReq, pReqIn->requestType);
    if (RT_FAILURE(rc))
        return vgdrvRcToNtStatus(rc);

    RtlCopyMemory(pReq, pReqIn, cbReq);
    pReq->rc        = VERR_GENERAL_FAILURE;
    pReq->reserved1 = 0;
    pReq->fRequestor = pSession->fRequestor;

    VbglR0GRPerform(pReq);

    RtlCopyMemory(pReqIn, pReq, cbReq);
    VbglR0GRFree(pReq);

    *pcbRet = cbReq;
    return STATUS_SUCCESS;
}

/**
 * IOCTL_VBOXGUEST98_WAIT_FOR_EVENTS.
 */
static NTSTATUS vgdrvIoCtlWaitForEvents(PVBOXGUESTDEVEXT pDevExt, PVBOXGUESTSESSION pSession, PVOID pvBuf,
                                        ULONG cbIn, ULONG cbOut, ULONG_PTR *pcbRet)
{
    VBOXGUEST98WAITEVENTS *pWait = (VBOXGUEST98WAITEVENTS *)pvBuf;
    UINT32 fEvents = 0;
    int rc;

    if (cbIn < sizeof(*pWait) || cbOut < sizeof(*pWait))
        return STATUS_INVALID_PARAMETER;

    rc = VGDrvCommonWaitForEvents(pDevExt, pSession, pWait->fEvents, pWait->cMsTimeout, &fEvents);
    if (rc == VERR_INVALID_PARAMETER)
        return STATUS_INVALID_PARAMETER;

    pWait->fEvents = fEvents;
    pWait->rc      = rc;
    *pcbRet = sizeof(*pWait);
    return STATUS_SUCCESS;
}

/**
 * IOCTL_VBOXGUEST98_HGCM_CONNECT.
 */
static NTSTATUS vgdrvIoCtlHgcmConnect(PVBOXGUESTDEVEXT pDevExt, PVBOXGUESTSESSION pSession, PVOID pvBuf,
                                      ULONG cbIn, ULONG cbOut, ULONG_PTR *pcbRet)
{
    VBOXGUEST98HGCMCONNECT *pConnect = (VBOXGUEST98HGCMCONNECT *)pvBuf;
    UINT32 idClient = 0;

    if (cbIn < sizeof(*pConnect) || cbOut < sizeof(*pConnect))
        return STATUS_INVALID_PARAMETER;

    pConnect->szServiceName[sizeof(pConnect->szServiceName) - 1] = '\0';
    pConnect->rc = VGDrvHgcmConnect(pDevExt, pSession, pConnect->szServiceName, &idClient);
    pConnect->u32ClientID = idClient;
    *pcbRet = sizeof(*pConnect);
    return STATUS_SUCCESS;
}

/**
 * IOCTL_VBOXGUEST98_HGCM_DISCONNECT.
 */
static NTSTATUS vgdrvIoCtlHgcmDisconnect(PVBOXGUESTDEVEXT pDevExt, PVBOXGUESTSESSION pSession, PVOID pvBuf,
                                         ULONG cbIn, ULONG cbOut, ULONG_PTR *pcbRet)
{
    VBOXGUEST98HGCMDISCONNECT *pDisconnect = (VBOXGUEST98HGCMDISCONNECT *)pvBuf;

    if (cbIn < sizeof(*pDisconnect) || cbOut < sizeof(*pDisconnect))
        return STATUS_INVALID_PARAMETER;

    pDisconnect->rc = VGDrvHgcmDisconnect(pDevExt, pSession, pDisconnect->u32ClientID);
    *pcbRet = sizeof(*pDisconnect);
    return STATUS_SUCCESS;
}

/**
 * IOCTL_VBOXGUEST98_HGCM_CALL.  The output buffer receives the updated copy
 * of the input buffer.
 */
static NTSTATUS vgdrvIoCtlHgcmCall(PVBOXGUESTDEVEXT pDevExt, PVBOXGUESTSESSION pSession, PVOID pvBuf,
                                   ULONG cbIn, ULONG cbOut, ULONG_PTR *pcbRet)
{
    VBOXGUEST98HGCMCALL *pCall = (VBOXGUEST98HGCMCALL *)pvBuf;

    if (cbIn < sizeof(*pCall))
        return STATUS_INVALID_PARAMETER;
    if (cbOut < cbIn)
        return STATUS_BUFFER_TOO_SMALL;

    pCall->rc = VGDrvHgcmCall(pDevExt, pSession, pCall, cbIn);
    *pcbRet = cbIn;
    return STATUS_SUCCESS;
}

/**
 * Device I/O Control entry point.
 *
 * @param   pDevObj     Device object.
 * @param   pIrp        Request packet.
 */
NTSTATUS NTAPI vgdrvWinIoCtl(PDEVICE_OBJECT pDevObj, PIRP pIrp)
{
    PVBOXGUESTDEVEXTWIN pDevExtWin = (PVBOXGUESTDEVEXTWIN)pDevObj->DeviceExtension;
    PVBOXGUESTDEVEXT pDevExt = &pDevExtWin->Core;
    PIO_STACK_LOCATION pStack = IoGetCurrentIrpStackLocation(pIrp);
    ULONG uCmd  = pStack->Parameters.DeviceIoControl.IoControlCode;
    ULONG cbIn  = pStack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG cbOut = pStack->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID pvBuf = pIrp->AssociatedIrp.SystemBuffer;
    PVBOXGUESTSESSION pSession = NULL;
    VBOXGUEST98CHANGEMASK *pMask = (VBOXGUEST98CHANGEMASK *)pvBuf;
    ULONG_PTR cbRet = 0;
    NTSTATUS rcNt;

    /* Kernel clients may send requests without a file object. */
    if (pStack->FileObject && pStack->FileObject->FsContext)
        pSession = (PVBOXGUESTSESSION)pStack->FileObject->FsContext;
    else if (pIrp->RequestorMode == KernelMode)
        pSession = pDevExt->pKernelSession;
    if (!pSession)
        return vgdrvWinCompleteRequest(pIrp, STATUS_INVALID_PARAMETER, 0);

    if (   pDevExtWin->enmDevState != VGDRVWINDEVSTATE_STARTED
        && uCmd != IOCTL_VBOXGUEST98_GET_VERSION_INFO
        && uCmd != IOCTL_VBOXGUEST98_WRITE_LOG)
        return vgdrvWinCompleteRequest(pIrp, STATUS_DEVICE_NOT_READY, 0);
    if (!pvBuf && (cbIn || cbOut))
        return vgdrvWinCompleteRequest(pIrp, STATUS_INVALID_PARAMETER, 0);

    switch (uCmd)
    {
        case IOCTL_VBOXGUEST98_GET_VERSION_INFO:
            rcNt = vgdrvIoCtlGetVersionInfo(pDevExt, pvBuf, cbOut, &cbRet);
            break;

        case IOCTL_VBOXGUEST98_VMMDEV_REQUEST:
            rcNt = vgdrvIoCtlVMMDevRequest(pSession, pvBuf, cbIn, cbOut, &cbRet);
            break;

        case IOCTL_VBOXGUEST98_WAIT_FOR_EVENTS:
            rcNt = vgdrvIoCtlWaitForEvents(pDevExt, pSession, pvBuf, cbIn, cbOut, &cbRet);
            break;

        case IOCTL_VBOXGUEST98_INTERRUPT_WAITS:
            VGDrvCommonInterruptWaits(pDevExt, pSession);
            rcNt = STATUS_SUCCESS;
            break;

        case IOCTL_VBOXGUEST98_CHANGE_FILTER_MASK:
            if (cbIn < sizeof(*pMask))
                rcNt = STATUS_INVALID_PARAMETER;
            else
                rcNt = vgdrvRcToNtStatus(VGDrvCommonSetSessionEventFilter(pDevExt, pSession,
                                                                          pMask->fOrMask, pMask->fNotMask));
            break;

        case IOCTL_VBOXGUEST98_CHANGE_GUEST_CAPABILITIES:
            if (cbIn < sizeof(*pMask))
                rcNt = STATUS_INVALID_PARAMETER;
            else
                rcNt = vgdrvRcToNtStatus(VGDrvCommonSetSessionCapabilities(pDevExt, pSession,
                                                                           pMask->fOrMask, pMask->fNotMask));
            break;

        case IOCTL_VBOXGUEST98_SET_MOUSE_STATUS:
            if (cbIn < sizeof(VBOXGUEST98MOUSESTATUS))
                rcNt = STATUS_INVALID_PARAMETER;
            else
                rcNt = vgdrvRcToNtStatus(VGDrvCommonSetSessionMouseStatus(pDevExt, pSession,
                                                                          ((VBOXGUEST98MOUSESTATUS *)pvBuf)->fFeatures));
            break;

        case IOCTL_VBOXGUEST98_HGCM_CONNECT:
            rcNt = vgdrvIoCtlHgcmConnect(pDevExt, pSession, pvBuf, cbIn, cbOut, &cbRet);
            break;

        case IOCTL_VBOXGUEST98_HGCM_DISCONNECT:
            rcNt = vgdrvIoCtlHgcmDisconnect(pDevExt, pSession, pvBuf, cbIn, cbOut, &cbRet);
            break;

        case IOCTL_VBOXGUEST98_HGCM_CALL:
            rcNt = vgdrvIoCtlHgcmCall(pDevExt, pSession, pvBuf, cbIn, cbOut, &cbRet);
            break;

        case IOCTL_VBOXGUEST98_WRITE_LOG:
            if (cbIn > VBOXGUEST98_MAX_LOG_SIZE)
                rcNt = STATUS_INVALID_PARAMETER;
            else
            {
                VGDrvLogWrite((const char *)pvBuf, cbIn);
                rcNt = STATUS_SUCCESS;
            }
            break;

        default:
            rcNt = STATUS_INVALID_DEVICE_REQUEST;
            break;
    }

    return vgdrvWinCompleteRequest(pIrp, rcNt, NT_SUCCESS(rcNt) ? cbRet : 0);
}

/**
 * Internal Device I/O Control entry point (kernel mode clients).
 *
 * @param   pDevObj     Device object.
 * @param   pIrp        Request packet.
 */
NTSTATUS NTAPI vgdrvWinInternalIoCtl(PDEVICE_OBJECT pDevObj, PIRP pIrp)
{
    PVBOXGUESTDEVEXTWIN pDevExtWin = (PVBOXGUESTDEVEXTWIN)pDevObj->DeviceExtension;
    PVBOXGUESTDEVEXT pDevExt = &pDevExtWin->Core;
    PIO_STACK_LOCATION pStack = IoGetCurrentIrpStackLocation(pIrp);
    ULONG cbIn  = pStack->Parameters.DeviceIoControl.InputBufferLength;
    PVOID pvBuf = pIrp->AssociatedIrp.SystemBuffer;
    NTSTATUS rcNt;

    if (pDevExtWin->enmDevState == VGDRVWINDEVSTATE_REMOVED)
        return vgdrvWinCompleteRequest(pIrp, STATUS_DELETE_PENDING, 0);

    switch (pStack->Parameters.DeviceIoControl.IoControlCode)
    {
        case IOCTL_VBOXGUEST98_INTERNAL_SET_MOUSE_NOTIFY:
            if (!pvBuf || cbIn < sizeof(VBOXGUEST98MOUSENOTIFY))
                rcNt = STATUS_INVALID_PARAMETER;
            else
            {
                VBOXGUEST98MOUSENOTIFY *pNotify = (VBOXGUEST98MOUSENOTIFY *)pvBuf;
                rcNt = vgdrvRcToNtStatus(VGDrvCommonSetMouseNotifyCallback(pDevExt, pNotify->pfnNotify,
                                                                           pNotify->pvUser));
            }
            break;

        case IOCTL_VBOXGUEST98_INTERNAL_SET_MOUSE_STATUS:
            if (!pvBuf || cbIn < sizeof(VBOXGUEST98MOUSESTATUS))
                rcNt = STATUS_INVALID_PARAMETER;
            else
                rcNt = vgdrvRcToNtStatus(VGDrvCommonSetSessionMouseStatus(pDevExt, pDevExt->pKernelSession,
                                                                          ((VBOXGUEST98MOUSESTATUS *)pvBuf)->fFeatures));
            break;

        default:
            rcNt = STATUS_INVALID_DEVICE_REQUEST;
            break;
    }

    return vgdrvWinCompleteRequest(pIrp, rcNt, 0);
}
