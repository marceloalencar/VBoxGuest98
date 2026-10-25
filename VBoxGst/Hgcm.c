/** @file
 * VBoxGst - Host-Guest Communication Manager (HGCM) requests.
 *
 * HGCM requests usually complete asynchronously: the port write returns
 * VINF_HGCM_ASYNC_EXECUTE and the host later sets VBOX_HGCM_REQ_DONE in the
 * request and raises VMMDEV_EVENT_HGCM.  If a wait has to be abandoned the
 * request is cancelled; if the host does not confirm the cancellation the
 * request and its buffers are leaked rather than freed under the host's feet.
 *
 * Buffer parameters are bounced through nonpaged pool and passed as linear
 * addresses.  On Windows 98 the pool lives in the system arena, which is
 * mapped in every context, so the host can access it whenever it completes.
 */

#include "VBoxGst.h"


/*********************************************************************************************************************************
*   Session client tracking                                                                                                      *
*********************************************************************************************************************************/

static void vgdrvHgcmLockSessions(PVBOXGUESTDEVEXT pDevExt)
{
    KeWaitForSingleObject(&pDevExt->SessionMutex, Executive, KernelMode, FALSE, NULL);
}

static void vgdrvHgcmUnlockSessions(PVBOXGUESTDEVEXT pDevExt)
{
    KeReleaseMutex(&pDevExt->SessionMutex, FALSE);
}

/**
 * Finds the slot of a client ID in a session.
 *
 * @returns Slot index, -1 if not found.  Caller holds SessionMutex.
 */
static int vgdrvHgcmFindClientLocked(PVBOXGUESTSESSION pSession, UINT32 idClient)
{
    int i;
    for (i = 0; i < VBOXGUEST98_MAX_HGCM_CLIENTS; i++)
        if (pSession->aHGCMClientIds[i] == idClient)
            return i;
    return -1;
}

/**
 * Checks whether a session owns a client ID.
 */
static BOOLEAN vgdrvHgcmSessionHasClient(PVBOXGUESTDEVEXT pDevExt, PVBOXGUESTSESSION pSession, UINT32 idClient)
{
    BOOLEAN fFound;
    if (idClient == 0 || idClient == ~0U) /* Free and reserved slots. */
        return FALSE;
    vgdrvHgcmLockSessions(pDevExt);
    fFound = vgdrvHgcmFindClientLocked(pSession, idClient) >= 0;
    vgdrvHgcmUnlockSessions(pDevExt);
    return fFound;
}


/*********************************************************************************************************************************
*   Request completion                                                                                                           *
*********************************************************************************************************************************/

/**
 * Asks the host to cancel an HGCM request.
 */
static int vgdrvHgcmCancel(VMMDevHGCMRequestHeader *pHGCMReq)
{
    VMMDevHGCMCancel2 *pReq;
    int rc = VbglR0GRAlloc((VMMDevRequestHeader **)&pReq, sizeof(*pReq), VMMDevReq_HGCMCancel2);
    if (RT_SUCCESS(rc))
    {
        pReq->physReqToCancel = VbglR0GRPhysAddr(&pHGCMReq->header);
        rc = VbglR0GRPerform(&pReq->header);
        VbglR0GRFree(&pReq->header);
    }
    return rc;
}

/**
 * Submits an HGCM request and waits for it to complete.
 *
 * @returns VBox status code: the request status, or the HGCM result when the
 *          request itself succeeded.
 * @param   pDevExt     The device extension.
 * @param   pSession    The calling session.
 * @param   pHGCMReq    The request.
 * @param   cMsTimeout  Timeout in milliseconds or VBOXGUEST98_WAIT_INFINITE.
 * @param   pfLeak      Set to TRUE when the host may still access the
 *                      request (and its buffers), which then must not be freed.
 */
static int vgdrvHgcmPerform(PVBOXGUESTDEVEXT pDevExt, PVBOXGUESTSESSION pSession,
                            VMMDevHGCMRequestHeader *pHGCMReq, UINT32 cMsTimeout, BOOLEAN *pfLeak)
{
    int rc;

    *pfLeak = FALSE;
    pHGCMReq->fu32Flags = 0;
    pHGCMReq->result    = VERR_GENERAL_FAILURE;

    rc = VbglR0GRPerform(&pHGCMReq->header);
    if (rc == VINF_HGCM_ASYNC_EXECUTE)
    {
        rc = VGDrvCommonWaitForHgcm(pDevExt, pSession, pHGCMReq, cMsTimeout);
        if (RT_FAILURE(rc))
        {
            vgdrvHgcmCancel(pHGCMReq);
            if (   !(pHGCMReq->fu32Flags & (VBOX_HGCM_REQ_DONE | VBOX_HGCM_REQ_CANCELLED))
                && RT_FAILURE(VGDrvCommonWaitForHgcm(pDevExt, pSession, pHGCMReq, 1000)))
                *pfLeak = TRUE;
            return rc;
        }
        if (pHGCMReq->fu32Flags & VBOX_HGCM_REQ_CANCELLED)
            return VERR_INTERRUPTED;
        rc = VINF_SUCCESS;
    }
    if (RT_FAILURE(rc))
        return rc;
    return pHGCMReq->result;
}


/*********************************************************************************************************************************
*   Connect / disconnect                                                                                                         *
*********************************************************************************************************************************/

/**
 * Connects the session to an HGCM service.  PASSIVE_LEVEL.
 *
 * @returns VBox status code.
 * @param   pDevExt     The device extension.
 * @param   pSession    The calling session.
 * @param   pszService  The service name.
 * @param   pidClient   Where to return the client ID.
 */
int VGDrvHgcmConnect(PVBOXGUESTDEVEXT pDevExt, PVBOXGUESTSESSION pSession, const char *pszService, UINT32 *pidClient)
{
    VMMDevHGCMConnect *pReq;
    BOOLEAN fLeak = FALSE;
    ULONG off;
    int iSlot;
    int rc;

    *pidClient = 0;

    /* Reserve a slot first so we don't end up with an untracked client. */
    vgdrvHgcmLockSessions(pDevExt);
    iSlot = vgdrvHgcmFindClientLocked(pSession, 0);
    if (iSlot >= 0)
        pSession->aHGCMClientIds[iSlot] = ~0U;
    vgdrvHgcmUnlockSessions(pDevExt);
    if (iSlot < 0)
        return VERR_TOO_MUCH_DATA;

    rc = VbglR0GRAlloc((VMMDevRequestHeader **)&pReq, sizeof(*pReq), VMMDevReq_HGCMConnect);
    if (RT_SUCCESS(rc))
    {
        pReq->loc.type = VMMDevHGCMLoc_LocalHost_Existing;
        /* No strncpy: NTKERN doesn't export the C runtime. The buffer is zeroed. */
        for (off = 0; off < sizeof(pReq->loc.u.host.achName) - 1 && pszService[off]; off++)
            pReq->loc.u.host.achName[off] = pszService[off];
        pReq->u32ClientID = 0;

        rc = vgdrvHgcmPerform(pDevExt, pSession, &pReq->header, VBOXGUEST98_WAIT_INFINITE, &fLeak);
        if (RT_SUCCESS(rc))
            *pidClient = pReq->u32ClientID;
        if (!fLeak)
            VbglR0GRFree(&pReq->header.header);
    }

    vgdrvHgcmLockSessions(pDevExt);
    pSession->aHGCMClientIds[iSlot] = RT_SUCCESS(rc) ? *pidClient : 0;
    vgdrvHgcmUnlockSessions(pDevExt);

    return rc;
}

/**
 * Disconnects from an HGCM service without checking ownership.
 */
static int vgdrvHgcmDisconnectWorker(PVBOXGUESTDEVEXT pDevExt, PVBOXGUESTSESSION pSession, UINT32 idClient)
{
    VMMDevHGCMDisconnect *pReq;
    BOOLEAN fLeak = FALSE;
    int rc = VbglR0GRAlloc((VMMDevRequestHeader **)&pReq, sizeof(*pReq), VMMDevReq_HGCMDisconnect);
    if (RT_SUCCESS(rc))
    {
        pReq->u32ClientID = idClient;
        rc = vgdrvHgcmPerform(pDevExt, pSession, &pReq->header, VBOXGUEST98_WAIT_INFINITE, &fLeak);
        if (!fLeak)
            VbglR0GRFree(&pReq->header.header);
    }
    return rc;
}

/**
 * Disconnects one of the session's HGCM clients.  PASSIVE_LEVEL.
 *
 * @returns VBox status code, VERR_INVALID_HANDLE if the session doesn't own it.
 */
int VGDrvHgcmDisconnect(PVBOXGUESTDEVEXT pDevExt, PVBOXGUESTSESSION pSession, UINT32 idClient)
{
    int iSlot;
    int rc;

    /* Take the client out of the session while disconnecting. */
    vgdrvHgcmLockSessions(pDevExt);
    iSlot = idClient != 0 && idClient != ~0U ? vgdrvHgcmFindClientLocked(pSession, idClient) : -1;
    if (iSlot >= 0)
        pSession->aHGCMClientIds[iSlot] = ~0U;
    vgdrvHgcmUnlockSessions(pDevExt);
    if (iSlot < 0)
        return VERR_INVALID_HANDLE;

    rc = vgdrvHgcmDisconnectWorker(pDevExt, pSession, idClient);

    vgdrvHgcmLockSessions(pDevExt);
    pSession->aHGCMClientIds[iSlot] = RT_SUCCESS(rc) ? 0 : idClient;
    vgdrvHgcmUnlockSessions(pDevExt);
    return rc;
}

/**
 * Disconnects all HGCM clients of a closing session.  PASSIVE_LEVEL.
 */
void VGDrvHgcmDisconnectSessionClients(PVBOXGUESTDEVEXT pDevExt, PVBOXGUESTSESSION pSession)
{
    int i;
    for (i = 0; i < VBOXGUEST98_MAX_HGCM_CLIENTS; i++)
    {
        UINT32 idClient = pSession->aHGCMClientIds[i];
        if (idClient != 0 && idClient != ~0U)
            vgdrvHgcmDisconnectWorker(pDevExt, pSession, idClient);
        pSession->aHGCMClientIds[i] = 0;
    }
}


/*********************************************************************************************************************************
*   Call                                                                                                                         *
*********************************************************************************************************************************/

/**
 * Validates the parameters of an HGCM call and sums up the buffer sizes.
 *
 * @returns VBox status code.
 * @param   pCall       The call (in the system buffer).
 * @param   cbCall      Size of the system buffer.
 * @param   pcbData     Where to return the total buffer size.
 */
static int vgdrvHgcmValidateCall(VBOXGUEST98HGCMCALL *pCall, ULONG cbCall, ULONG *pcbData)
{
    VBOXGUEST98HGCMPARM *paParms = VBOXGUEST98_HGCM_CALL_PARMS(pCall);
    ULONG cbHdr;
    ULONG cbData = 0;
    UINT32 i;

    *pcbData = 0;
    if (pCall->cParms > VMMDEV_MAX_HGCM_PARMS)
        return VERR_INVALID_PARAMETER;
    cbHdr = sizeof(*pCall) + pCall->cParms * sizeof(VBOXGUEST98HGCMPARM);
    if (cbCall < cbHdr)
        return VERR_INVALID_PARAMETER;

    for (i = 0; i < pCall->cParms; i++)
    {
        switch (paParms[i].enmType)
        {
            case VBOXGUEST98_HGCM_PARM_32BIT:
            case VBOXGUEST98_HGCM_PARM_64BIT:
                break;

            case VBOXGUEST98_HGCM_PARM_BUFFER_IN:
            case VBOXGUEST98_HGCM_PARM_BUFFER_OUT:
            case VBOXGUEST98_HGCM_PARM_BUFFER_INOUT:
                if (   paParms[i].cb > VBOXGUEST98_MAX_HGCM_DATA_SIZE
                    || paParms[i].off < cbHdr
                    || paParms[i].off > cbCall
                    || paParms[i].cb > cbCall - paParms[i].off)
                    return VERR_INVALID_PARAMETER;
                cbData += paParms[i].cb;
                if (cbData > VBOXGUEST98_MAX_HGCM_DATA_SIZE)
                    return VERR_TOO_MUCH_DATA;
                break;

            default:
                return VERR_INVALID_PARAMETER;
        }
    }

    *pcbData = cbData;
    return VINF_SUCCESS;
}

/**
 * Performs an HGCM call on behalf of a session.  PASSIVE_LEVEL.
 *
 * Parameter values and output buffers are updated in place.
 *
 * @returns VBox status code (the host result if the request succeeded).
 * @param   pDevExt     The device extension.
 * @param   pSession    The calling session.
 * @param   pCall       The call header, parameters and data (system buffer).
 * @param   cbCall      Size of the buffer at pCall.
 */
int VGDrvHgcmCall(PVBOXGUESTDEVEXT pDevExt, PVBOXGUESTSESSION pSession, VBOXGUEST98HGCMCALL *pCall, ULONG cbCall)
{
    VBOXGUEST98HGCMPARM *paParms = VBOXGUEST98_HGCM_CALL_PARMS(pCall);
    HGCMFunctionParameter32 *paReqParms;
    VMMDevHGCMCall *pReq = NULL;
    PUCHAR pbBounce = NULL;
    PUCHAR pbCur;
    BOOLEAN fLeak = FALSE;
    ULONG cbData;
    UINT32 i;
    int rc;

    rc = vgdrvHgcmValidateCall(pCall, cbCall, &cbData);
    if (RT_FAILURE(rc))
        return rc;
    if (!vgdrvHgcmSessionHasClient(pDevExt, pSession, pCall->u32ClientID))
        return VERR_INVALID_HANDLE;

    if (cbData)
    {
        pbBounce = (PUCHAR)ExAllocatePool(NonPagedPool, cbData);
        if (!pbBounce)
            return VERR_NO_MEMORY;
    }

    rc = VbglR0GRAlloc((VMMDevRequestHeader **)&pReq,
                       sizeof(VMMDevHGCMCall) + pCall->cParms * sizeof(HGCMFunctionParameter32),
                       VMMDevReq_HGCMCall32);
    if (RT_FAILURE(rc))
    {
        if (pbBounce)
            ExFreePool(pbBounce);
        return rc;
    }

    /*
     * Translate the parameters, copying input buffers.
     */
    pReq->u32ClientID = pCall->u32ClientID;
    pReq->u32Function = pCall->u32Function;
    pReq->cParms      = pCall->cParms;
    paReqParms = (HGCMFunctionParameter32 *)(pReq + 1);
    pbCur = pbBounce;
    for (i = 0; i < pCall->cParms; i++)
    {
        switch (paParms[i].enmType)
        {
            case VBOXGUEST98_HGCM_PARM_32BIT:
                paReqParms[i].type = VMMDevHGCMParmType_32bit;
                paReqParms[i].u.value32 = (UINT32)paParms[i].u64Value;
                break;

            case VBOXGUEST98_HGCM_PARM_64BIT:
                paReqParms[i].type = VMMDevHGCMParmType_64bit;
                paReqParms[i].u.value64 = paParms[i].u64Value;
                break;

            default: /* Buffers, validated above. */
                if (paParms[i].enmType == VBOXGUEST98_HGCM_PARM_BUFFER_IN)
                    paReqParms[i].type = VMMDevHGCMParmType_LinAddr_In;
                else if (paParms[i].enmType == VBOXGUEST98_HGCM_PARM_BUFFER_OUT)
                    paReqParms[i].type = VMMDevHGCMParmType_LinAddr_Out;
                else
                    paReqParms[i].type = VMMDevHGCMParmType_LinAddr;
                paReqParms[i].u.Pointer.size = paParms[i].cb;
                paReqParms[i].u.Pointer.u.linearAddr = (UINT32)(ULONG_PTR)pbCur;
                if (paParms[i].enmType != VBOXGUEST98_HGCM_PARM_BUFFER_OUT)
                    RtlCopyMemory(pbCur, (PUCHAR)pCall + paParms[i].off, paParms[i].cb);
                else
                    RtlZeroMemory(pbCur, paParms[i].cb);
                pbCur += paParms[i].cb;
                break;
        }
    }

    rc = vgdrvHgcmPerform(pDevExt, pSession, &pReq->header, pCall->cMsTimeout, &fLeak);

    /*
     * Copy back values and output buffers.
     */
    if (RT_SUCCESS(rc) || (!fLeak && (pReq->header.fu32Flags & VBOX_HGCM_REQ_DONE)))
    {
        pbCur = pbBounce;
        for (i = 0; i < pCall->cParms; i++)
        {
            switch (paParms[i].enmType)
            {
                case VBOXGUEST98_HGCM_PARM_32BIT:
                    paParms[i].u64Value = paReqParms[i].u.value32;
                    break;

                case VBOXGUEST98_HGCM_PARM_64BIT:
                    paParms[i].u64Value = paReqParms[i].u.value64;
                    break;

                default:
                    if (paParms[i].enmType != VBOXGUEST98_HGCM_PARM_BUFFER_IN)
                    {
                        UINT32 cbOut = paReqParms[i].u.Pointer.size;
                        RtlCopyMemory((PUCHAR)pCall + paParms[i].off, pbCur,
                                      cbOut < paParms[i].cb ? cbOut : paParms[i].cb);
                    }
                    pbCur += paParms[i].cb;
                    /* The host may report the size it needs (VERR_BUFFER_OVERFLOW). */
                    paParms[i].cb = paReqParms[i].u.Pointer.size;
                    break;
            }
        }
    }

    if (!fLeak)
    {
        VbglR0GRFree(&pReq->header.header);
        if (pbBounce)
            ExFreePool(pbBounce);
    }
    return rc;
}
