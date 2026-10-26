/** @file
 * VBoxGst - Driver entry point, device creation/deletion and file handles.
 */

#include <wdm.h>
#include <initguid.h>   /* Instantiates GUID_DEVINTERFACE_VBOXGUEST98 in this file. */
#include "VBoxGst.h"


/*********************************************************************************************************************************
*   Internal Functions                                                                                                           *
*********************************************************************************************************************************/

NTSTATUS DriverEntry(PDRIVER_OBJECT pDrvObj, PUNICODE_STRING pRegPath);
static NTSTATUS NTAPI vgdrvWinAddDevice(PDRIVER_OBJECT pDrvObj, PDEVICE_OBJECT pPdo);
static VOID     NTAPI vgdrvWinUnload(PDRIVER_OBJECT pDrvObj);
static NTSTATUS NTAPI vgdrvWinCreate(PDEVICE_OBJECT pDevObj, PIRP pIrp);
static NTSTATUS NTAPI vgdrvWinCleanup(PDEVICE_OBJECT pDevObj, PIRP pIrp);
static NTSTATUS NTAPI vgdrvWinClose(PDEVICE_OBJECT pDevObj, PIRP pIrp);
static NTSTATUS NTAPI vgdrvWinSystemControl(PDEVICE_OBJECT pDevObj, PIRP pIrp);
static NTSTATUS NTAPI vgdrvWinNotSupported(PDEVICE_OBJECT pDevObj, PIRP pIrp);


/*********************************************************************************************************************************
*   Global Variables                                                                                                             *
*********************************************************************************************************************************/

/** The guest OS type, determined in DriverEntry. */
static VBOXOSTYPE g_enmVGDrvOsType = VBOXOSTYPE_UNKNOWN;


/**
 * Driver entry point.
 *
 * @returns appropriate status code.
 * @param   pDrvObj     Pointer to driver object.
 * @param   pRegPath    Registry base path.
 */
NTSTATUS DriverEntry(PDRIVER_OBJECT pDrvObj, PUNICODE_STRING pRegPath)
{
    ULONG i;

    VGDrvLogInit();

    /*
     * Check where we're running:
     *  - Windows 2000 and newer support WDM 1.10 and below (1, 0x10).
     *  - Windows ME supports WDM 1.05 and below (1, 0x05).
     *  - Windows 98 supports WDM 1.00 and below (1, 0x00).
     */
    if (IoIsWdmVersionAvailable(1, 0x10))
        g_enmVGDrvOsType = VBOXOSTYPE_UNKNOWN;  /* Windows 2000 or newer, use the upstream driver. */
    else if (IoIsWdmVersionAvailable(1, 0x05))
        g_enmVGDrvOsType = VBOXOSTYPE_WINME;
    else if (IoIsWdmVersionAvailable(1, 0x00))
        g_enmVGDrvOsType = VBOXOSTYPE_WIN98;

    if (g_enmVGDrvOsType == VBOXOSTYPE_UNKNOWN)
    {
        VGDrvLogRel("not loading, Windows 2000 or newer needs the upstream driver\n");
        return STATUS_DRIVER_UNABLE_TO_LOAD;
    }
    VGDrvLogRel("VBoxGuest98 %s r%u on Windows %s\n", VBOX_VERSION_STRING, VBOX_SVN_REV,
                g_enmVGDrvOsType == VBOXOSTYPE_WINME ? "ME" : "98");

    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++)
        pDrvObj->MajorFunction[i] = vgdrvWinNotSupported;

    pDrvObj->DriverExtension->AddDevice                      = vgdrvWinAddDevice;
    pDrvObj->DriverUnload                                    = vgdrvWinUnload;
    pDrvObj->MajorFunction[IRP_MJ_CREATE]                    = vgdrvWinCreate;
    pDrvObj->MajorFunction[IRP_MJ_CLEANUP]                   = vgdrvWinCleanup;
    pDrvObj->MajorFunction[IRP_MJ_CLOSE]                     = vgdrvWinClose;
    pDrvObj->MajorFunction[IRP_MJ_DEVICE_CONTROL]            = vgdrvWinIoCtl;
    pDrvObj->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL]   = vgdrvWinInternalIoCtl;
    pDrvObj->MajorFunction[IRP_MJ_PNP]                       = vgdrvWinPnP;
    pDrvObj->MajorFunction[IRP_MJ_POWER]                     = vgdrvWinPower;
    pDrvObj->MajorFunction[IRP_MJ_SYSTEM_CONTROL]            = vgdrvWinSystemControl;
    return STATUS_SUCCESS;
}

/**
 * Handle request from the Plug & Play subsystem.
 *
 * Creates the functional device object, the DOS symbolic link and the device
 * interface, and attaches to the device stack.
 *
 * @returns NT status code
 * @param   pDrvObj   Driver object
 * @param   pPdo      Physical device object
 */
static NTSTATUS NTAPI vgdrvWinAddDevice(PDRIVER_OBJECT pDrvObj, PDEVICE_OBJECT pPdo)
{
    UNICODE_STRING DevName;
    UNICODE_STRING DosName;
    PDEVICE_OBJECT pDeviceObject = NULL;
    PVBOXGUESTDEVEXTWIN pDevExt;
    NTSTATUS rcNt;

    RtlInitUnicodeString(&DevName, VBOXGUEST98_DEVICE_NAME_NT);
    rcNt = IoCreateDevice(pDrvObj, sizeof(VBOXGUESTDEVEXTWIN), &DevName, FILE_DEVICE_UNKNOWN, 0, FALSE, &pDeviceObject);
    if (!NT_SUCCESS(rcNt))
        return rcNt;

    pDevExt = (PVBOXGUESTDEVEXTWIN)pDeviceObject->DeviceExtension;
    RtlZeroMemory(pDevExt, sizeof(*pDevExt));
    pDevExt->pDeviceObject         = pDeviceObject;
    pDevExt->pPhysicalDeviceObject = pPdo;
    pDevExt->enmDevState           = VGDRVWINDEVSTATE_STOPPED;

    if (RT_SUCCESS(VGDrvCommonInitDevExtFundament(&pDevExt->Core, g_enmVGDrvOsType)))
    {
        RtlInitUnicodeString(&DosName, VBOXGUEST98_DEVICE_NAME_DOS);
        rcNt = IoCreateSymbolicLink(&DosName, &DevName);
        if (NT_SUCCESS(rcNt))
        {
            pDevExt->pNextLowerDriver = IoAttachDeviceToDeviceStack(pDeviceObject, pPdo);
            if (pDevExt->pNextLowerDriver)
            {
                /* The device interface is how user mode finds us on Windows 98
                   (VBoxTray), failure is not fatal. */
                rcNt = IoRegisterDeviceInterface(pPdo, &GUID_DEVINTERFACE_VBOXGUEST98, NULL, &pDevExt->InterfaceName);
                if (!NT_SUCCESS(rcNt))
                    pDevExt->InterfaceName.Buffer = NULL;

                IoInitializeDpcRequest(pDeviceObject, vgdrvWinDpcHandler);

                /* Ensure we are not called at elevated IRQL, even if our code isn't pagable any more. */
                pDeviceObject->Flags |= DO_POWER_PAGABLE | DO_BUFFERED_IO;

                /* Driver is ready now. */
                pDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
                return STATUS_SUCCESS;
            }

            rcNt = STATUS_DEVICE_NOT_CONNECTED;
            IoDeleteSymbolicLink(&DosName);
        }
        VGDrvCommonDeleteDevExtFundament(&pDevExt->Core);
    }
    else
        rcNt = STATUS_INSUFFICIENT_RESOURCES;

    IoDeleteDevice(pDeviceObject);
    return rcNt;
}

/**
 * Counter part to vgdrvWinAddDevice, used by IRP_MN_REMOVE_DEVICE.
 *
 * The hardware resources must have been released already.
 *
 * @param   pDevExt     The device extension.
 */
void vgdrvWinDeleteDevice(PVBOXGUESTDEVEXTWIN pDevExt)
{
    UNICODE_STRING DosName;
    PDEVICE_OBJECT pDeviceObject = pDevExt->pDeviceObject;

    if (pDevExt->InterfaceName.Buffer)
    {
        IoSetDeviceInterfaceState(&pDevExt->InterfaceName, FALSE);
        RtlFreeUnicodeString(&pDevExt->InterfaceName);
        pDevExt->InterfaceName.Buffer = NULL;
    }

    RtlInitUnicodeString(&DosName, VBOXGUEST98_DEVICE_NAME_DOS);
    IoDeleteSymbolicLink(&DosName);

    VGDrvCommonDeleteDevExtFundament(&pDevExt->Core);

    if (pDevExt->pNextLowerDriver)
    {
        IoDetachDevice(pDevExt->pNextLowerDriver);
        pDevExt->pNextLowerDriver = NULL;
    }
    IoDeleteDevice(pDeviceObject);
}

/**
 * Unload the driver.  PnP has removed all devices by now.
 *
 * @param   pDrvObj     Driver object.
 */
static VOID NTAPI vgdrvWinUnload(PDRIVER_OBJECT pDrvObj)
{
    /* Nothing to do, the devices and their resources are gone already. */
}

/**
 * Completes an IRP.
 *
 * @returns rcNt.
 * @param   pIrp        The request packet.
 * @param   rcNt        The status.
 * @param   cbInfo      Value for IoStatus.Information.
 */
NTSTATUS vgdrvWinCompleteRequest(PIRP pIrp, NTSTATUS rcNt, ULONG_PTR cbInfo)
{
    pIrp->IoStatus.Status      = rcNt;
    pIrp->IoStatus.Information = cbInfo;
    IoCompleteRequest(pIrp, IO_NO_INCREMENT);
    return rcNt;
}

/**
 * Create (i.e. Open) file entry point.  Creates a session for the handle.
 *
 * @param   pDevObj     Device object.
 * @param   pIrp        Request packet.
 */
static NTSTATUS NTAPI vgdrvWinCreate(PDEVICE_OBJECT pDevObj, PIRP pIrp)
{
    PVBOXGUESTDEVEXTWIN pDevExt = (PVBOXGUESTDEVEXTWIN)pDevObj->DeviceExtension;
    PIO_STACK_LOCATION pStack = IoGetCurrentIrpStackLocation(pIrp);
    PFILE_OBJECT pFileObj = pStack->FileObject;
    PVBOXGUESTSESSION pSession;
    int rc;

    if (pDevExt->enmDevState == VGDRVWINDEVSTATE_REMOVED)
        return vgdrvWinCompleteRequest(pIrp, STATUS_DELETE_PENDING, 0);
    if (!pFileObj)
        return vgdrvWinCompleteRequest(pIrp, STATUS_INVALID_PARAMETER, 0);

    rc = VGDrvCommonCreateSession(&pDevExt->Core, (BOOLEAN)(pIrp->RequestorMode == UserMode), &pSession);
    if (RT_FAILURE(rc))
        return vgdrvWinCompleteRequest(pIrp, vgdrvRcToNtStatus(rc), 0);

    pFileObj->FsContext = pSession;
    return vgdrvWinCompleteRequest(pIrp, STATUS_SUCCESS, 0);
}

/**
 * Cleanup entry point: the last handle is being closed, wake up its waiters.
 *
 * @param   pDevObj     Device object.
 * @param   pIrp        Request packet.
 */
static NTSTATUS NTAPI vgdrvWinCleanup(PDEVICE_OBJECT pDevObj, PIRP pIrp)
{
    PVBOXGUESTDEVEXTWIN pDevExt = (PVBOXGUESTDEVEXTWIN)pDevObj->DeviceExtension;
    PFILE_OBJECT pFileObj = IoGetCurrentIrpStackLocation(pIrp)->FileObject;

    if (pFileObj && pFileObj->FsContext)
        VGDrvCommonInterruptWaits(&pDevExt->Core, (PVBOXGUESTSESSION)pFileObj->FsContext);
    return vgdrvWinCompleteRequest(pIrp, STATUS_SUCCESS, 0);
}

/**
 * Close file entry point: destroys the session.
 *
 * @param   pDevObj     Device object.
 * @param   pIrp        Request packet.
 */
static NTSTATUS NTAPI vgdrvWinClose(PDEVICE_OBJECT pDevObj, PIRP pIrp)
{
    PVBOXGUESTDEVEXTWIN pDevExt = (PVBOXGUESTDEVEXTWIN)pDevObj->DeviceExtension;
    PFILE_OBJECT pFileObj = IoGetCurrentIrpStackLocation(pIrp)->FileObject;

    if (pFileObj && pFileObj->FsContext)
    {
        VGDrvCommonCloseSession(&pDevExt->Core, (PVBOXGUESTSESSION)pFileObj->FsContext);
        pFileObj->FsContext = NULL;
    }
    return vgdrvWinCompleteRequest(pIrp, STATUS_SUCCESS, 0);
}

/**
 * WMI requests are not handled, pass them down.
 */
static NTSTATUS NTAPI vgdrvWinSystemControl(PDEVICE_OBJECT pDevObj, PIRP pIrp)
{
    PVBOXGUESTDEVEXTWIN pDevExt = (PVBOXGUESTDEVEXTWIN)pDevObj->DeviceExtension;
    IoSkipCurrentIrpStackLocation(pIrp);
    return IoCallDriver(pDevExt->pNextLowerDriver, pIrp);
}

/**
 * Handler for all the IRP_MJ_XXX we don't support (read, write, ...).
 */
static NTSTATUS NTAPI vgdrvWinNotSupported(PDEVICE_OBJECT pDevObj, PIRP pIrp)
{
    return vgdrvWinCompleteRequest(pIrp, STATUS_NOT_SUPPORTED, 0);
}
