#include <wdm.h>
#include <initguid.h>

#include "DebugPrint.h"

#define VBOX_VERSION_MAJOR  (6)
#define VBOX_VERSION_MINOR  (1)
#define VBOX_VERSION_BUILD  (6)
#define VBOX_SVN_REV        (137129)
#define VBOX_VERSION_STRING "6.1.6"

// {719B4B48-C5DF-4474-A8E7-AF615CA3BC83}
DEFINE_GUID(WDM_GUID, 
			0x719b4b48, 0xc5df, 0x4474, 0xa8, 0xe7, 0xaf, 0x61, 0x5c, 0xa3, 0xbc, 0x83);

typedef enum VBOXOSTYPE
{
    VBOXOSTYPE_UNKNOWN    = 0,
	VBOXOSTYPE_WIN98      = 0x22000,
	VBOXOSTYPE_WINME      = 0x23000,
	VBOXOSTYPE_32BIT_HACK = 0x7fffffff
} VBOXOSTYPE;

typedef struct VMMDevMemory
{
    UINT32 u32Size;
    UINT32 u32Version;
    union
    {
        struct
        {
            BOOLEAN fHaveEvents;
        } V1_04;
        struct
        {
            UINT32 u32HostEvents;
            UINT32 u32GuestEventMask;
        } V1_03;
    } V;
} VMMDevMemory;

typedef struct VBOXGUESTDEVEXTWIN
{
	PHYSICAL_ADDRESS uIoPortPhysAddr;
	ULONG cbIoPort;

	VMMDevMemory volatile *pVMMDevMemory;
    PHYSICAL_ADDRESS uVmmDevMemoryPhysAddr;
    ULONG cbVmmDevMemory;

	ULONG uInterruptLevel;
    ULONG uInterruptVector;
    KAFFINITY fInterruptAffinity;
	KINTERRUPT_MODE enmInterruptMode;

	PDMA_ADAPTER dmaAdapter;
	ULONG dmaBufferSize;
	PVOID dmaBufferVirt;
	PHYSICAL_ADDRESS dmaBufferPhys;

	PDEVICE_OBJECT pDeviceObject;
	PDEVICE_OBJECT pNextLowerDriver;
	UNICODE_STRING SymLinkName;

	UINT32 HostFeatures;
} VBOXGUESTDEVEXTWIN;
typedef VBOXGUESTDEVEXTWIN *PVBOXGUESTDEVEXTWIN;

#define VERR_GENERAL_FAILURE (-1)
#define VMMDEV_MEMORY_VERSION (1)

#define VMMDEV_EVENT_MOUSE_POSITION_CHANGED (1 << 9)

typedef enum VMMDevRequestType
{
	VMMDevReq_InvalidRequest       =  0,
	VMMDevReq_SetMouseStatus       =  2,
	VMMDevReq_GetHostVersion       =  4,
	VMMDevReq_AcknowledgeEvents    = 41,
	VMMDevReq_CtlGuestFilterMask   = 42,
	VMMDevReq_ReportGuestInfo      = 50,
	VMMDevReq_SetGuestCapabilities = 56,
	VMMDevReq_ReportGuestInfo2     = 58,
	VMMDevReq_ReportGuestStatus    = 59,
	VMMDevReq_SizeHack             = 0x7fffffff
} VMMDevRequestType;

#define VMMDEV_REQUEST_HEADER_VERSION (0x10001)

#define VMMDEV_REQUESTOR_USR_DRV (0x00000001U)
#define VMMDEV_REQUESTOR_KERNEL  (0x00000000U)

typedef struct VMMDevRequestHeader
{
    UINT32 size;
    UINT32 version;
    VMMDevRequestType requestType;
    INT32  rc;
    UINT32 reserved1;
    UINT32 fRequestor;
} VMMDevRequestHeader;

typedef struct
{
	VMMDevRequestHeader header;
	UINT32 mouseFeatures;
	INT32 pointerXPos;
	INT32 pointerYPos;
} VMMDevReqMouseStatus;

typedef struct
{
    VMMDevRequestHeader header;
    short major;
    short minor;
    UINT32 build;
    UINT32 revision;
    UINT32 features;
} VMMDevReqHostVersion;

typedef struct
{
	VMMDevRequestHeader header;
	UINT32 u32OrMask;
	UINT32 u32NotMask;
} VMMDevCtlGuestFilterMask;

#define VMMDEV_VERSION (0x00010004)

typedef struct
{
    VMMDevRequestHeader header;
	UINT32 interfaceVersion;
	VBOXOSTYPE osType;
} VMMDevReportGuestInfo;

typedef struct
{
	VMMDevRequestHeader header;
	UINT32 u32OrMask;
	UINT32 u32NotMask; 
} VMMDevReqGuestCapabilities2;

#define VBOXGSTINFO2_F_REQUESTOR_INFO (0x00000001U)

typedef struct
{
    VMMDevRequestHeader header;
    short additionsMajor;
	short additionsMinor;
	UINT32 additionsBuild;
	UINT32 additionsRevision;
	UINT32 additionsFeatures;
	char szName[128];
} VMMDevReportGuestInfo2;

typedef enum
{
    VBoxGuestFacilityType_Unknown         = 0,
    VBoxGuestFacilityType_VBoxGuestDriver = 20,
	VBoxGuestFacilityType_All             = 0x7ffffffe,
	VBoxGuestFacilityType_SizeHack        = 0x7fffffff
} VBoxGuestFacilityType;

typedef enum
{
	VBoxGuestFacilityStatus_Inactive    = 0,
	VBoxGuestFacilityStatus_Active      = 50,
	VBoxGuestFacilityStatus_Unknown     = 999,
    VBoxGuestFacilityStatus_SizeHack    = 0x7fffffff
} VBoxGuestFacilityStatus;

typedef struct
{
	VBoxGuestFacilityType facility;
	VBoxGuestFacilityStatus status;
	UINT32 flags;
} VBoxGuestStatus;

typedef struct
{
	VMMDevRequestHeader header;
	VBoxGuestStatus guestStatus;
} VMMDevReportGuestStatus;

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath);
static NTSTATUS NTAPI vgdrvWinAddDevice(PDRIVER_OBJECT pDrvObj, PDEVICE_OBJECT pdo);
static void     NTAPI vgdrvWinUnload(PDRIVER_OBJECT pDrvObj);
static NTSTATUS NTAPI vgdrvWinCreate(PDEVICE_OBJECT pDevObj, PIRP pIrp);
static NTSTATUS NTAPI vgdrvWinPnP(PDEVICE_OBJECT pDevObj, PIRP pIrp);
static NTSTATUS NTAPI vgdrvWinPower(PDEVICE_OBJECT pDevObj, PIRP pIrp);
static NTSTATUS       vgdrvWinPnPSendIrpSynchronously(PDEVICE_OBJECT pDevObj, PIRP pIrp);
static NTSTATUS       vgdrvWinPnpIrpComplete(PDEVICE_OBJECT pDevObj, PIRP pIrp, PKEVENT pEvent);
static NTSTATUS       vgdrvWinSetupDevice(PVBOXGUESTDEVEXTWIN pDevExt, PDEVICE_OBJECT pDevObj, PIRP pIrp);
       int            VGDrvCommonInitDevExtResources(PVBOXGUESTDEVEXTWIN pDevExt, UINT32 fFixedEvents);
       void           VGDrvCommonDeleteDevExtResources(PVBOXGUESTDEVEXTWIN pDevExt);
static void     NTAPI vgdrvWinDpcHandler(PKDPC pDPC, PDEVICE_OBJECT pDevObj, PIRP pIrp, PVOID pContext);
static NTSTATUS       vgdrvWinScanPCIResourceList(PVBOXGUESTDEVEXTWIN pDevExt, PCM_RESOURCE_LIST pResList);
static NTSTATUS       vgdrvWinMapVMMDevMemory(PVBOXGUESTDEVEXTWIN pDevExt, PHYSICAL_ADDRESS PhysAddr,
											  ULONG cbToMap, void **ppvMMIOBase, UINT32 *pcbMMIO);
static void           vgdrvWinUnmapVMMDevMemory(PVBOXGUESTDEVEXTWIN pDevExt);
