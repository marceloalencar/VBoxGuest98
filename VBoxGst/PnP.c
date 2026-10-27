/** @file
 * VBoxGst - Plug & Play and power management, hardware resources.
 */

#include "VBoxGst.h"


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/

/** Events the driver always wants: mouse position changes are forwarded to
 *  the mouse filter, HGCM completions wake up the HGCM callers. */
#define VGDRV_FIXED_EVENTS  (VMMDEV_EVENT_MOUSE_POSITION_CHANGED | VMMDEV_EVENT_HGCM)


/**
 * Irp completion routine for PnP Irps we send.
 *
 * @returns NT status code.
 * @param   pDevObj   Device object.
 * @param   pIrp      Request packet.
 * @param   pvEvent   Semaphore (PKEVENT).
 */
static NTSTATUS NTAPI vgdrvWinPnpIrpComplete(PDEVICE_OBJECT pDevObj, PIRP pIrp, PVOID pvEvent)
{
    KeSetEvent((PKEVENT)pvEvent, 0, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/**
 * Helper to send a PnP IRP down the stack and wait until it's done.
 *
 * @returns NT status code.
 * @param   pDevObj    Lower device object.
 * @param   pIrp       Request packet.
 * @param   fStrict    When FALSE, STATUS_NOT_SUPPORTED and
 *                     STATUS_INVALID_DEVICE_REQUEST are turned into success.
 */
static NTSTATUS vgdrvWinPnPSendIrpSynchronously(PDEVICE_OBJECT pDevObj, PIRP pIrp, BOOLEAN fStrict)
{
    KEVENT Event;
    NTSTATUS rcNt;

    KeInitializeEvent(&Event, SynchronizationEvent, FALSE);

    IoCopyCurrentIrpStackLocationToNext(pIrp);
    IoSetCompletionRoutine(pIrp, vgdrvWinPnpIrpComplete, &Event, TRUE, TRUE, TRUE);

    rcNt = IoCallDriver(pDevObj, pIrp);
    if (rcNt == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        rcNt = pIrp->IoStatus.Status;
    }

    if (   !fStrict
        && (rcNt == STATUS_NOT_SUPPORTED || rcNt == STATUS_INVALID_DEVICE_REQUEST))
        rcNt = STATUS_SUCCESS;

    return rcNt;
}

/**
 * Passes an IRP down the stack without waiting for it.
 */
static NTSTATUS vgdrvWinPnPPassDown(PVBOXGUESTDEVEXTWIN pDevExt, PIRP pIrp)
{
    IoSkipCurrentIrpStackLocation(pIrp);
    return IoCallDriver(pDevExt->pNextLowerDriver, pIrp);
}

/**
 * Helper to scan the PCI resource list and remember stuff.
 *
 * @returns NT status code, failure if the I/O ports or the MMIO are missing.
 * @param   pDevExt         The device extension.
 * @param   pResList        Resource list (translated).
 */
static NTSTATUS vgdrvWinScanPCIResourceList(PVBOXGUESTDEVEXTWIN pDevExt, PCM_RESOURCE_LIST pResList)
{
    PCM_PARTIAL_RESOURCE_DESCRIPTOR pPartialData;
    BOOLEAN fGotIrq     = FALSE;
    BOOLEAN fGotMmio    = FALSE;
    BOOLEAN fGotIoPorts = FALSE;
    ULONG i;

    if (!pResList || pResList->Count < 1)
        return STATUS_DEVICE_CONFIGURATION_ERROR;

    for (i = 0; i < pResList->List[0].PartialResourceList.Count; i++)
    {
        pPartialData = &pResList->List[0].PartialResourceList.PartialDescriptors[i];
        switch (pPartialData->Type)
        {
            case CmResourceTypePort:
                /* Save the first I/O port base. */
                if (!fGotIoPorts)
                {
                    pDevExt->uIoPortPhysAddr = pPartialData->u.Port.Start;
                    pDevExt->cbIoPort        = pPartialData->u.Port.Length;
                    fGotIoPorts = TRUE;
                }
                break;

            case CmResourceTypeInterrupt:
                if (!fGotIrq)
                {
                    /* Save information. */
                    /* Only the low byte is an IRQL; when the device is started after
                       Windows is up (e.g. installed at logon), NTKERN leaves garbage
                       in the rest. */
                    pDevExt->uInterruptLevel    = (KIRQL)pPartialData->u.Interrupt.Level;
                    pDevExt->uInterruptVector   = pPartialData->u.Interrupt.Vector;
                    pDevExt->fInterruptAffinity = pPartialData->u.Interrupt.Affinity;

                    /* Check interrupt mode. */
                    if (pPartialData->Flags & CM_RESOURCE_INTERRUPT_LATCHED)
                        pDevExt->enmInterruptMode = Latched;
                    else
                        pDevExt->enmInterruptMode = LevelSensitive;
                    fGotIrq = TRUE;
                }
                break;

            case CmResourceTypeMemory:
                /* We only care about the first read/write memory range. */
                if (!fGotMmio && pPartialData->Flags == CM_RESOURCE_MEMORY_READ_WRITE)
                {
                    /* Save physical MMIO base + length for VMMDev. */
                    pDevExt->uVmmDevMemoryPhysAddr = pPartialData->u.Memory.Start;
                    pDevExt->cbVmmDevMemory        = pPartialData->u.Memory.Length;
                    fGotMmio = TRUE;
                }
                break;

            default:
                break;
        }
    }

    /* The IRQ is optional: without it everything but events works. */
    if (!fGotIoPorts || !fGotMmio)
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    return STATUS_SUCCESS;
}

/**
 * Maps the VMMDev memory into kernel address space.
 *
 * @returns NT status code.
 * @param   pDevExt     The device extension.  Uses uVmmDevMemoryPhysAddr and
 *                      cbVmmDevMemory, sets pvMMIOBase.
 * @param   pcbMMIO     Where to return the size of the VMMDev memory structure.
 */
static NTSTATUS vgdrvWinMapVMMDevMemory(PVBOXGUESTDEVEXTWIN pDevExt, UINT32 *pcbMMIO)
{
    VMMDevMemory volatile *pVMMDevMemory;

    if (   pDevExt->uVmmDevMemoryPhysAddr.HighPart != 0 /* We're mapping below 4GB. */
        || pDevExt->uVmmDevMemoryPhysAddr.LowPart == 0
        || pDevExt->cbVmmDevMemory < sizeof(VMMDevMemory))
        return STATUS_DEVICE_CONFIGURATION_ERROR;

    pVMMDevMemory = (VMMDevMemory volatile *)MmMapIoSpace(pDevExt->uVmmDevMemoryPhysAddr, pDevExt->cbVmmDevMemory,
                                                          MmNonCached);
    if (!pVMMDevMemory)
        return STATUS_INSUFFICIENT_RESOURCES;

    /* Check version of the structure; do we have the right memory version? */
    if (pVMMDevMemory->u32Version != VMMDEV_MEMORY_VERSION)
    {
        MmUnmapIoSpace((PVOID)pVMMDevMemory, pDevExt->cbVmmDevMemory);
        return STATUS_UNSUCCESSFUL;
    }

    pDevExt->pvMMIOBase = (PVOID)pVMMDevMemory;
    *pcbMMIO = pVMMDevMemory->u32Size;
    return STATUS_SUCCESS;
}

/**
 * Unmaps the VMMDev memory from kernel space.
 *
 * @param   pDevExt     The device extension.
 */
static void vgdrvWinUnmapVMMDevMemory(PVBOXGUESTDEVEXTWIN pDevExt)
{
    if (pDevExt->pvMMIOBase)
    {
        MmUnmapIoSpace(pDevExt->pvMMIOBase, pDevExt->cbVmmDevMemory);
        pDevExt->pvMMIOBase = NULL;
    }
    pDevExt->uVmmDevMemoryPhysAddr.QuadPart = 0;
    pDevExt->cbVmmDevMemory = 0;
}

/**
 * Reads the driver configuration from the driver registry key
 * (HKLM\System\CurrentControlSet\Services\Class\System\00xx on Windows 98).
 *
 * @param   pDevExt     The device extension.
 */
static void vgdrvWinReadConfiguration(PVBOXGUESTDEVEXTWIN pDevExt)
{
    ULONG au32Buf[(sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG) + sizeof(ULONG) - 1) / sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION pInfo = (PKEY_VALUE_PARTIAL_INFORMATION)&au32Buf[0];
    UNICODE_STRING ValueName;
    HANDLE hKey;
    ULONG cbRet = 0;
    NTSTATUS rcNt;

    pDevExt->fPowerOffOnShutdown = FALSE;

    rcNt = IoOpenDeviceRegistryKey(pDevExt->pPhysicalDeviceObject, PLUGPLAY_REGKEY_DRIVER, KEY_READ, &hKey);
    if (!NT_SUCCESS(rcNt))
        return;

    /* The Windows 98 INF syntax has no REG_DWORD, so accept 4 binary bytes too. */
    RtlInitUnicodeString(&ValueName, L"PowerOffOnShutdown");
    rcNt = ZwQueryValueKey(hKey, &ValueName, KeyValuePartialInformation, pInfo, sizeof(au32Buf), &cbRet);
    if (   NT_SUCCESS(rcNt)
        && (pInfo->Type == REG_DWORD || pInfo->Type == REG_BINARY)
        && pInfo->DataLength >= sizeof(ULONG))
        pDevExt->fPowerOffOnShutdown = *(ULONG UNALIGNED *)&pInfo->Data[0] != 0;

    ZwClose(hKey);
}

/**
 * Sets up the device and its resources (IRP_MN_START_DEVICE).
 *
 * @returns NT status code.
 * @param   pDevExt     Our device extension data.
 * @param   pIrp        The request packet.
 */
static NTSTATUS vgdrvWinSetupDevice(PVBOXGUESTDEVEXTWIN pDevExt, PIRP pIrp)
{
    PIO_STACK_LOCATION pStack = IoGetCurrentIrpStackLocation(pIrp);
    UINT32 cbMMIO = 0;
    NTSTATUS rcNt;
    int rc;

    rcNt = vgdrvWinScanPCIResourceList(pDevExt, pStack->Parameters.StartDevice.AllocatedResourcesTranslated);
    if (!NT_SUCCESS(rcNt))
    {
        VGDrvLogRel("missing hardware resources, status %#x\n", rcNt);
        return rcNt;
    }

    /*
     * Map physical address of VMMDev memory into MMIO region
     * and init the common device extension bits.
     */
    rcNt = vgdrvWinMapVMMDevMemory(pDevExt, &cbMMIO);
    if (!NT_SUCCESS(rcNt))
    {
        VGDrvLogRel("mapping the VMMDev memory failed, status %#x\n", rcNt);
        return rcNt;
    }

    rc = VGDrvCommonInitDevExtResources(&pDevExt->Core, pDevExt->pPhysicalDeviceObject,
                                        (USHORT)pDevExt->uIoPortPhysAddr.LowPart,
                                        pDevExt->pvMMIOBase, cbMMIO, VGDRV_FIXED_EVENTS);
    if (RT_SUCCESS(rc))
    {
        rcNt = STATUS_SUCCESS;
        if (pDevExt->uInterruptVector)
        {
            rcNt = IoConnectInterrupt(&pDevExt->pInterruptObject,     /* Out: interrupt object. */
                                      vgdrvWinIsrHandler,             /* Our ISR handler. */
                                      pDevExt,                        /* Device context. */
                                      NULL,                           /* Optional spinlock. */
                                      pDevExt->uInterruptVector,      /* Interrupt vector. */
                                      pDevExt->uInterruptLevel,       /* Irql. */
                                      pDevExt->uInterruptLevel,       /* SynchronizeIrql. */
                                      pDevExt->enmInterruptMode,      /* LevelSensitive or Latched. */
                                      TRUE,                           /* Shareable interrupt. */
                                      pDevExt->fInterruptAffinity,    /* CPU affinity. */
                                      FALSE);                         /* Don't save FPU stack. */
            if (!NT_SUCCESS(rcNt))
            {
                VGDrvLogRel("connecting IRQ vector %u failed, status %#x\n", pDevExt->uInterruptVector, rcNt);
                pDevExt->pInterruptObject = NULL;
            }
        }
        else
            VGDrvLogRel("no interrupt assigned, events will not be delivered\n");
        if (NT_SUCCESS(rcNt))
        {
            vgdrvWinReadConfiguration(pDevExt);
            VGDrvLogRel("started: I/O port %#x, IRQ level %u, PowerOffOnShutdown=%u\n",
                        pDevExt->uIoPortPhysAddr.LowPart, pDevExt->uInterruptLevel, pDevExt->fPowerOffOnShutdown);
            if (pDevExt->InterfaceName.Buffer)
                IoSetDeviceInterfaceState(&pDevExt->InterfaceName, TRUE);
            pDevExt->enmDevState = VGDRVWINDEVSTATE_STARTED;
            return STATUS_SUCCESS;
        }
        VGDrvCommonDeleteDevExtResources(&pDevExt->Core);
    }
    else
        rcNt = STATUS_DEVICE_CONFIGURATION_ERROR;

    vgdrvWinUnmapVMMDevMemory(pDevExt);
    return rcNt;
}

/**
 * Deletes the device hardware resources.
 *
 * Used during removal and stopping.
 *
 * @param   pDevExt         The device extension.
 */
static void vgdrvWinDeleteDeviceResources(PVBOXGUESTDEVEXTWIN pDevExt)
{
    if (pDevExt->enmDevState == VGDRVWINDEVSTATE_STARTED && pDevExt->InterfaceName.Buffer)
        IoSetDeviceInterfaceState(&pDevExt->InterfaceName, FALSE);

    if (pDevExt->pInterruptObject)
    {
        IoDisconnectInterrupt(pDevExt->pInterruptObject);
        pDevExt->pInterruptObject = NULL;
        KeRemoveQueueDpc(&pDevExt->pDeviceObject->Dpc);
        pDevExt->Core.fIsrEvents = 0;
    }

    VGDrvCommonDeleteDevExtResources(&pDevExt->Core);
    vgdrvWinUnmapVMMDevMemory(pDevExt);
}

/**
 * PnP Request handler.
 *
 * @param  pDevObj    Device object.
 * @param  pIrp       Request packet.
 */
NTSTATUS NTAPI vgdrvWinPnP(PDEVICE_OBJECT pDevObj, PIRP pIrp)
{
    PVBOXGUESTDEVEXTWIN pDevExt = (PVBOXGUESTDEVEXTWIN)pDevObj->DeviceExtension;
    PIO_STACK_LOCATION pStack = IoGetCurrentIrpStackLocation(pIrp);
    NTSTATUS rcNt;

    switch (pStack->MinorFunction)
    {
        case IRP_MN_START_DEVICE:
        {
            /* This must be handled first by the lower driver. */
            rcNt = vgdrvWinPnPSendIrpSynchronously(pDevExt->pNextLowerDriver, pIrp, TRUE);
            if (NT_SUCCESS(rcNt) && NT_SUCCESS(pIrp->IoStatus.Status))
            {
                if (pStack->Parameters.StartDevice.AllocatedResourcesTranslated)
                    rcNt = vgdrvWinSetupDevice(pDevExt, pIrp);
                else
                    rcNt = STATUS_UNSUCCESSFUL;
            }
            return vgdrvWinCompleteRequest(pIrp, rcNt, 0);
        }

        /*
         * Refuse to stop or remove the device while user mode has handles
         * open, their sessions would lose their host state.
         */
        case IRP_MN_QUERY_STOP_DEVICE:
        case IRP_MN_QUERY_REMOVE_DEVICE:
            if (pDevExt->Core.cUserSessions > 0)
                return vgdrvWinCompleteRequest(pIrp, STATUS_DEVICE_BUSY, 0);
            pIrp->IoStatus.Status = STATUS_SUCCESS;
            return vgdrvWinPnPPassDown(pDevExt, pIrp);

        case IRP_MN_CANCEL_STOP_DEVICE:
        case IRP_MN_CANCEL_REMOVE_DEVICE:
            /* Must be handled by the lower driver first. */
            rcNt = vgdrvWinPnPSendIrpSynchronously(pDevExt->pNextLowerDriver, pIrp, FALSE);
            return vgdrvWinCompleteRequest(pIrp, rcNt, pIrp->IoStatus.Information);

        case IRP_MN_STOP_DEVICE:
            vgdrvWinDeleteDeviceResources(pDevExt);
            pDevExt->enmDevState = VGDRVWINDEVSTATE_STOPPED;
            pIrp->IoStatus.Status = STATUS_SUCCESS;
            return vgdrvWinPnPPassDown(pDevExt, pIrp);

        case IRP_MN_SURPRISE_REMOVAL:
            vgdrvWinDeleteDeviceResources(pDevExt);
            pDevExt->enmDevState = VGDRVWINDEVSTATE_SURPRISE_REMOVED;
            pIrp->IoStatus.Status = STATUS_SUCCESS;
            return vgdrvWinPnPPassDown(pDevExt, pIrp);

        /*
         * Device and/or driver removal.  Destroy everything.
         */
        case IRP_MN_REMOVE_DEVICE:
            vgdrvWinDeleteDeviceResources(pDevExt);
            pDevExt->enmDevState = VGDRVWINDEVSTATE_REMOVED;
            pIrp->IoStatus.Status = STATUS_SUCCESS;
            rcNt = vgdrvWinPnPPassDown(pDevExt, pIrp);
            vgdrvWinDeleteDevice(pDevExt);
            return rcNt;

        default:
            return vgdrvWinPnPPassDown(pDevExt, pIrp);
    }
}

/**
 * Handle the Power requests.
 *
 * Everything is passed down; on system shutdown the host is optionally told
 * to power off the VM (see VBOXGUESTDEVEXTWIN::fPowerOffOnShutdown).  The
 * condition is the one of upstream vgdrvNtNt5PlusPower, which does it
 * unconditionally; here it stays opt-in until it is known when Windows 98
 * sends this IRP during shutdown.
 *
 * @returns   NT status code
 * @param     pDevObj   device object
 * @param     pIrp      IRP
 */
NTSTATUS NTAPI vgdrvWinPower(PDEVICE_OBJECT pDevObj, PIRP pIrp)
{
    PVBOXGUESTDEVEXTWIN pDevExt = (PVBOXGUESTDEVEXTWIN)pDevObj->DeviceExtension;
    PIO_STACK_LOCATION pStack = IoGetCurrentIrpStackLocation(pIrp);

    if (   pStack->MinorFunction == IRP_MN_SET_POWER
        && pStack->Parameters.Power.Type == SystemPowerState
        && pStack->Parameters.Power.State.SystemState >= PowerSystemShutdown
        && (   pStack->Parameters.Power.ShutdownType == PowerActionShutdown
            || pStack->Parameters.Power.ShutdownType == PowerActionShutdownOff)
        && pDevExt->fPowerOffOnShutdown
        && pDevExt->enmDevState == VGDRVWINDEVSTATE_STARTED)
        VGDrvCommonReportPowerOff(&pDevExt->Core);

    PoStartNextPowerIrp(pIrp);
    IoSkipCurrentIrpStackLocation(pIrp);
    return PoCallDriver(pDevExt->pNextLowerDriver, pIrp);
}
