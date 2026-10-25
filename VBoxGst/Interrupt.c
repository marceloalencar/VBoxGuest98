/** @file
 * VBoxGst - Interrupt service routine and DPC.
 *
 * The ISR acknowledges the events with the host using a preallocated request
 * and defers all processing to the DPC; it must not take spinlocks or log.
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

    /* The interrupt line is shared, check whether the VMMDev raised it. */
    if (!pVMMDevMemory || !pReq || !pVMMDevMemory->V.V1_04.fHaveEvents)
        return FALSE;

    pReq->header.rc = VERR_INTERNAL_ERROR;
    pReq->events    = 0;
    if (RT_SUCCESS(VbglR0GRPerform(&pReq->header)) && pReq->events)
    {
        vgdrvAtomicOrU32(&pDevExt->Core.fIsrEvents, pReq->events);
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
