#include "VBoxGst.h"

static VBOXOSTYPE           g_enmVGDrvOsType = VBOXOSTYPE_UNKNOWN;
static VBGLDATA             g_vbgldata;

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
* @param   pDevObj   Device object
*/
static NTSTATUS NTAPI vgdrvWinAddDevice(PDRIVER_OBJECT pDrvObj, PDEVICE_OBJECT pDevObj)
{
	NTSTATUS retStatus = STATUS_SUCCESS;
	UNICODE_STRING DevName;
	UNICODE_STRING DosName;
	PDEVICE_OBJECT pDeviceObject = NULL;
	PVBOXGUESTDEVEXTWIN pDevExt = NULL;
	
	//Create device.
	RtlInitUnicodeString(&DevName, VBOXGUEST_DEVICE_NAME_NT);
	retStatus = IoCreateDevice(pDrvObj, sizeof(VBOXGUESTDEVEXTWIN), &DevName, FILE_DEVICE_UNKNOWN, 0, FALSE, &pDeviceObject);
	if (NT_SUCCESS(retStatus))
	{
		//Create symbolic link (DOS devices).
		RtlInitUnicodeString(&DosName, VBOXGUEST_DEVICE_NAME_DOS);
		retStatus = IoCreateSymbolicLink(&DosName, &DevName);
		if (NT_SUCCESS(retStatus))
		{
			//Setup the device extension.
			pDevExt = (PVBOXGUESTDEVEXTWIN)pDeviceObject->DeviceExtension;
			retStatus = vgdrvWinInitDevExtFundament(pDevExt, pDevObj);
			if (NT_SUCCESS(retStatus))
			{
				pDevExt->pNextLowerDriver = IoAttachDeviceToDeviceStack(pDeviceObject, pDevObj);
				if (pDevExt->pNextLowerDriver != NULL)
			    {
					/* Ensure we are not called at elevated IRQL, even if our code isn't pagable any more. */
                    pDeviceObject->Flags |= DO_POWER_PAGABLE;

                    /* Driver is ready now. */
                    pDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
					
					return retStatus;
				}
				//IoAttachDeviceToDeviceStack did not give a nextLowerDriver!
			    retStatus = STATUS_DEVICE_NOT_CONNECTED;
			    vgdrvWinDeleteDevExtFundament(pDevExt);
			}
			
			IoDeleteSymbolicLink(&DosName);
		}
		else
		{
			//IoCreateSymbolicLink failed!
			IoDeleteDevice(pDeviceObject);
		}
	}
	return retStatus;
	
//	retStatus = IoRegisterDeviceInterface(pDevObj, &WDM_GUID, NULL, &pDevExt->SymLinkName);
//	if (!NT_SUCCESS(retStatus))
//	{
//		//IoRegisterDeviceInterface failed!
//		IoDeleteDevice(pDeviceObject);
//		return retStatus;
//	}
//	
//	IoSetDeviceInterfaceState(&pDevExt->SymLinkName, TRUE);
//	//TODO: Initialize ext fundamentals
//	pDevExt->pDeviceObject = pDeviceObject;
}

/**
* Does the fundamental device extension initialization.
*
* @returns NT status.
* @param   pDevExt             The device extension.
* @param   pDevObj             The device object.
*/
static NTSTATUS vgdrvWinInitDevExtFundament(PVBOXGUESTDEVEXTWIN pDevExt, PDEVICE_OBJECT pDevObj)
{
	NTSTATUS rc;
	
	RtlZeroMemory(pDevObj, sizeof(PDEVICE_OBJECT));
	
	KeInitializeSpinLock(&pDevExt->MouseEventAccessSpinLock);
	pDevExt->pDeviceObject   = pDevObj;
	
	rc = VGDrvCommonInitDevExtFundament(&pDevExt->Core);
	if (NT_SUCCESS(rc))
	{
		return STATUS_SUCCESS;
	}
	return STATUS_UNSUCCESSFUL;
}

/**
 * Initialize the device extension fundament.
 *
 * There are no device resources at this point, VGDrvCommonInitDevExtResources
 * should be called when they are available.
 *
 * @returns VBox status code.
 * @param   pDevExt         The device extension to init.
 */
NTSTATUS VGDrvCommonInitDevExtFundament(PVBOXGUESTDEVEXT pDevExt)
{
	NTSTATUS rc = STATUS_SUCCESS;
	
	// Initialize the data.
	pDevExt->fHostFeatures = 0;
	
	return rc;
}

/**
* Counter part to vgdrvWinInitDevExtFundament.
*
* @param   pDevExt             The device extension.
*/
static void vgdrvWinDeleteDevExtFundament(PVBOXGUESTDEVEXTWIN pDevExt)
{
	VGDrvCommonDeleteDevExtFundament(&pDevExt->Core);
}

/**
 * Counter to VGDrvCommonInitDevExtFundament.
 *
 * @param   pDevExt         The device extension.
 */
void VGDrvCommonDeleteDevExtFundament(PVBOXGUESTDEVEXT pDevExt)
{
    //pDevExt->uInitState = VBOXGUESTDEVEXT_INIT_STATE_DELETED;
}

/**
* Unload the driver.
*
* @param   pDrvObj     Driver object.
*/
static void NTAPI vgdrvWinUnload(PDRIVER_OBJECT pDrvObj)
{
	PDEVICE_OBJECT pDevObj = pDrvObj->DeviceObject;
	if (pDevObj)
	{
		PVBOXGUESTDEVEXTWIN pDevExt = (PVBOXGUESTDEVEXTWIN)pDevObj->DeviceExtension;
		
		vgdrvWinDeleteDeviceResources(pDevExt);
		vgdrvWinDeleteDeviceFundamentAndUnlink(pDevObj, pDevExt);
	}

	DebugPrintMsg("vgdrvWinUnload");
	DebugPrintClose();
}

/**
 * Deletes the device hardware resources.
 *
 * Used during removal, stopping and legacy module unloading.
 *
 * @param   pDevExt         The device extension.
 */
static void vgdrvWinDeleteDeviceResources(PVBOXGUESTDEVEXTWIN pDevExt)
{
    if (pDevExt->pInterruptObject)
    {
        IoDisconnectInterrupt(pDevExt->pInterruptObject);
        pDevExt->pInterruptObject = NULL;
    }
    //pDevExt->pPowerStateRequest = NULL; /* Will be deleted by the following call. */
    //if (pDevExt->Core.uInitState == VBOXGUESTDEVEXT_INIT_STATE_RESOURCES)
    //    VGDrvCommonDeleteDevExtResources(&pDevExt->Core);
    vgdrvWinUnmapVMMDevMemory(pDevExt);
}

/**
 * Deletes the device extension fundament and unlinks the device
 *
 * Used during removal and legacy module unloading.  Must have called
 * vgdrvNtDeleteDeviceResources.
 *
 * @param   pDevObj         Device object.
 * @param   pDevExt         The device extension.
 */
static void vgdrvWinDeleteDeviceFundamentAndUnlink(PDEVICE_OBJECT pDevObj, PVBOXGUESTDEVEXTWIN pDevExt)
{
	UNICODE_STRING DosName;
	
    /*
     * Delete the remainder of the device extension.
     */
    vgdrvWinDeleteDevExtFundament(pDevExt);

    /*
     * Delete the DOS symlink to the device and finally the device itself.
     */
    RtlInitUnicodeString(&DosName, VBOXGUEST_DEVICE_NAME_DOS);
    IoDeleteSymbolicLink(&DosName);

    IoDeleteDevice(pDevObj);
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
			/* This must be handled first by the lower driver. */
			status = vgdrvWinPnPSendIrpSynchronously(pDevExt->pNextLowerDriver, pIrp, TRUE);
			if (   NT_SUCCESS(status)
			    && NT_SUCCESS(pIrp->IoStatus.Status))
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

//			IoSetDeviceInterfaceState(&pDevExt->SymLinkName, FALSE);
//			RtlFreeUnicodeString(&pDevExt->SymLinkName);

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
static NTSTATUS vgdrvWinPnPSendIrpSynchronously(PDEVICE_OBJECT pDevObj, PIRP pIrp, BOOLEAN fStrict)
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
    
    if (   !fStrict
	    && (rcNt == STATUS_NOT_SUPPORTED || rcNt == STATUS_INVALID_DEVICE_REQUEST))
    {
		rcNt = STATUS_SUCCESS;
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
			pDevExt->Core.pVMMDevMemory = (VMMDevMemory *)pvMMIOBase;
			//rcNt = VGDrvCommonInitDevExtResources(&pDevExt->Core,
			rcNt = VGDrvCommonInitDevExtResources(pDevExt,
                                                  pDevExt->Core.IOPortBase,
                                                  pvMMIOBase, cbMMIO,
                                                  VMMDEV_EVENT_MOUSE_POSITION_CHANGED);
			if (NT_SUCCESS(rcNt))
			{
				//TODO: Allocate VMMDevPowerStateReq
				if (NT_SUCCESS(rcNt))
				{
					ULONG uInterruptVector = pDevExt->uInterruptVector;
					KIRQL uHandlerIrql = (KIRQL)pDevExt->uInterruptLevel;
					IoInitializeDpcRequest(pDevExt->pDeviceObject, vgdrvWinDpcHandler);
					if (uInterruptVector)
					{
						//rcNt = IoConnectInterrupt
					}
					if (NT_SUCCESS(rcNt))
					{
						//TODO: ReadConfiguration and return success.
						return STATUS_SUCCESS;
					}
					pDevExt->pInterruptObject = NULL;
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
 * @param   IOPortBase      The base of the I/O port range.
 * @param   pvMmioReq       The base of the MMIO request region.
 * @param   pvMMIOBase      The base of the MMIO memory mapping.
 *                          This is optional, pass NULL if not present.
 * @param   cbMMIO          The size of the MMIO memory mapping.
 *                          This is optional, pass 0 if not present.
 * @param   fFixedEvents    Events that will be enabled upon init and no client
 *                          will ever be allowed to mask.
 */
//int VGDrvCommonInitDevExtResources(PVBOXGUESTDEVEXT pDevExt, UINT16 IOPortBase,
int VGDrvCommonInitDevExtResources(PVBOXGUESTDEVEXTWIN pDevExt, UINT16 IOPortBase,
                                   void *pvMMIOBase, UINT32 cbMMIO,
                                   UINT32 fFixedEvents)
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

    if (pvMMIOBase)
    {
    	VMMDevMemory *pVMMDev = (VMMDevMemory *)pvMMIOBase;
    	if (    pVMMDev->u32Version == VMMDEV_MEMORY_VERSION
    	    &&  pVMMDev->u32Size >= 32
		    &&  pVMMDev->u32Size <= cbMMIO)
	    {
	    	//pDevExt->pVMMDevMemory = pVMMDev;
	    	pDevExt->Core.pVMMDevMemory = pVMMDev;
		}
	}

	/*
     * Initialize the guest library and report the guest info back to VMMDev,
     * set the interrupt control filter mask, and fixate the guest mappings
     * made by the VMM.
     */
//    pDevExt->IOPortBase   = IOPortBase;
	pDevExt->Core.IOPortBase   = IOPortBase;
//    rc = VbglR0InitPrimary(pDevExt->IOPortBase, (VMMDevMemory *)pDevExt->pVMMDevMemory, &pDevExt->fHostFeatures);
    
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
	pDevExt->Core.fHostFeatures = ((VMMDevReqHostVersion *)dmaBufferVirt)->features;
	DebugPrint("Host features: %x", pDevExt->Core.fHostFeatures);
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
    //pDevExt->pVMMDevMemory = NULL;
    pDevExt->Core.pVMMDevMemory = NULL;
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
	BOOLEAN                         fGotDma      = FALSE;
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
    if (pDevExt->Core.pVMMDevMemory)
    {
        MmUnmapIoSpace((void*)pDevExt->Core.pVMMDevMemory, pDevExt->cbVmmDevMemory);
        pDevExt->Core.pVMMDevMemory = NULL;
    }

    pDevExt->uVmmDevMemoryPhysAddr.QuadPart = 0;
    pDevExt->cbVmmDevMemory = 0;
}

//int VbglR0InitPrimary(UINT16 portVMMDev, VMMDevMemory *pVMMDevMemory, UINT32 *pfFeatures)
//{
//	RtlZeroMemory(g_vbgldata, sizeof(VBGLDATA));
//	g_vbgldata.portVMMDev = portVMMDev;
//	g_vbgldata.pVMMDevMemory = pVMMDevMemory;
//	vbglR0QueryHostVersion();
//	*pfFeatures = g_vbgldata.hostVersion.features;
//	return STATUS_SUCCESS;
//}
//
///**
// * Used by vbglR0QueryDriverInfo and VbglInit to try get the host feature mask
// * and version information (g_vbgldata::hostVersion).
// *
// * This was first implemented by the host in 3.1 and we quietly ignore failures
// * for that reason.
// */
//static void vbglR0QueryHostVersion(void)
//{
//    VMMDevReqHostVersion *pReq;
//    int rc = VbglR0GRAlloc((VMMDevRequestHeader **) &pReq, sizeof (*pReq), VMMDevReq_GetHostVersion);
//    if (RT_SUCCESS(rc))
//    {
//        rc = VbglR0GRPerform(&pReq->header);
//        if (RT_SUCCESS(rc))
//        {
//            g_vbgldata.hostVersion = *pReq;
//        }
//        
//        VbglR0GRFree(&pReq->header);
//    }
//}
//
//int VbglR0GRAlloc(VMMDevRequestHeader **ppReq, size_t cbReq, VMMDevRequestType enmReqType)
//{
//	
//}
//
//void VbglR0GRFree(VMMDevRequestHeader *pReq)
//{
//	
//}
