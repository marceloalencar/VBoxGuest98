/** @file
 * VBoxGst - Interrupt service routine and DPC.
 *
 * The ISR acknowledges the events with the host (fast acknowledge port or a
 * preallocated request) and defers all processing to the DPC; it must not
 * take spinlocks or log.
 *
 * Deliberate difference from upstream: VMMDEV_EVENT_MOUSE_POSITION_CHANGED is
 * also delivered to event waiters, because on Windows 98 the pointer is
 * positioned by VBoxTray in user mode, not by a kernel mouse filter.
 */

#include "VBoxGst.h"

/**
 * ISR handler.
 *
 * @return  BOOLEAN         Indicates whether the IRQ came from us (TRUE) or not (FALSE).
 * @param   pInterrupt      Interrupt that was triggered.
 * @param   pvContext       Context specific pointer (our device extension).
 */
BOOLEAN NTAPI vgdrvWinIsrHandler(PKINTERRUPT pInterrupt, PVOID pvContext)
{
    PVBOXGUESTDEVEXTWIN pDevExt = (PVBOXGUESTDEVEXTWIN)pvContext;
    VMMDevMemory volatile *pVMMDevMemory = pDevExt->Core.pVMMDevMemory;
    VMMDevEvents *pReq = pDevExt->Core.pIrqAckEvents;
    UINT32 fEvents;

    /* The interrupt line is shared, check whether the VMMDev raised it. */
    if (!pVMMDevMemory || !pReq || !pVMMDevMemory->V.V1_04.fHaveEvents)
        return FALSE;

    /*
     * Acknowledge the events.  Like upstream VGDrvCommonISR, prefer the fast
     * acknowledge port (one IN instruction) when the host offers it.
     */
    if (pDevExt->Core.fHostFeatures & VMMDEV_HVF_FAST_IRQ_ACK)
    {
        fEvents = READ_PORT_ULONG((PULONG)(ULONG_PTR)(pDevExt->Core.IOPortBase + VMMDEV_PORT_OFF_REQUEST_FAST));
        if (fEvents == 0xffffffffU)
            fEvents = 0; /* Failure. */
    }
    else
    {
        pReq->header.rc = VERR_INTERNAL_ERROR;
        pReq->events    = 0;
        fEvents = RT_SUCCESS(VbglR0GRPerform(&pReq->header)) ? pReq->events : 0;
    }

    if (fEvents)
    {
        vgdrvAtomicOrU32(&pDevExt->Core.fIsrEvents, fEvents);
        IoRequestDpc(pDevExt->pDeviceObject, NULL, NULL);
    }
    return TRUE;
}

/**
 * DPC handler.
 *
 * @param   pDpc        DPC descriptor.
 * @param   pDevObj     Device object.
 * @param   pIrp        Interrupt request packet.
 * @param   pvContext   Context specific pointer.
 */
VOID NTAPI vgdrvWinDpcHandler(PKDPC pDpc, PDEVICE_OBJECT pDevObj, PIRP pIrp, PVOID pvContext)
{
    PVBOXGUESTDEVEXTWIN pDevExt = (PVBOXGUESTDEVEXTWIN)pDevObj->DeviceExtension;
    UINT32 fEvents = vgdrvAtomicXchgU32(&pDevExt->Core.fIsrEvents, 0);
    if (fEvents)
        VGDrvCommonProcessEvents(&pDevExt->Core, fEvents);
}
