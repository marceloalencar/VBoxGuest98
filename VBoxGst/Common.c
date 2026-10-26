/** @file
 * VBoxGst - OS independent core: host initialization, sessions, events and
 * heartbeat.  Follows src/VBox/Additions/common/VBoxGuest/VBoxGuest.cpp.
 */

#include "VBoxGst.h"


/*********************************************************************************************************************************
*   Helpers                                                                                                                      *
*********************************************************************************************************************************/

/**
 * Converts a VBox status code to an NT status code.
 */
NTSTATUS vgdrvRcToNtStatus(int rc)
{
    switch (rc)
    {
        case VINF_SUCCESS:              return STATUS_SUCCESS;
        case VERR_INVALID_PARAMETER:    return STATUS_INVALID_PARAMETER;
        case VERR_INVALID_POINTER:      return STATUS_INVALID_PARAMETER;
        case VERR_INVALID_HANDLE:       return STATUS_INVALID_HANDLE;
        case VERR_NO_MEMORY:            return STATUS_INSUFFICIENT_RESOURCES;
        case VERR_PERMISSION_DENIED:    return STATUS_ACCESS_DENIED;
        case VERR_NOT_SUPPORTED:        return STATUS_NOT_SUPPORTED;
        case VERR_NOT_IMPLEMENTED:      return STATUS_NOT_IMPLEMENTED;
        case VERR_TIMEOUT:              return STATUS_IO_TIMEOUT;
        case VERR_INTERRUPTED:          return STATUS_CANCELLED;
        case VERR_BUFFER_OVERFLOW:      return STATUS_BUFFER_TOO_SMALL;
        case VERR_TOO_MUCH_DATA:        return STATUS_BUFFER_TOO_SMALL;
        case VERR_NOT_FOUND:            return STATUS_OBJECT_NAME_NOT_FOUND;
        default:
            return RT_SUCCESS(rc) ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
    }
}

static void vgdrvAcquireSessionMutex(PVBOXGUESTDEVEXT pDevExt)
{
    KeWaitForSingleObject(&pDevExt->SessionMutex, Executive, KernelMode, FALSE, NULL);
}

static void vgdrvReleaseSessionMutex(PVBOXGUESTDEVEXT pDevExt)
{
    KeReleaseMutex(&pDevExt->SessionMutex, FALSE);
}

/**
 * Applies a change of one session's mask to a usage tracker.
 *
 * @returns TRUE if the combined mask changed.
 * @param   pTracker    The tracker.
 * @param   fChanged    The bits that changed in the session.
 * @param   fPrevious   The session mask before the change.
 */
static BOOLEAN vgdrvBitUsageTrackerChange(VBOXGUESTBITUSAGETRACER *pTracker, UINT32 fChanged, UINT32 fPrevious)
{
    BOOLEAN fGlobalChange = FALSE;
    UINT32 iBit;

    for (iBit = 0; iBit < 32; iBit++)
    {
        UINT32 fBit = 1U << iBit;
        if (!(fChanged & fBit))
            continue;
        if (fPrevious & fBit)
        {
            if (--pTracker->acPerBitUsage[iBit] == 0)
            {
                pTracker->fMask &= ~fBit;
                fGlobalChange = TRUE;
            }
        }
        else
        {
            if (++pTracker->acPerBitUsage[iBit] == 1)
            {
                pTracker->fMask |= fBit;
                fGlobalChange = TRUE;
            }
        }
    }
    return fGlobalChange;
}


/*********************************************************************************************************************************
*   Host requests                                                                                                                *
*********************************************************************************************************************************/

/**
 * Sets the event filter mask on the host (which events raise interrupts).
 */
static int vgdrvSetHostEventFilter(UINT32 fFilter)
{
    VMMDevCtlGuestFilterMask *pReq;
    int rc = VbglR0GRAlloc((VMMDevRequestHeader **)&pReq, sizeof(*pReq), VMMDevReq_CtlGuestFilterMask);
    if (RT_SUCCESS(rc))
    {
        pReq->u32OrMask  = fFilter;
        pReq->u32NotMask = ~fFilter;
        rc = VbglR0GRPerform(&pReq->header);
        VbglR0GRFree(&pReq->header);
    }
    return rc;
}

/**
 * Sets the guest capabilities (VMMDEV_GUEST_SUPPORTS_XXX) on the host.
 */
static int vgdrvSetHostCapabilities(UINT32 fCaps)
{
    VMMDevReqGuestCapabilities2 *pReq;
    int rc = VbglR0GRAlloc((VMMDevRequestHeader **)&pReq, sizeof(*pReq), VMMDevReq_SetGuestCapabilities);
    if (RT_SUCCESS(rc))
    {
        pReq->u32OrMask  = fCaps;
        pReq->u32NotMask = ~fCaps;
        rc = VbglR0GRPerform(&pReq->header);
        VbglR0GRFree(&pReq->header);
    }
    return rc;
}

/**
 * Sets the guest mouse features (VMMDEV_MOUSE_GUEST_MASK bits) on the host.
 */
static int vgdrvSetHostMouseStatus(UINT32 fFeatures)
{
    VMMDevReqMouseStatus *pReq;
    int rc = VbglR0GRAlloc((VMMDevRequestHeader **)&pReq, sizeof(*pReq), VMMDevReq_SetMouseStatus);
    if (RT_SUCCESS(rc))
    {
        pReq->mouseFeatures = fFeatures & VMMDEV_MOUSE_GUEST_MASK;
        pReq->pointerXPos   = 0;
        pReq->pointerYPos   = 0;
        rc = VbglR0GRPerform(&pReq->header);
        VbglR0GRFree(&pReq->header);
    }
    return rc;
}

/**
 * Reports the guest information to the host.
 *
 * There are two protocols: INFO2 + INFO1 (hosts >= 3.2.51) and INFO1 with an
 * optional INFO2 (older hosts).  INFO2 first fails with VERR_NOT_SUPPORTED on
 * hosts that need the old ordering.
 */
static int vgdrvReportGuestInfo(VBOXOSTYPE enmOsType)
{
    VMMDevReportGuestInfo2 *pReqInfo2 = NULL;
    VMMDevReportGuestInfo  *pReqInfo1 = NULL;
    int rc;

    rc = VbglR0GRAlloc((VMMDevRequestHeader **)&pReqInfo2, sizeof(*pReqInfo2), VMMDevReq_ReportGuestInfo2);
    if (RT_SUCCESS(rc))
    {
        pReqInfo2->additionsMajor    = VBOX_VERSION_MAJOR;
        pReqInfo2->additionsMinor    = VBOX_VERSION_MINOR;
        pReqInfo2->additionsBuild    = VBOX_VERSION_BUILD;
        pReqInfo2->additionsRevision = VBOX_SVN_REV;
        pReqInfo2->additionsFeatures = VBOXGSTINFO2_F_REQUESTOR_INFO;
        RtlCopyMemory(pReqInfo2->szName, VBOX_VERSION_STRING, sizeof(VBOX_VERSION_STRING));

        rc = VbglR0GRAlloc((VMMDevRequestHeader **)&pReqInfo1, sizeof(*pReqInfo1), VMMDevReq_ReportGuestInfo);
        if (RT_SUCCESS(rc))
        {
            pReqInfo1->interfaceVersion = VMMDEV_VERSION;
            pReqInfo1->osType           = enmOsType;

            rc = VbglR0GRPerform(&pReqInfo2->header);
            if (RT_SUCCESS(rc))
                rc = VbglR0GRPerform(&pReqInfo1->header);
            else if (rc == VERR_NOT_SUPPORTED || rc == VERR_NOT_IMPLEMENTED)
            {
                rc = VbglR0GRPerform(&pReqInfo1->header);
                if (RT_SUCCESS(rc))
                {
                    rc = VbglR0GRPerform(&pReqInfo2->header);
                    if (rc == VERR_NOT_IMPLEMENTED)
                        rc = VINF_SUCCESS;
                }
            }
            VbglR0GRFree(&pReqInfo1->header);
        }
        VbglR0GRFree(&pReqInfo2->header);
    }
    return rc;
}

/**
 * Reports the status of the guest driver facility to the host.
 */
static int vgdrvReportDriverStatus(VBoxGuestFacilityStatus enmStatus)
{
    VMMDevReportGuestStatus *pReq;
    int rc = VbglR0GRAlloc((VMMDevRequestHeader **)&pReq, sizeof(*pReq), VMMDevReq_ReportGuestStatus);
    if (RT_SUCCESS(rc))
    {
        pReq->guestStatus.facility = VBoxGuestFacilityType_VBoxGuestDriver;
        pReq->guestStatus.status   = enmStatus;
        pReq->guestStatus.flags    = 0;
        rc = VbglR0GRPerform(&pReq->header);
        if (rc == VERR_NOT_IMPLEMENTED) /* Compatibility with older hosts. */
            rc = VINF_SUCCESS;
        VbglR0GRFree(&pReq->header);
    }
    return rc;
}

/**
 * Tells the host the guest is powering off.  Callable at any IRQL.
 */
void VGDrvCommonReportPowerOff(PVBOXGUESTDEVEXT pDevExt)
{
    VMMDevPowerStateRequest *pReq = pDevExt->pPowerStateReq;
    if (pReq)
    {
        pReq->header.rc = VERR_GENERAL_FAILURE;
        pReq->powerState = VMMDevPowerState_PowerOff;
        VGDrvLogRel("Windows is shutting down, powering off the VM\n");
        VbglR0GRPerform(&pReq->header);
    }
}


/*********************************************************************************************************************************
*   Heartbeat                                                                                                                    *
*********************************************************************************************************************************/

/**
 * Heartbeat timer DPC: tells the host the guest is alive.
 */
static VOID NTAPI vgdrvHeartbeatDpc(PKDPC pDpc, PVOID pvContext, PVOID pvArg1, PVOID pvArg2)
{
    PVBOXGUESTDEVEXT pDevExt = (PVBOXGUESTDEVEXT)pvContext;
    if (pDevExt->pHeartbeatReq)
    {
        pDevExt->pHeartbeatReq->rc = VERR_GENERAL_FAILURE;
        VbglR0GRPerform(pDevExt->pHeartbeatReq);
    }
}

/**
 * Enables or disables the heartbeat on the host.
 *
 * @param   fEnabled        Whether to enable it.
 * @param   pcMsInterval    Where to return the interval the host wants (optional).
 */
static int vgdrvHeartbeatHostConfigure(BOOLEAN fEnabled, UINT32 *pcMsInterval)
{
    VMMDevReqHeartbeat *pReq;
    int rc = VbglR0GRAlloc((VMMDevRequestHeader **)&pReq, sizeof(*pReq), VMMDevReq_HeartbeatConfigure);
    if (RT_SUCCESS(rc))
    {
        pReq->fEnabled = fEnabled;
        rc = VbglR0GRPerform(&pReq->header);
        if (RT_SUCCESS(rc) && pcMsInterval)
        {
            /* 64-bit division needs runtime helpers, so split by hand
               (2^32 ns is ~4295 ms). */
            ULARGE_INTEGER cNs;
            cNs.QuadPart = pReq->cNsInterval;
            *pcMsInterval = cNs.HighPart * 4295 + cNs.LowPart / 1000000;
        }
        VbglR0GRFree(&pReq->header);
    }
    return rc;
}

/**
 * Starts the heartbeat if the host supports it.  Failures are not fatal.
 */
static void vgdrvHeartbeatInit(PVBOXGUESTDEVEXT pDevExt)
{
    LARGE_INTEGER DueTime;
    UINT32 cMsInterval = 0;
    int rc;

    rc = VbglR0GRAlloc(&pDevExt->pHeartbeatReq, sizeof(VMMDevRequestHeader), VMMDevReq_GuestHeartbeat);
    if (RT_FAILURE(rc))
    {
        VGDrvLogRel("heartbeat request allocation failed, rc=%d\n", rc);
        return;
    }

    rc = vgdrvHeartbeatHostConfigure(TRUE, &cMsInterval);
    if (RT_FAILURE(rc) || cMsInterval == 0)
    {
        VGDrvLogRel("heartbeat not started, rc=%d\n", rc);
        VbglR0GRFree(pDevExt->pHeartbeatReq);
        pDevExt->pHeartbeatReq = NULL;
        return;
    }

    VGDrvLogRel("heartbeat every %u ms\n", cMsInterval);
    VbglR0GRPerform(pDevExt->pHeartbeatReq);

    DueTime.QuadPart = Int32x32To64(cMsInterval, -10000);
    KeSetTimerEx(&pDevExt->HeartbeatTimer, DueTime, (LONG)cMsInterval, &pDevExt->HeartbeatDpc);
}

/**
 * Stops the heartbeat.
 */
static void vgdrvHeartbeatTerm(PVBOXGUESTDEVEXT pDevExt)
{
    if (!pDevExt->pHeartbeatReq)
        return;

    KeCancelTimer(&pDevExt->HeartbeatTimer);
    KeRemoveQueueDpc(&pDevExt->HeartbeatDpc);
    vgdrvHeartbeatHostConfigure(FALSE, NULL);

    VbglR0GRFree(pDevExt->pHeartbeatReq);
    pDevExt->pHeartbeatReq = NULL;
}


/*********************************************************************************************************************************
*   Device extension                                                                                                             *
*********************************************************************************************************************************/

/**
 * Initialize the device extension fundament.
 *
 * There are no device resources at this point, VGDrvCommonInitDevExtResources
 * should be called when they are available.  The extension must be zeroed.
 *
 * @returns VBox status code.
 * @param   pDevExt         The device extension to init.
 * @param   enmOsType       The guest OS type to report.
 */
int VGDrvCommonInitDevExtFundament(PVBOXGUESTDEVEXT pDevExt, VBOXOSTYPE enmOsType)
{
    int rc;

    pDevExt->enmOsType     = enmOsType;
    pDevExt->fHostFeatures = 0;

    KeInitializeSpinLock(&pDevExt->EventSpinLock);
    InitializeListHead(&pDevExt->WaitList);
    KeInitializeMutex(&pDevExt->SessionMutex, 0);
    InitializeListHead(&pDevExt->SessionList);
    KeInitializeSpinLock(&pDevExt->MouseEventAccessSpinLock);
    KeInitializeTimer(&pDevExt->HeartbeatTimer);
    KeInitializeDpc(&pDevExt->HeartbeatDpc, vgdrvHeartbeatDpc, pDevExt);

    rc = VGDrvCommonCreateSession(pDevExt, FALSE, &pDevExt->pKernelSession);
    if (RT_SUCCESS(rc))
        pDevExt->uInitState = VBOXGUESTDEVEXT_INIT_STATE_FUNDAMENT;
    return rc;
}

/**
 * Counter to VGDrvCommonInitDevExtFundament.
 *
 * @param   pDevExt         The device extension.
 */
void VGDrvCommonDeleteDevExtFundament(PVBOXGUESTDEVEXT pDevExt)
{
    if (pDevExt->pKernelSession)
    {
        VGDrvCommonCloseSession(pDevExt, pDevExt->pKernelSession);
        pDevExt->pKernelSession = NULL;
    }
    pDevExt->uInitState = VBOXGUESTDEVEXT_INIT_STATE_DELETED;
}

/**
 * Initializes the VBoxGuest device extension resource parts.
 *
 * The native code locates the VMMDev on the PCI bus and retrieves the MMIO and
 * I/O port ranges, this function validates the MMIO memory and brings up the
 * communication with the host.  Upon successful return the native code should
 * set up the interrupt handler.
 *
 * @returns VBox status code.
 * @param   pDevExt         The device extension.
 * @param   pPdo            The physical device object (for DMA).
 * @param   IOPortBase      The base of the I/O port range.
 * @param   pvMMIOBase      The base of the MMIO memory mapping.
 *                          This is optional, pass NULL if not present.
 * @param   cbMMIO          The size of the MMIO memory mapping.
 *                          This is optional, pass 0 if not present.
 * @param   fFixedEvents    Events that will be enabled upon init and no client
 *                          will ever be allowed to mask.
 */
int VGDrvCommonInitDevExtResources(PVBOXGUESTDEVEXT pDevExt, PDEVICE_OBJECT pPdo, USHORT IOPortBase,
                                   void *pvMMIOBase, UINT32 cbMMIO, UINT32 fFixedEvents)
{
    int rc;

    pDevExt->pVMMDevMemory = NULL;
    if (pvMMIOBase)
    {
        VMMDevMemory volatile *pVMMDev = (VMMDevMemory volatile *)pvMMIOBase;
        if (   pVMMDev->u32Version == VMMDEV_MEMORY_VERSION
            && pVMMDev->u32Size >= 32
            && pVMMDev->u32Size <= cbMMIO)
            pDevExt->pVMMDevMemory = pVMMDev;
    }

    /*
     * Initialize the guest library and preallocate the requests used at
     * elevated IRQL, where VbglR0GRAlloc can't be called.
     */
    pDevExt->IOPortBase = IOPortBase;
    rc = VbglR0InitPrimary(pPdo, IOPortBase, pDevExt->pVMMDevMemory, &pDevExt->fHostFeatures);
    if (RT_FAILURE(rc))
    {
        VGDrvLogRel("VMMDev initialization failed, rc=%d\n", rc);
        return rc;
    }
    VGDrvLogRel("host %u.%u.%u r%u, features %#x, VMMDev memory %s\n",
                g_vbgldata.hostVersion.major, g_vbgldata.hostVersion.minor, g_vbgldata.hostVersion.build,
                g_vbgldata.hostVersion.revision, pDevExt->fHostFeatures, pDevExt->pVMMDevMemory ? "mapped" : "none");

    rc = VbglR0GRAlloc((VMMDevRequestHeader **)&pDevExt->pIrqAckEvents, sizeof(VMMDevEvents),
                       VMMDevReq_AcknowledgeEvents);
    if (RT_SUCCESS(rc))
        rc = VbglR0GRAlloc((VMMDevRequestHeader **)&pDevExt->pMouseStatusReq, sizeof(VMMDevReqMouseStatus),
                           VMMDevReq_GetMouseStatus);
    if (RT_SUCCESS(rc))
        rc = VbglR0GRAlloc((VMMDevRequestHeader **)&pDevExt->pPowerStateReq, sizeof(VMMDevPowerStateRequest),
                           VMMDevReq_SetPowerStatus);

    /*
     * Report the guest info back to VMMDev and set the interrupt control
     * filter mask, guest capabilities and mouse status.  Session state
     * survives a stop/start cycle, so use the combined masks.
     */
    if (RT_SUCCESS(rc))
        rc = vgdrvReportGuestInfo(pDevExt->enmOsType);
    if (RT_SUCCESS(rc))
    {
        pDevExt->fFixedEvents = fFixedEvents;
        rc = vgdrvSetHostEventFilter(pDevExt->fFixedEvents | pDevExt->EventFilterTracker.fMask);
    }
    if (RT_SUCCESS(rc))
        rc = vgdrvSetHostCapabilities(pDevExt->CapabilitiesTracker.fMask);
    if (RT_SUCCESS(rc))
        rc = vgdrvSetHostMouseStatus(pDevExt->MouseStatusTracker.fMask);
    if (RT_SUCCESS(rc))
    {
        vgdrvHeartbeatInit(pDevExt);
        vgdrvReportDriverStatus(VBoxGuestFacilityStatus_Active);
        pDevExt->uInitState = VBOXGUESTDEVEXT_INIT_STATE_RESOURCES;
        return VINF_SUCCESS;
    }

    VGDrvLogRel("host setup failed, rc=%d\n", rc);
    pDevExt->uInitState = VBOXGUESTDEVEXT_INIT_STATE_RESOURCES; /* So the delete call below does its job. */
    VGDrvCommonDeleteDevExtResources(pDevExt);
    return rc;
}

/**
 * Counter to VGDrvCommonInitDevExtResources.
 *
 * The interrupt must be disconnected before calling this.
 *
 * @param   pDevExt         The device extension.
 */
void VGDrvCommonDeleteDevExtResources(PVBOXGUESTDEVEXT pDevExt)
{
    if (pDevExt->uInitState != VBOXGUESTDEVEXT_INIT_STATE_RESOURCES)
        return;
    pDevExt->uInitState = VBOXGUESTDEVEXT_INIT_STATE_FUNDAMENT;

    /* Wake up everyone, nothing will complete from now on. */
    VGDrvCommonInterruptWaits(pDevExt, NULL);

    vgdrvHeartbeatTerm(pDevExt);
    vgdrvReportDriverStatus(VBoxGuestFacilityStatus_Terminated);
    vgdrvSetHostEventFilter(0);
    vgdrvSetHostCapabilities(0);
    vgdrvSetHostMouseStatus(0);

    if (pDevExt->pIrqAckEvents)
    {
        VbglR0GRFree(&pDevExt->pIrqAckEvents->header);
        pDevExt->pIrqAckEvents = NULL;
    }
    if (pDevExt->pMouseStatusReq)
    {
        VbglR0GRFree(&pDevExt->pMouseStatusReq->header);
        pDevExt->pMouseStatusReq = NULL;
    }
    if (pDevExt->pPowerStateReq)
    {
        VbglR0GRFree(&pDevExt->pPowerStateReq->header);
        pDevExt->pPowerStateReq = NULL;
    }

    VbglR0TerminatePrimary();
    pDevExt->pVMMDevMemory = NULL;
    pDevExt->IOPortBase    = 0;
}


/*********************************************************************************************************************************
*   Events and waiting                                                                                                           *
*********************************************************************************************************************************/

/**
 * Checks whether a wait is satisfied, consuming the matching pending events.
 * Caller holds EventSpinLock.
 */
static BOOLEAN vgdrvWaitCheckLocked(PVBOXGUESTDEVEXT pDevExt, PVBOXGUESTWAIT pWait)
{
    UINT32 fMatch;

    if (pWait->pHGCMReq)
        return (pWait->pHGCMReq->fu32Flags & (VBOX_HGCM_REQ_DONE | VBOX_HGCM_REQ_CANCELLED)) != 0;

    fMatch = pWait->fReqEvents & pDevExt->fPendingEvents;
    if (!fMatch)
        return FALSE;
    pWait->fResEvents = fMatch;
    pDevExt->fPendingEvents &= ~fMatch;
    return TRUE;
}

/**
 * Takes a waiter off the wait list and wakes it up.  Caller holds EventSpinLock.
 */
static void vgdrvWaitWakeUpLocked(PVBOXGUESTWAIT pWait)
{
    RemoveEntryList(&pWait->ListEntry);
    InitializeListHead(&pWait->ListEntry); /* Marks it as off the list. */
    KeSetEvent(&pWait->Event, 0, FALSE);
}

/**
 * Common wait worker.  Must be called at PASSIVE_LEVEL.
 *
 * @returns VINF_SUCCESS, VERR_TIMEOUT or VERR_INTERRUPTED.
 */
static int vgdrvWait(PVBOXGUESTDEVEXT pDevExt, PVBOXGUESTWAIT pWait, UINT32 cMsTimeout)
{
    LARGE_INTEGER Timeout;
    BOOLEAN fDone;
    KIRQL OldIrql;

    KeInitializeEvent(&pWait->Event, NotificationEvent, FALSE);
    InitializeListHead(&pWait->ListEntry);

    KeAcquireSpinLock(&pDevExt->EventSpinLock, &OldIrql);
    if (pDevExt->uInitState != VBOXGUESTDEVEXT_INIT_STATE_RESOURCES)
    {
        KeReleaseSpinLock(&pDevExt->EventSpinLock, OldIrql);
        return VERR_INTERRUPTED;
    }
    if (vgdrvWaitCheckLocked(pDevExt, pWait))
    {
        KeReleaseSpinLock(&pDevExt->EventSpinLock, OldIrql);
        return VINF_SUCCESS;
    }
    if (cMsTimeout == 0)
    {
        KeReleaseSpinLock(&pDevExt->EventSpinLock, OldIrql);
        return VERR_TIMEOUT;
    }
    InsertTailList(&pDevExt->WaitList, &pWait->ListEntry);
    KeReleaseSpinLock(&pDevExt->EventSpinLock, OldIrql);

    if (cMsTimeout == VBOXGUEST98_WAIT_INFINITE)
        KeWaitForSingleObject(&pWait->Event, Executive, KernelMode, FALSE, NULL);
    else
    {
        Timeout.QuadPart = Int32x32To64(cMsTimeout, -10000);
        KeWaitForSingleObject(&pWait->Event, Executive, KernelMode, FALSE, &Timeout);
    }

    KeAcquireSpinLock(&pDevExt->EventSpinLock, &OldIrql);
    if (!IsListEmpty(&pWait->ListEntry))
    {
        /* Timed out; it may have completed in the meantime. */
        RemoveEntryList(&pWait->ListEntry);
        InitializeListHead(&pWait->ListEntry);
        fDone = vgdrvWaitCheckLocked(pDevExt, pWait);
    }
    else
        fDone = !pWait->fInterrupted;
    KeReleaseSpinLock(&pDevExt->EventSpinLock, OldIrql);

    if (fDone)
        return VINF_SUCCESS;
    return pWait->fInterrupted ? VERR_INTERRUPTED : VERR_TIMEOUT;
}

/**
 * Waits for one of the given events.  PASSIVE_LEVEL.
 *
 * @returns VINF_SUCCESS, VERR_TIMEOUT, VERR_INTERRUPTED or VERR_INVALID_PARAMETER.
 * @param   pDevExt     The device extension.
 * @param   pSession    The calling session.
 * @param   fReqEvents  The events to wait for.
 * @param   cMsTimeout  Timeout in ms, 0 to poll, VBOXGUEST98_WAIT_INFINITE.
 * @param   pfEvents    Where to return the events that occurred.
 */
int VGDrvCommonWaitForEvents(PVBOXGUESTDEVEXT pDevExt, PVBOXGUESTSESSION pSession,
                             UINT32 fReqEvents, UINT32 cMsTimeout, UINT32 *pfEvents)
{
    VBOXGUESTWAIT Wait;
    int rc;

    *pfEvents = 0;
    fReqEvents &= VMMDEV_EVENT_VALID_EVENT_MASK & ~VMMDEV_EVENT_HGCM;
    if (!fReqEvents)
        return VERR_INVALID_PARAMETER;

    RtlZeroMemory(&Wait, sizeof(Wait));
    Wait.pSession   = pSession;
    Wait.fReqEvents = fReqEvents;
    rc = vgdrvWait(pDevExt, &Wait, cMsTimeout);
    if (RT_SUCCESS(rc))
        *pfEvents = Wait.fResEvents;
    return rc;
}

/**
 * Waits for an HGCM request to complete.  PASSIVE_LEVEL.
 *
 * @returns VINF_SUCCESS, VERR_TIMEOUT or VERR_INTERRUPTED.
 */
int VGDrvCommonWaitForHgcm(PVBOXGUESTDEVEXT pDevExt, PVBOXGUESTSESSION pSession,
                           VMMDevHGCMRequestHeader volatile *pHGCMReq, UINT32 cMsTimeout)
{
    VBOXGUESTWAIT Wait;

    RtlZeroMemory(&Wait, sizeof(Wait));
    Wait.pSession = pSession;
    Wait.pHGCMReq = pHGCMReq;
    return vgdrvWait(pDevExt, &Wait, cMsTimeout);
}

/**
 * Interrupts waiters.
 *
 * @param   pDevExt     The device extension.
 * @param   pSession    Interrupt the event waits of this session only.  NULL
 *                      interrupts all waits, HGCM ones included.
 */
void VGDrvCommonInterruptWaits(PVBOXGUESTDEVEXT pDevExt, PVBOXGUESTSESSION pSession)
{
    PLIST_ENTRY pEntry;
    PLIST_ENTRY pNext;
    KIRQL OldIrql;

    KeAcquireSpinLock(&pDevExt->EventSpinLock, &OldIrql);
    for (pEntry = pDevExt->WaitList.Flink; pEntry != &pDevExt->WaitList; pEntry = pNext)
    {
        PVBOXGUESTWAIT pWait = CONTAINING_RECORD(pEntry, VBOXGUESTWAIT, ListEntry);
        pNext = pEntry->Flink;
        if (   !pSession
            || (pWait->pSession == pSession && !pWait->pHGCMReq))
        {
            pWait->fInterrupted = TRUE;
            vgdrvWaitWakeUpLocked(pWait);
        }
    }
    KeReleaseSpinLock(&pDevExt->EventSpinLock, OldIrql);
}

/**
 * Calls the mouse notification callback with the current mouse status.
 * DISPATCH_LEVEL.
 */
static void vgdrvNotifyMouse(PVBOXGUESTDEVEXT pDevExt)
{
    VMMDevReqMouseStatus *pReq = pDevExt->pMouseStatusReq;
    KIRQL OldIrql;

    KeAcquireSpinLock(&pDevExt->MouseEventAccessSpinLock, &OldIrql);
    if (pDevExt->pfnMouseNotify && pReq)
    {
        pReq->header.rc     = VERR_GENERAL_FAILURE;
        pReq->mouseFeatures = 0;
        pReq->pointerXPos   = 0;
        pReq->pointerYPos   = 0;
        if (RT_SUCCESS(VbglR0GRPerform(&pReq->header)))
            pDevExt->pfnMouseNotify(pDevExt->pvMouseNotifyUser, pReq->mouseFeatures,
                                    pReq->pointerXPos, pReq->pointerYPos);
    }
    KeReleaseSpinLock(&pDevExt->MouseEventAccessSpinLock, OldIrql);
}

/**
 * Processes the events acknowledged by the ISR.  Called from the DPC.
 *
 * @param   pDevExt     The device extension.
 * @param   fEvents     The events (VMMDEV_EVENT_XXX).
 */
void VGDrvCommonProcessEvents(PVBOXGUESTDEVEXT pDevExt, UINT32 fEvents)
{
    PLIST_ENTRY pEntry;
    PLIST_ENTRY pNext;
    KIRQL OldIrql;

    if (fEvents & VMMDEV_EVENT_MOUSE_POSITION_CHANGED)
        vgdrvNotifyMouse(pDevExt);

    KeAcquireSpinLock(&pDevExt->EventSpinLock, &OldIrql);

    /* Only keep events someone asked for, HGCM completion is tracked by the
       request flags. */
    pDevExt->fPendingEvents |= fEvents & pDevExt->EventFilterTracker.fMask & ~VMMDEV_EVENT_HGCM;

    for (pEntry = pDevExt->WaitList.Flink; pEntry != &pDevExt->WaitList; pEntry = pNext)
    {
        PVBOXGUESTWAIT pWait = CONTAINING_RECORD(pEntry, VBOXGUESTWAIT, ListEntry);
        pNext = pEntry->Flink;
        if (vgdrvWaitCheckLocked(pDevExt, pWait))
            vgdrvWaitWakeUpLocked(pWait);
    }

    KeReleaseSpinLock(&pDevExt->EventSpinLock, OldIrql);
}


/*********************************************************************************************************************************
*   Sessions                                                                                                                     *
*********************************************************************************************************************************/

/**
 * Creates a session.  PASSIVE_LEVEL.
 *
 * @returns VBox status code.
 * @param   pDevExt         The device extension.
 * @param   fUserSession    Whether this is for a user mode handle.
 * @param   ppSession       Where to return the session.
 */
int VGDrvCommonCreateSession(PVBOXGUESTDEVEXT pDevExt, BOOLEAN fUserSession, PVBOXGUESTSESSION *ppSession)
{
    PVBOXGUESTSESSION pSession = (PVBOXGUESTSESSION)ExAllocatePool(NonPagedPool, sizeof(VBOXGUESTSESSION));
    *ppSession = NULL;
    if (!pSession)
        return VERR_NO_MEMORY;
    RtlZeroMemory(pSession, sizeof(*pSession));
    pSession->fUserSession = fUserSession;

    /*
     * Requestor flags, as upstream (vgdrvNtCalcRequestorFlags and
     * VGDrvCommonCreateKernelSession).  Windows 98 has no user accounts to
     * tell apart and a single device node, so every user mode client counts
     * as an administrator on the trusted device.
     */
    if (fUserSession)
        pSession->fRequestor = VMMDEV_REQUESTOR_USERMODE       | VMMDEV_REQUESTOR_USR_ROOT
                             | VMMDEV_REQUESTOR_CON_DONT_KNOW  | VMMDEV_REQUESTOR_TRUST_NOT_GIVEN
                             | VMMDEV_REQUESTOR_NO_USER_DEVICE;
    else
        pSession->fRequestor = VMMDEV_REQUESTOR_KERNEL         | VMMDEV_REQUESTOR_USR_DRV_OTHER
                             | VMMDEV_REQUESTOR_CON_DONT_KNOW  | VMMDEV_REQUESTOR_TRUST_NOT_GIVEN;

    vgdrvAcquireSessionMutex(pDevExt);
    InsertTailList(&pDevExt->SessionList, &pSession->ListEntry);
    if (fUserSession)
        pDevExt->cUserSessions++;
    vgdrvReleaseSessionMutex(pDevExt);

    *ppSession = pSession;
    return VINF_SUCCESS;
}

/**
 * Closes a session, undoing everything it changed on the host.  PASSIVE_LEVEL.
 */
void VGDrvCommonCloseSession(PVBOXGUESTDEVEXT pDevExt, PVBOXGUESTSESSION pSession)
{
    VGDrvCommonInterruptWaits(pDevExt, pSession);
    VGDrvHgcmDisconnectSessionClients(pDevExt, pSession);
    VGDrvCommonSetSessionEventFilter(pDevExt, pSession, 0, ~0U);
    VGDrvCommonSetSessionCapabilities(pDevExt, pSession, 0, ~0U);
    VGDrvCommonSetSessionMouseStatus(pDevExt, pSession, 0);

    vgdrvAcquireSessionMutex(pDevExt);
    RemoveEntryList(&pSession->ListEntry);
    if (pSession->fUserSession)
        pDevExt->cUserSessions--;
    vgdrvReleaseSessionMutex(pDevExt);

    ExFreePool(pSession);
}

/**
 * Changes one session mask and pushes the combined mask to the host.
 * Caller holds SessionMutex.
 *
 * @returns VBox status code.  The session mask is unchanged on failure.
 */
static int vgdrvChangeSessionMask(PVBOXGUESTDEVEXT pDevExt, VBOXGUESTBITUSAGETRACER *pTracker, UINT32 *pfSession,
                                  UINT32 fNew, int (*pfnSetHost)(UINT32), UINT32 fHostExtra)
{
    UINT32 fPrevious = *pfSession;
    UINT32 fChanged  = fPrevious ^ fNew;
    int rc = VINF_SUCCESS;

    if (!fChanged)
        return VINF_SUCCESS;

    *pfSession = fNew;
    if (   vgdrvBitUsageTrackerChange(pTracker, fChanged, fPrevious)
        && pDevExt->uInitState == VBOXGUESTDEVEXT_INIT_STATE_RESOURCES)
    {
        rc = pfnSetHost(pTracker->fMask | fHostExtra);
        if (RT_FAILURE(rc))
        {
            vgdrvBitUsageTrackerChange(pTracker, fChanged, fNew);
            *pfSession = fPrevious;
        }
    }
    return rc;
}

/**
 * Changes the events a session wants.  PASSIVE_LEVEL.
 */
int VGDrvCommonSetSessionEventFilter(PVBOXGUESTDEVEXT pDevExt, PVBOXGUESTSESSION pSession,
                                     UINT32 fOrMask, UINT32 fNotMask)
{
    int rc;
    if ((fOrMask & ~VMMDEV_EVENT_VALID_EVENT_MASK) != 0)
        return VERR_INVALID_PARAMETER;

    vgdrvAcquireSessionMutex(pDevExt);
    rc = vgdrvChangeSessionMask(pDevExt, &pDevExt->EventFilterTracker, &pSession->fEventFilter,
                                (pSession->fEventFilter | fOrMask) & ~fNotMask,
                                vgdrvSetHostEventFilter, pDevExt->fFixedEvents);
    vgdrvReleaseSessionMutex(pDevExt);
    return rc;
}

/**
 * Changes the guest capabilities a session reports.  PASSIVE_LEVEL.
 */
int VGDrvCommonSetSessionCapabilities(PVBOXGUESTDEVEXT pDevExt, PVBOXGUESTSESSION pSession,
                                      UINT32 fOrMask, UINT32 fNotMask)
{
    int rc;
    if ((fOrMask & ~VMMDEV_GUEST_CAPABILITIES_MASK) != 0)
        return VERR_INVALID_PARAMETER;

    vgdrvAcquireSessionMutex(pDevExt);
    rc = vgdrvChangeSessionMask(pDevExt, &pDevExt->CapabilitiesTracker, &pSession->fCapabilities,
                                (pSession->fCapabilities | fOrMask) & ~fNotMask,
                                vgdrvSetHostCapabilities, 0);
    vgdrvReleaseSessionMutex(pDevExt);
    return rc;
}

/**
 * Sets the mouse features a session requires.  PASSIVE_LEVEL.
 */
int VGDrvCommonSetSessionMouseStatus(PVBOXGUESTDEVEXT pDevExt, PVBOXGUESTSESSION pSession, UINT32 fFeatures)
{
    int rc;
    if ((fFeatures & ~VMMDEV_MOUSE_GUEST_MASK) != 0)
        return VERR_INVALID_PARAMETER;

    vgdrvAcquireSessionMutex(pDevExt);
    rc = vgdrvChangeSessionMask(pDevExt, &pDevExt->MouseStatusTracker, &pSession->fMouseStatus,
                                fFeatures, vgdrvSetHostMouseStatus, 0);
    vgdrvReleaseSessionMutex(pDevExt);
    return rc;
}

/**
 * Installs or removes the mouse notification callback.
 */
int VGDrvCommonSetMouseNotifyCallback(PVBOXGUESTDEVEXT pDevExt, PFNVBOXGUEST98MOUSENOTIFY pfnNotify, PVOID pvUser)
{
    KIRQL OldIrql;

    KeAcquireSpinLock(&pDevExt->MouseEventAccessSpinLock, &OldIrql);
    pDevExt->pfnMouseNotify    = pfnNotify;
    pDevExt->pvMouseNotifyUser = pfnNotify ? pvUser : NULL;
    KeReleaseSpinLock(&pDevExt->MouseEventAccessSpinLock, OldIrql);
    return VINF_SUCCESS;
}
