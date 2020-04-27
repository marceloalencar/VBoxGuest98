#include "VBoxGst.h"

static VBOXOSTYPE           g_enmVGDrvOsType = VBOXOSTYPE_UNKNOWN;

void __forceinline KeMemoryBarrier()
{
	LONG Barrier;
	__asm xchg [Barrier], eax
}

/**
* Driver entry point.
*
* @returns appropriate status code.
* @param   pDrvObj     Pointer to driver object.
* @param   pRegPath    Registry base path.
*/
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
	DebugPrintInit("VBoxGst");
	// Checking where we're running:
	// Windows 2000 and newer supports WDM 1.10 and below (1, 0x10)
	// Windows ME supports WDM 1.05 and below (1, 0x05)
	// Windows 98 supports WDM 1.00 and below (1, 0x00)
	if (IoIsWdmVersionAvailable(1, 0x10))
	{
		//VBoxGuest isn't running on Windows 98/ME! [NEWER]
		return STATUS_DRIVER_UNABLE_TO_LOAD;
	}
	if (IoIsWdmVersionAvailable(1, 0x05))
	{
		g_enmVGDrvOsType = VBOXOSTYPE_WINME;
		DebugPrint("Guest is Windows ME.");
	}
	else
	{
		if (IoIsWdmVersionAvailable(1, 0x00))
		{
			g_enmVGDrvOsType = VBOXOSTYPE_WIN98;
			DebugPrint("Guest is Windows 98");
		}
		else
		{
			DebugPrint("Guest is not Windows 98/ME");
			//VBoxGuest isn't running on Windows 98/ME! [OLDER]
			return STATUS_DRIVER_UNABLE_TO_LOAD;
		}
	}
	
	DriverObject->DriverExtension->AddDevice   = vgdrvWinAddDevice;
	DriverObject->DriverUnload                 = vgdrvWinUnload;
	DriverObject->MajorFunction[IRP_MJ_CREATE] = vgdrvWinCreate;
	//Close
	//DeviceControl
	//InternalDeviceControl
	//Shutdown
	//Read = NotSupported
	//Write = NotSupported
	DriverObject->MajorFunction[IRP_MJ_PNP]    = vgdrvWinPnP;
    DriverObject->MajorFunction[IRP_MJ_POWER]  = vgdrvWinPower;
	//SystemControl
    return STATUS_SUCCESS;
}

/**
* Handle request from the Plug & Play subsystem.
*
* @returns NT status code
* @param   pDrvObj   Driver object
* @param   pdo       Device object
*/
static NTSTATUS NTAPI vgdrvWinAddDevice(PDRIVER_OBJECT pDrvObj, PDEVICE_OBJECT pdo)
{
	NTSTATUS retStatus = STATUS_SUCCESS;
	PDEVICE_OBJECT fdo = NULL;
	PVBOXGUESTDEVEXTWIN pDevExt = NULL;
	
	retStatus = IoCreateDevice(pDrvObj, sizeof(VBOXGUESTDEVEXTWIN), NULL, FILE_DEVICE_UNKNOWN, 0, FALSE, &fdo);
	if (!NT_SUCCESS(retStatus))
	{
		//IoCreateDevice failed!
		return retStatus;
	}
	
	pDevExt = (PVBOXGUESTDEVEXTWIN)fdo->DeviceExtension;
	
	retStatus = IoRegisterDeviceInterface(pdo, &WDM_GUID, NULL, &pDevExt->SymLinkName);
	if (!NT_SUCCESS(retStatus))
	{
		//IoRegisterDeviceInterface failed!
		IoDeleteDevice(fdo);
		return retStatus;
	}
	
	IoSetDeviceInterfaceState(&pDevExt->SymLinkName, TRUE);
	//TODO: Initialize ext fundamentals
	pDevExt->pDeviceObject = fdo;
	pDevExt->pNextLowerDriver = IoAttachDeviceToDeviceStack(fdo, pdo);
	
	if (pDevExt->pNextLowerDriver == NULL)
    {
		//IoAttachDeviceToDeviceStack did not give a nextLowerDriver!
        retStatus = STATUS_DEVICE_NOT_CONNECTED;
	}
	
	fdo->Flags |= DO_POWER_PAGABLE;
    fdo->Flags &= ~DO_DEVICE_INITIALIZING;
	
    //Returning success
	return retStatus;
}

/**
* Unload the driver.
*
* @param   pDrvObj     Driver object.
*/
static void NTAPI vgdrvWinUnload(PDRIVER_OBJECT pDrvObj)
{
	IoDeleteDevice(pDrvObj->DeviceObject);
	DebugPrintMsg("vgdrvWinUnload");
	DebugPrintClose();
}

/**
 * Create (i.e. Open) file entry point.
 *
 * @param   pDevObj     Device object.
 * @param   pIrp        Request packet.
 */
static NTSTATUS NTAPI vgdrvWinCreate(PDEVICE_OBJECT pDevObj, PIRP pIrp)
{
	pIrp->IoStatus.Status = STATUS_SUCCESS;
	pIrp->IoStatus.Information = 0;
	IoCompleteRequest(pIrp, IO_NO_INCREMENT);
	return STATUS_SUCCESS;
}

/**
* PnP Request handler.
*
* @param  pDevObj    Device object.
* @param  pIrp       Request packet.
*/
static NTSTATUS NTAPI vgdrvWinPnP(PDEVICE_OBJECT pDevObj, PIRP pIrp)
{
	PVBOXGUESTDEVEXTWIN pDevExt;
	PIO_STACK_LOCATION pStack;
	ULONG MinorFunction;
	NTSTATUS status;
	
	status = STATUS_SUCCESS;
	pDevExt = (PVBOXGUESTDEVEXTWIN)pDevObj->DeviceExtension;
	pStack = IoGetCurrentIrpStackLocation(pIrp);

	MinorFunction = pStack->MinorFunction;
	switch (MinorFunction)
	{
		case IRP_MN_START_DEVICE:
		{
			status = vgdrvWinPnPSendIrpSynchronously(pDevExt->pNextLowerDriver, pIrp);
			if (NT_SUCCESS(status) && NT_SUCCESS(pIrp->IoStatus.Status))
            {
                if (pStack->Parameters.StartDevice.AllocatedResources)
                {
                    status = vgdrvWinSetupDevice(pDevExt, pDevObj, pIrp);
                }
                else
                {
                    status = STATUS_UNSUCCESSFUL;
                }
            }

			pIrp->IoStatus.Status = status;
            IoCompleteRequest(pIrp, IO_NO_INCREMENT);
			return status;
		}
		/*
         * Device and/or driver removal.  Destroy everything.
         */
		case IRP_MN_REMOVE_DEVICE:
		{
			pIrp->IoStatus.Status = STATUS_SUCCESS;

			IoSkipCurrentIrpStackLocation(pIrp);
			status = IoCallDriver(pDevExt->pNextLowerDriver, pIrp);

			IoSetDeviceInterfaceState(&pDevExt->SymLinkName, FALSE);
			RtlFreeUnicodeString(&pDevExt->SymLinkName);

			IoDetachDevice(pDevExt->pNextLowerDriver);
			IoDeleteDevice(pDevObj);
			return status;
		}
		default:
		{
			IoSkipCurrentIrpStackLocation(pIrp);
			status = IoCallDriver(pDevExt->pNextLowerDriver, pIrp);
			return status;
		}
	}
}

/**
* Handle the Power requests.
*
* @returns   NT status code
* @param     pDevObj   device object
* @param     pIrp      IRP
*/
static NTSTATUS NTAPI vgdrvWinPower(PDEVICE_OBJECT pDevObj, PIRP pIrp)
{
	PVBOXGUESTDEVEXTWIN pDevExt;
	pDevExt = (PVBOXGUESTDEVEXTWIN)pDevObj->DeviceExtension;
	PoStartNextPowerIrp(pIrp);
	IoSkipCurrentIrpStackLocation(pIrp);
	return PoCallDriver(pDevExt->pNextLowerDriver, pIrp);
}

/**
 * Helper to send a PnP IRP and wait until it's done.
 *
 * @returns NT status code.
 * @param    pDevObj    Device object.
 * @param    pIrp       Request packet.
 */
static NTSTATUS vgdrvWinPnPSendIrpSynchronously(PDEVICE_OBJECT pDevObj, PIRP pIrp)
{
    KEVENT Event;
	NTSTATUS rcNt;

    KeInitializeEvent(&Event, SynchronizationEvent, FALSE);

    IoCopyCurrentIrpStackLocationToNext(pIrp);
    IoSetCompletionRoutine(pIrp, (PIO_COMPLETION_ROUTINE)vgdrvWinPnpIrpComplete, &Event, TRUE, TRUE, TRUE);

    rcNt = IoCallDriver(pDevObj, pIrp);
    if (rcNt == STATUS_PENDING)
    {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        rcNt = pIrp->IoStatus.Status;
    }

    return rcNt;
}

/**
 * Irp completion routine for PnP Irps we send.
 *
 * @returns NT status code.
 * @param   pDevObj   Device object.
 * @param   pIrp      Request packet.
 * @param   pEvent    Semaphore.
 */
static NTSTATUS vgdrvWinPnpIrpComplete(PDEVICE_OBJECT pDevObj, PIRP pIrp, PKEVENT pEvent)
{
    KeSetEvent(pEvent, 0, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/**
 * Sets up the device and its resources.
 *
 * @param   pDevExt     Our device extension data.
 * @param   pDevObj     The device object.
 * @param   pIrp        The request packet.
 */
static NTSTATUS vgdrvWinSetupDevice(PVBOXGUESTDEVEXTWIN pDevExt, PDEVICE_OBJECT pDevObj, PIRP pIrp)
{
	NTSTATUS rcNt;
	if (!pIrp)
    {
        rcNt = STATUS_INTERNAL_ERROR;
    }
	else
	{
		PIO_STACK_LOCATION pStack = IoGetCurrentIrpStackLocation(pIrp);
		rcNt = vgdrvWinScanPCIResourceList(pDevExt, pStack->Parameters.StartDevice.AllocatedResourcesTranslated);
	}
	if (NT_SUCCESS(rcNt))
	{
		/*
		 * Map physical address of VMMDev memory into MMIO region
		 * and init the common device extension bits.
		 */
		void   *pvMMIOBase = NULL;
		UINT32 cbMMIO     = 0;
		rcNt = vgdrvWinMapVMMDevMemory(pDevExt,
									   pDevExt->uVmmDevMemoryPhysAddr,
									   pDevExt->cbVmmDevMemory,
									   &pvMMIOBase,
									   &cbMMIO);
		if (NT_SUCCESS(rcNt))
		{
			pDevExt->pVMMDevMemory = (VMMDevMemory *)pvMMIOBase;
			rcNt = VGDrvCommonInitDevExtResources(pDevExt, VMMDEV_EVENT_MOUSE_POSITION_CHANGED);
			if (NT_SUCCESS(rcNt))
			{
				//TODO: Allocate VMMDevPowerStateReq
				if (NT_SUCCESS(rcNt))
				{
					//IoInitializeDpcRequest(pDevExt->pDeviceObject, vgdrvWinDpcHandler);
					//if uInterruptVector:
					//IoConnectInterrupt
					//if success:
					//TODO: ReadConfiguration and return success.
					//else:
					//TODO: Free VMMDevPowerStateReq
				}
				else
				{
					rcNt = STATUS_UNSUCCESSFUL;
				}
				VGDrvCommonDeleteDevExtResources(pDevExt);
			}
			else
			{
				rcNt = STATUS_DEVICE_CONFIGURATION_ERROR;
			}
			vgdrvWinUnmapVMMDevMemory(pDevExt);
		}
	}
	return rcNt;
}

/**
 * Initializes the VBoxGuest device extension resource parts.
 *
 * The native code locates the VMMDev on the PCI bus and retrieve the MMIO and
 * I/O port ranges, this function will take care of mapping the MMIO memory (if
 * present).  Upon successful return the native code should set up the interrupt
 * handler.
 *
 * @returns VBox status code.
 *
 * @param   pDevExt         The device extension. Allocated by the native code.
 * @param   fFixedEvents    Events that will be enabled upon init and no client
 *                          will ever be allowed to mask.
 */
int VGDrvCommonInitDevExtResources(PVBOXGUESTDEVEXTWIN pDevExt, UINT32 fFixedEvents)
{
	int rc;
	DEVICE_DESCRIPTION devDesc;
	ULONG numberOfMapRegisters;
	PDMA_ADAPTER dmaAdapter = NULL;
	PVOID volatile dmaBufferVirt = NULL;
	PHYSICAL_ADDRESS dmaBufferPhys;
	VMMDevReqHostVersion reqHostVersion;
	VMMDevReportGuestInfo2 reportGuestInfo2;
	VMMDevReportGuestInfo reportGuestInfo;
	VMMDevCtlGuestFilterMask ctlGuestFilterMask;
	VMMDevReqGuestCapabilities2 reqGuestCapabilities2;
	VMMDevReqMouseStatus reqMouseStatus;
	VMMDevReportGuestStatus reportGuestStatus;

	/*
     * Initialize the guest library and report the guest info back to VMMDev,
     * set the interrupt control filter mask, and fixate the guest mappings
     * made by the VMM.
     */
	RtlZeroMemory(&devDesc, sizeof(DEVICE_DESCRIPTION));
	devDesc.Version = DEVICE_DESCRIPTION_VERSION;
	devDesc.Master = FALSE;
	devDesc.ScatterGather = FALSE;
	devDesc.InterfaceType = PCIBus;
	dmaAdapter = IoGetDmaAdapter(pDevExt->pDeviceObject, &devDesc, &numberOfMapRegisters);
	dmaBufferVirt = dmaAdapter->DmaOperations->AllocateCommonBuffer(dmaAdapter, PAGE_SIZE, &dmaBufferPhys, FALSE);
	pDevExt->dmaAdapter = dmaAdapter;
	pDevExt->dmaBufferSize = PAGE_SIZE;
	pDevExt->dmaBufferVirt = dmaBufferVirt;
	pDevExt->dmaBufferPhys = dmaBufferPhys;
	if (dmaBufferVirt)
	{
		RtlZeroMemory(dmaBufferVirt, PAGE_SIZE);
		DebugPrint("dmaBuffer (%d) Virt: %x Phys: %x", pDevExt->dmaBufferSize, pDevExt->dmaBufferVirt, pDevExt->dmaBufferPhys);
	}
	else
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	//TODO: Shared code for request function
	//vbglR0QueryHostVersion
	RtlFillMemory(&reqHostVersion, sizeof(VMMDevReqHostVersion), 0xAA);
	reqHostVersion.header.size = sizeof(VMMDevReqHostVersion);
	reqHostVersion.header.version = VMMDEV_REQUEST_HEADER_VERSION;
	reqHostVersion.header.requestType = VMMDevReq_GetHostVersion;
	reqHostVersion.header.rc = VERR_GENERAL_FAILURE;
	reqHostVersion.header.reserved1 = 0;
	reqHostVersion.header.fRequestor = VMMDEV_REQUESTOR_KERNEL | VMMDEV_REQUESTOR_USR_DRV;
	RtlCopyMemory(dmaBufferVirt, &reqHostVersion, sizeof(VMMDevReqHostVersion));

	WRITE_PORT_ULONG((PULONG)(pDevExt->uIoPortPhysAddr.LowPart), dmaBufferPhys.LowPart);
	KeMemoryBarrier();
//	DebugPrint("QueryHostVersion: %d.%d.%dr%d %x",
//               ((VMMDevReqHostVersion *)dmaBufferVirt)->major, ((VMMDevReqHostVersion *)dmaBufferVirt)->minor,
//				 ((VMMDevReqHostVersion *)dmaBufferVirt)->build, ((VMMDevReqHostVersion *)dmaBufferVirt)->revision,
//				 ((VMMDevReqHostVersion *)dmaBufferVirt)->features);
//	RtlCopyMemory(&reqHostVersion, dmaBufferVirt, sizeof(VMMDevReqHostVersion));
	pDevExt->HostFeatures = ((VMMDevReqHostVersion *)dmaBufferVirt)->features;
	DebugPrint("Host features: %x", pDevExt->HostFeatures);
	if (((VMMDevRequestHeader *)dmaBufferVirt)->rc != 0)
		return STATUS_NOT_SUPPORTED;

	//ReportGuestInfo
	RtlFillMemory(&reportGuestInfo2, sizeof(VMMDevReportGuestInfo2), 0xAA);
	reportGuestInfo2.header.size = sizeof(VMMDevReportGuestInfo2);
	reportGuestInfo2.header.version = VMMDEV_REQUEST_HEADER_VERSION;
	reportGuestInfo2.header.requestType = VMMDevReq_ReportGuestInfo2;
	reportGuestInfo2.header.rc = VERR_GENERAL_FAILURE;
	reportGuestInfo2.header.reserved1 = 0;
	reportGuestInfo2.header.fRequestor = VMMDEV_REQUESTOR_KERNEL | VMMDEV_REQUESTOR_USR_DRV;
	reportGuestInfo2.additionsMajor = VBOX_VERSION_MAJOR;
	reportGuestInfo2.additionsMinor = VBOX_VERSION_MINOR;
	reportGuestInfo2.additionsBuild = VBOX_VERSION_BUILD;
	reportGuestInfo2.additionsRevision = VBOX_SVN_REV;
	reportGuestInfo2.additionsFeatures = VBOXGSTINFO2_F_REQUESTOR_INFO;
	strcpy(reportGuestInfo2.szName, VBOX_VERSION_STRING);
	RtlCopyMemory(dmaBufferVirt, &reportGuestInfo2, sizeof(VMMDevReportGuestInfo2));
	WRITE_PORT_ULONG((PULONG)(pDevExt->uIoPortPhysAddr.LowPart), dmaBufferPhys.LowPart);
	KeMemoryBarrier();
	DebugPrint("ReportGuestInfo2 (%d) rc: %d",((VMMDevRequestHeader *)dmaBufferVirt)->requestType,
				((VMMDevRequestHeader *)dmaBufferVirt)->rc);

	RtlFillMemory(&reportGuestInfo, sizeof(VMMDevReportGuestInfo), 0xAA);
	reportGuestInfo.header.size = sizeof(VMMDevReportGuestInfo);
	reportGuestInfo.header.version = VMMDEV_REQUEST_HEADER_VERSION;
	reportGuestInfo.header.requestType = VMMDevReq_ReportGuestInfo;
	reportGuestInfo.header.rc = VERR_GENERAL_FAILURE;
	reportGuestInfo.header.reserved1 = 0;
	reportGuestInfo.header.fRequestor = VMMDEV_REQUESTOR_KERNEL | VMMDEV_REQUESTOR_USR_DRV;
	reportGuestInfo.interfaceVersion = VMMDEV_VERSION;
	reportGuestInfo.osType = g_enmVGDrvOsType;
	RtlCopyMemory(dmaBufferVirt, &reportGuestInfo, sizeof(VMMDevReportGuestInfo));
	WRITE_PORT_ULONG((PULONG)(pDevExt->uIoPortPhysAddr.LowPart), dmaBufferPhys.LowPart);
	KeMemoryBarrier();
	DebugPrint("ReportGuestInfo (%d) rc: %d",((VMMDevRequestHeader *)dmaBufferVirt)->requestType,
				((VMMDevRequestHeader *)dmaBufferVirt)->rc);
	if (((VMMDevRequestHeader *)dmaBufferVirt)->rc != 0)
		return STATUS_NOT_SUPPORTED;

	//ResetEventFilterOnHost
	RtlFillMemory(&ctlGuestFilterMask, sizeof(VMMDevCtlGuestFilterMask), 0xAA);
	ctlGuestFilterMask.header.size = sizeof(VMMDevCtlGuestFilterMask);
	ctlGuestFilterMask.header.version = VMMDEV_REQUEST_HEADER_VERSION;
	ctlGuestFilterMask.header.requestType = VMMDevReq_CtlGuestFilterMask;
	ctlGuestFilterMask.header.rc = VERR_GENERAL_FAILURE;
	ctlGuestFilterMask.header.reserved1 = 0;
	ctlGuestFilterMask.header.fRequestor = VMMDEV_REQUESTOR_KERNEL | VMMDEV_REQUESTOR_USR_DRV;
	ctlGuestFilterMask.u32OrMask = 0xFFFFFFFFU & ~fFixedEvents;
	ctlGuestFilterMask.u32NotMask = fFixedEvents;
	RtlCopyMemory(dmaBufferVirt, &ctlGuestFilterMask, sizeof(VMMDevCtlGuestFilterMask));
	WRITE_PORT_ULONG((PULONG)(pDevExt->uIoPortPhysAddr.LowPart), dmaBufferPhys.LowPart);
	KeMemoryBarrier();
	DebugPrint("CtlGuestFilterMask (%d) rc: %d",((VMMDevRequestHeader *)dmaBufferVirt)->requestType,
				((VMMDevRequestHeader *)dmaBufferVirt)->rc);
	if (((VMMDevRequestHeader *)dmaBufferVirt)->rc != 0)
		return STATUS_NOT_SUPPORTED;


	//ResetCapabilitiesOnHost
	RtlFillMemory(&reqGuestCapabilities2, sizeof(VMMDevReqGuestCapabilities2), 0xAA);
	reqGuestCapabilities2.header.size = sizeof(VMMDevReqGuestCapabilities2);
	reqGuestCapabilities2.header.version = VMMDEV_REQUEST_HEADER_VERSION;
	reqGuestCapabilities2.header.requestType = VMMDevReq_SetGuestCapabilities;
	reqGuestCapabilities2.header.rc = VERR_GENERAL_FAILURE;
	reqGuestCapabilities2.header.reserved1 = 0;
	reqGuestCapabilities2.header.fRequestor = VMMDEV_REQUESTOR_KERNEL | VMMDEV_REQUESTOR_USR_DRV;
	reqGuestCapabilities2.u32OrMask = 0xFFFFFFFFU;
	reqGuestCapabilities2.u32NotMask = 0;
	RtlCopyMemory(dmaBufferVirt, &reqGuestCapabilities2, sizeof(VMMDevReqGuestCapabilities2));
	WRITE_PORT_ULONG((PULONG)(pDevExt->uIoPortPhysAddr.LowPart), dmaBufferPhys.LowPart);
	KeMemoryBarrier();
	DebugPrint("ReqGuestCapabilities2 (%d) rc: %d",((VMMDevRequestHeader *)dmaBufferVirt)->requestType,
				((VMMDevRequestHeader *)dmaBufferVirt)->rc);
	if (((VMMDevRequestHeader *)dmaBufferVirt)->rc != 0)
		return STATUS_NOT_SUPPORTED;

	//ResetMouseStatusOnHost
	RtlFillMemory(&reqMouseStatus, sizeof(VMMDevReqMouseStatus), 0xAA);
	reqMouseStatus.header.size = sizeof(VMMDevReqMouseStatus);
	reqMouseStatus.header.version = VMMDEV_REQUEST_HEADER_VERSION;
	reqMouseStatus.header.requestType = VMMDevReq_SetMouseStatus;
	reqMouseStatus.header.rc = VERR_GENERAL_FAILURE;
	reqMouseStatus.header.reserved1 = 0;
	reqMouseStatus.header.fRequestor = VMMDEV_REQUESTOR_KERNEL | VMMDEV_REQUESTOR_USR_DRV;
	reqMouseStatus.mouseFeatures = 0;
	reqMouseStatus.pointerXPos = 0;
	reqMouseStatus.pointerYPos = 0;
	RtlCopyMemory(dmaBufferVirt, &reqMouseStatus, sizeof(VMMDevReqMouseStatus));
	WRITE_PORT_ULONG((PULONG)(pDevExt->uIoPortPhysAddr.LowPart), dmaBufferPhys.LowPart);
	KeMemoryBarrier();
	DebugPrint("ReqMouseStatus (%d) rc: %d",((VMMDevRequestHeader *)dmaBufferVirt)->requestType,
				((VMMDevRequestHeader *)dmaBufferVirt)->rc);
	if (((VMMDevRequestHeader *)dmaBufferVirt)->rc != 0)
		return STATUS_NOT_SUPPORTED;

	//InitFixateGuestMappings
	//HeartbeatInit
	//ReportDriverStatus
	RtlFillMemory(&reportGuestStatus, sizeof(VMMDevReportGuestStatus), 0xAA);
	reportGuestStatus.header.size = sizeof(VMMDevReportGuestStatus);
	reportGuestStatus.header.version = VMMDEV_REQUEST_HEADER_VERSION;
	reportGuestStatus.header.requestType = VMMDevReq_ReportGuestStatus;
	reportGuestStatus.header.rc = VERR_GENERAL_FAILURE;
	reportGuestStatus.header.reserved1 = 0;
	reportGuestStatus.header.fRequestor = VMMDEV_REQUESTOR_KERNEL | VMMDEV_REQUESTOR_USR_DRV;
	reportGuestStatus.guestStatus.facility = VBoxGuestFacilityType_VBoxGuestDriver;
	reportGuestStatus.guestStatus.status = VBoxGuestFacilityStatus_Active;
	reportGuestStatus.guestStatus.flags = 0;
	RtlCopyMemory(dmaBufferVirt, &reportGuestStatus, sizeof(VMMDevReportGuestStatus));
	WRITE_PORT_ULONG((PULONG)(pDevExt->uIoPortPhysAddr.LowPart), dmaBufferPhys.LowPart);
	KeMemoryBarrier();
	DebugPrint("ReportGuestStatus (%d) rc: %d",((VMMDevRequestHeader *)dmaBufferVirt)->requestType,
				((VMMDevRequestHeader *)dmaBufferVirt)->rc);

	return STATUS_SUCCESS;
}

/**
 * Counter to VGDrvCommonInitDevExtResources.
 *
 * @param   pDevExt         The device extension.
 */
void VGDrvCommonDeleteDevExtResources(PVBOXGUESTDEVEXTWIN pDevExt)
{
	if (pDevExt->dmaAdapter)
	{
		pDevExt->dmaAdapter->DmaOperations->FreeCommonBuffer(pDevExt->dmaAdapter, PAGE_SIZE, pDevExt->dmaBufferPhys,
			pDevExt->dmaBufferVirt, FALSE);
		pDevExt->dmaAdapter->DmaOperations->PutDmaAdapter(pDevExt->dmaAdapter);
	}
    pDevExt->pVMMDevMemory = NULL;
}

/**
 * DPC handler.
 *
 * @param   pDPC        DPC descriptor.
 * @param   pDevObj     Device object.
 * @param   pIrp        Interrupt request packet.
 * @param   pContext    Context specific pointer.
 */
static void NTAPI vgdrvWinDpcHandler(PKDPC pDPC, PDEVICE_OBJECT pDevObj, PIRP pIrp, PVOID pContext)
{
	//TODO
}

/**
 * Helper to scan the PCI resource list and remember stuff.
 *
 * @param   pDevExt         The device extension.
 * @param   pResList        Resource list
 */
static NTSTATUS vgdrvWinScanPCIResourceList(PVBOXGUESTDEVEXTWIN pDevExt, PCM_RESOURCE_LIST pResList)
{
	ULONG i;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR pPartialData = NULL;
    BOOLEAN                         fGotIrq      = FALSE;
    BOOLEAN                         fGotMmio     = FALSE;
    BOOLEAN                         fGotIoPorts  = FALSE;
	BOOLEAN                         fGotDma  = FALSE;
    NTSTATUS                        rc           = STATUS_SUCCESS;
    for (i = 0; i < pResList->List->PartialResourceList.Count; i++)
    {
        pPartialData = &pResList->List->PartialResourceList.PartialDescriptors[i];
        switch (pPartialData->Type)
        {
            case CmResourceTypePort:
                /* Save the first I/O port base. */
                if (!fGotIoPorts)
                {
                    pDevExt->uIoPortPhysAddr = pPartialData->u.Port.Start;
					pDevExt->cbIoPort = pPartialData->u.Port.Length;
                    fGotIoPorts = TRUE;
					DebugPrint("pDevExt->uIoPortPhysAddr: %x", pDevExt->uIoPortPhysAddr);
					DebugPrint("pDevExt->cbIoPort: %u", pDevExt->cbIoPort);
					if (pPartialData->Flags & CM_RESOURCE_PORT_IO)
						DebugPrint("CM_RESOURCE_PORT_IO is set");
                }
                break;

            case CmResourceTypeInterrupt:
                if (!fGotIrq)
                {
                    /* Save information. */
                    pDevExt->uInterruptLevel    = pPartialData->u.Interrupt.Level;
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
                if (!fGotMmio && (pPartialData->Flags == CM_RESOURCE_MEMORY_READ_WRITE))
                {
                    /* Save physical MMIO base + length for VMMDev. */
                    pDevExt->uVmmDevMemoryPhysAddr = pPartialData->u.Memory.Start;
                    pDevExt->cbVmmDevMemory = pPartialData->u.Memory.Length;
					DebugPrint("pDevExt->uVmmDevMemoryPhysAddr: %x", pDevExt->uVmmDevMemoryPhysAddr);
					DebugPrint("pDevExt->cbVmmDevMemory: %u", pDevExt->cbVmmDevMemory);
                    fGotMmio = TRUE;
                }
                break;
            default:
                break;
        }
    }
    return rc;
}

/**
 * Maps the I/O space from VMMDev to virtual kernel address space.
 *
 * @return NTSTATUS
 *
 * @param pDevExt           The device extension.
 * @param PhysAddr          Physical address to map.
 * @param cbToMap           Number of bytes to map.
 * @param ppvMMIOBase       Pointer of mapped I/O base.
 * @param pcbMMIO           Length of mapped I/O base.
 */
static NTSTATUS vgdrvWinMapVMMDevMemory(PVBOXGUESTDEVEXTWIN pDevExt, PHYSICAL_ADDRESS PhysAddr, ULONG cbToMap,
                                       void **ppvMMIOBase, UINT32 *pcbMMIO)
{
	NTSTATUS rc = STATUS_SUCCESS;
    if (PhysAddr.LowPart > 0) /* We're mapping below 4GB. */
    {
         VMMDevMemory *pVMMDevMemory = (VMMDevMemory *)MmMapIoSpace(PhysAddr, cbToMap, MmNonCached);
         if (pVMMDevMemory)
         {
             /* Check version of the structure; do we have the right memory version? */
             if (pVMMDevMemory->u32Version == VMMDEV_MEMORY_VERSION)
             {
                 /* Save results. */
                 *ppvMMIOBase = pVMMDevMemory;
                 if (pcbMMIO) /* Optional. */
                     *pcbMMIO = pVMMDevMemory->u32Size;
				 DebugPrint("vgdrvWinMapVMMDevMemory: VMMDevMemory: mapping=%x size=%d version=%d",
							pVMMDevMemory, pVMMDevMemory->u32Size, pVMMDevMemory->u32Version);
             }
             else
             {
                 /* Not our version, refuse operation and unmap the memory. */
				 DebugPrint("vgdrvWinMapVMMDevMemory: Bogus VMMDev memory; u32Version=%d (expected %d) u32Size=%d",
							pVMMDevMemory->u32Version, VMMDEV_MEMORY_VERSION, pVMMDevMemory->u32Size);
                 vgdrvWinUnmapVMMDevMemory(pDevExt);
                 rc = STATUS_UNSUCCESSFUL;
             }
         }
         else
             rc = STATUS_UNSUCCESSFUL;
    }
    return rc;
}

/**
 * Unmaps the VMMDev I/O range from kernel space.
 *
 * @param   pDevExt     The device extension.
 */
static void vgdrvWinUnmapVMMDevMemory(PVBOXGUESTDEVEXTWIN pDevExt)
{
    if (pDevExt->pVMMDevMemory)
    {
        MmUnmapIoSpace((void*)pDevExt->pVMMDevMemory, pDevExt->cbVmmDevMemory);
        pDevExt->pVMMDevMemory = NULL;
    }

    pDevExt->uVmmDevMemoryPhysAddr.QuadPart = 0;
    pDevExt->cbVmmDevMemory = 0;
}

