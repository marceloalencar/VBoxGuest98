/** @file
 * VMMDev - Guest <-> VirtualBox host protocol definitions.
 *
 * Subset of include/VBox/VMMDev.h, include/VBox/VMMDevCoreTypes.h,
 * include/VBox/ostypes.h, include/VBox/HostServices/GuestPropertySvc.h,
 * include/VBox/HostServices/VBoxClipboardSvc.h,
 * include/VBox/GuestHost/SharedClipboard.h, include/VBox/err.h and
 * include/iprt/err.h from VirtualBox 7.2.22 (r175245), rewritten in C89 so it
 * builds with Visual C++ 6.0 and the Windows 2000 DDK.  Values and structure
 * sizes were checked against those headers; the sizes are also enforced at
 * compile time at the end of this file.
 *
 * This header has no kernel dependencies and is shared by the driver
 * (VBoxGst) and user mode tools (VBGTest).  Include <wdm.h> or <windows.h>
 * before it.
 */

#ifndef VBOXGUEST98_VMMDEV_H
#define VBOXGUEST98_VMMDEV_H

#pragma pack(push, 4)

/*********************************************************************************************************************************
*   VirtualBox status codes (subset of iprt/err.h)                                                                              *
*********************************************************************************************************************************/

#define VINF_SUCCESS                    (0)
#define VERR_GENERAL_FAILURE            (-1)
#define VERR_INVALID_PARAMETER          (-2)
#define VERR_INVALID_HANDLE             (-4)
#define VERR_INVALID_POINTER            (-6)
#define VERR_NO_MEMORY                  (-8)
#define VERR_PERMISSION_DENIED          (-10)
#define VERR_VERSION_MISMATCH           (-11)
#define VERR_NOT_IMPLEMENTED            (-12)
#define VERR_WRONG_ORDER                (-22)
#define VERR_NOT_SUPPORTED              (-37)
#define VERR_INTERRUPTED                (-39)
#define VERR_TIMEOUT                    (-40)
#define VERR_BUFFER_OVERFLOW            (-41)
#define VERR_TOO_MUCH_DATA              (-42)
#define VERR_OUT_OF_RANGE               (-54)
#define VERR_CANCELLED                  (-70)
#define VERR_NOT_FOUND                  (-78)
#define VERR_OPEN_FAILED                (-101)
#define VERR_PATH_NOT_FOUND             (-103)
#define VERR_INVALID_NAME               (-104)
#define VERR_READ_ERROR                 (-111)
#define VERR_WRITE_ERROR                (-112)
#define VERR_FILENAME_TOO_LONG          (-120)
#define VERR_NO_DATA                    (-304)
#define VERR_INTERNAL_ERROR             (-225)
#define VINF_BUFFER_OVERFLOW            (41)
#define VINF_HGCM_ASYNC_EXECUTE         (2903)

#define RT_SUCCESS(rc)                  ((int)(rc) >= VINF_SUCCESS)
#define RT_FAILURE(rc)                  (!RT_SUCCESS(rc))


/*********************************************************************************************************************************
*   Guest OS type                                                                                                                *
*********************************************************************************************************************************/

typedef enum VBOXOSTYPE
{
    VBOXOSTYPE_UNKNOWN    = 0,
    VBOXOSTYPE_DOS        = 0x10000,
    VBOXOSTYPE_WIN31      = 0x15000,
    VBOXOSTYPE_WIN95      = 0x21000,
    VBOXOSTYPE_WIN98      = 0x22000,
    VBOXOSTYPE_WINME      = 0x23000,
    VBOXOSTYPE_32BIT_HACK = 0x7fffffff
} VBOXOSTYPE;


/*********************************************************************************************************************************
*   VMMDev PCI device                                                                                                            *
*********************************************************************************************************************************/

/** PCI vendor and device ID of the VMMDev. */
#define VMMDEV_VENDORID                 0x80ee
#define VMMDEV_DEVICEID                 0xcafe

/** Port for generic request interface (relative offset). */
#define VMMDEV_PORT_OFF_REQUEST         0
/** Port for fast IRQ acknowledging: reading it acknowledges and returns the
 *  pending events, UINT32_MAX on failure.  Only if the host reports
 *  VMMDEV_HVF_FAST_IRQ_ACK. */
#define VMMDEV_PORT_OFF_REQUEST_FAST    8

/** Backdoor logging port (absolute, iprt/log.h): bytes written to it appear
 *  in VBox.log as "VMMDev: Guest Log:" lines.  The host buffers them up to
 *  a newline.  Disabled when the VM has VMMDev "BackdoorLogDisabled" set. */
#define RTLOG_DEBUG_PORT                0x504

/** Version of the shared memory (VMMDevMemory) structure. */
#define VMMDEV_MEMORY_VERSION           (1)
/** Version of the guest <-> host interface (VMMDevReportGuestInfo). */
#define VMMDEV_VERSION                  (0x00010004)
/** Version of VMMDevRequestHeader. */
#define VMMDEV_REQUEST_HEADER_VERSION   (0x10001)

/** The VMMDev shared memory, mapped through the PCI MMIO BAR. */
typedef struct VMMDevMemory
{
    /** The size of this structure. */
    UINT32 u32Size;
    /** The structure version (VMMDEV_MEMORY_VERSION). */
    UINT32 u32Version;
    union
    {
        struct
        {
            /** Flag telling that VMMDev has events pending (i.e. IRQ is ours). */
            BOOLEAN fHaveEvents;
        } V1_04;
        struct
        {
            UINT32 u32HostEvents;
            UINT32 u32GuestEventMask;
        } V1_03;
    } V;
} VMMDevMemory;


/*********************************************************************************************************************************
*   Events                                                                                                                       *
*********************************************************************************************************************************/

#define VMMDEV_EVENT_MOUSE_CAPABILITIES_CHANGED             (1U << 0)
#define VMMDEV_EVENT_HGCM                                   (1U << 1)
#define VMMDEV_EVENT_DISPLAY_CHANGE_REQUEST                 (1U << 2)
#define VMMDEV_EVENT_JUDGE_CREDENTIALS                      (1U << 3)
#define VMMDEV_EVENT_RESTORED                               (1U << 4)
#define VMMDEV_EVENT_SEAMLESS_MODE_CHANGE_REQUEST           (1U << 5)
#define VMMDEV_EVENT_BALLOON_CHANGE_REQUEST                 (1U << 6)
#define VMMDEV_EVENT_STATISTICS_INTERVAL_CHANGE_REQUEST     (1U << 7)
#define VMMDEV_EVENT_VRDP                                   (1U << 8)
#define VMMDEV_EVENT_MOUSE_POSITION_CHANGED                 (1U << 9)
#define VMMDEV_EVENT_CPU_HOTPLUG                            (1U << 10)
#define VMMDEV_EVENT_VALID_EVENT_MASK                       (0x000007ffU)


/*********************************************************************************************************************************
*   Requests                                                                                                                     *
*********************************************************************************************************************************/

typedef enum VMMDevRequestType
{
    VMMDevReq_InvalidRequest            =   0,
    VMMDevReq_GetMouseStatus            =   1,
    VMMDevReq_SetMouseStatus            =   2,
    VMMDevReq_SetPointerShape           =   3,
    VMMDevReq_GetHostVersion            =   4,
    VMMDevReq_Idle                      =   5,
    VMMDevReq_GetHostTime               =  10,
    VMMDevReq_GetHypervisorInfo         =  20,
    VMMDevReq_SetHypervisorInfo         =  21,
    VMMDevReq_RegisterPatchMemory       =  22,
    VMMDevReq_DeregisterPatchMemory     =  23,
    VMMDevReq_SetPowerStatus            =  30,
    VMMDevReq_AcknowledgeEvents         =  41,
    VMMDevReq_CtlGuestFilterMask        =  42,
    VMMDevReq_ReportGuestInfo           =  50,
    VMMDevReq_GetDisplayChangeRequest   =  51,
    VMMDevReq_VideoModeSupported        =  52,
    VMMDevReq_GetHeightReduction        =  53,
    VMMDevReq_GetDisplayChangeRequest2  =  54,
    VMMDevReq_ReportGuestCapabilities   =  55,
    VMMDevReq_SetGuestCapabilities      =  56,
    VMMDevReq_VideoModeSupported2       =  57,
    VMMDevReq_GetDisplayChangeRequestEx =  80,
    VMMDevReq_GetDisplayChangeRequestMulti = 81,
    VMMDevReq_GetDisplayChangeRequestMulti2 = 82,
    VMMDevReq_ReportGuestInfo2          =  58,
    VMMDevReq_ReportGuestStatus         =  59,
    VMMDevReq_HGCMConnect               =  60,
    VMMDevReq_HGCMDisconnect            =  61,
    VMMDevReq_HGCMCall32                =  62,
    VMMDevReq_HGCMCall64                =  63,
    VMMDevReq_HGCMCancel                =  64,
    VMMDevReq_HGCMCancel2               =  65,
    VMMDevReq_VideoAccelEnable          =  70,
    VMMDevReq_VideoAccelFlush           =  71,
    VMMDevReq_VideoSetVisibleRegion     =  72,
    VMMDevReq_GetSeamlessChangeRequest  =  73,
    VMMDevReq_ReportGuestUserState      =  74,
    VMMDevReq_QueryCredentials          = 100,
    VMMDevReq_ReportCredentialsJudgement = 101,
    VMMDevReq_ReportGuestStats          = 110,
    VMMDevReq_GetMemBalloonChangeRequest = 111,
    VMMDevReq_GetStatisticsChangeRequest = 112,
    VMMDevReq_ChangeMemBalloon          = 113,
    VMMDevReq_GetVRDPChangeRequest      = 150,
    VMMDevReq_LogString                 = 200,
    VMMDevReq_GetCpuHotPlugRequest      = 210,
    VMMDevReq_SetCpuHotPlugStatus       = 211,
    VMMDevReq_RegisterSharedModule      = 212,
    VMMDevReq_UnregisterSharedModule    = 213,
    VMMDevReq_CheckSharedModules        = 214,
    VMMDevReq_GetPageSharingStatus      = 215,
    VMMDevReq_DebugIsPageShared         = 216,
    VMMDevReq_GetSessionId              = 217,
    VMMDevReq_WriteCoreDump             = 218,
    VMMDevReq_GuestHeartbeat            = 219,
    VMMDevReq_HeartbeatConfigure        = 220,
    VMMDevReq_NtBugCheck                = 221,
    VMMDevReq_VideoUpdateMonitorPositions = 222,
    VMMDevReq_GetMouseStatusEx          = 223,
    VMMDevReq_GetHostGraphicsCapability = 224,
    VMMDevReq_SizeHack                  = 0x7fffffff
} VMMDevRequestType;

/** @name VMMDevRequestHeader::fRequestor values.
 * Only used by the host when VBOXGSTINFO2_F_REQUESTOR_INFO was reported.
 * @{ */
/** User of the requestor (VMMDEV_REQUESTOR_USR_MASK). */
#define VMMDEV_REQUESTOR_USR_NOT_GIVEN              (0x00000000U)
#define VMMDEV_REQUESTOR_USR_DRV                    (0x00000001U)
#define VMMDEV_REQUESTOR_USR_DRV_OTHER              (0x00000002U)
#define VMMDEV_REQUESTOR_USR_ROOT                   (0x00000003U)
#define VMMDEV_REQUESTOR_USR_SYSTEM                 (0x00000004U)
#define VMMDEV_REQUESTOR_USR_RESERVED1              (0x00000005U)
#define VMMDEV_REQUESTOR_USR_USER                   (0x00000006U)
#define VMMDEV_REQUESTOR_USR_GUEST                  (0x00000007U)
#define VMMDEV_REQUESTOR_USR_MASK                   (0x00000007U)
/** Kernel or user mode requestor. */
#define VMMDEV_REQUESTOR_KERNEL                     (0x00000000U)
#define VMMDEV_REQUESTOR_USERMODE                   (0x00000008U)
/** Whether the requestor is on the physical console (VMMDEV_REQUESTOR_CON_MASK). */
#define VMMDEV_REQUESTOR_CON_DONT_KNOW              (0x00000000U)
#define VMMDEV_REQUESTOR_CON_NO                     (0x00000010U)
#define VMMDEV_REQUESTOR_CON_YES                    (0x00000020U)
#define VMMDEV_REQUESTOR_CON_USER                   (0x00000030U)
#define VMMDEV_REQUESTOR_CON_MASK                   (0x00000030U)
/** Group membership. */
#define VMMDEV_REQUESTOR_GRP_VBOX                   (0x00000080U)
#define VMMDEV_REQUESTOR_GRP_WHEEL                  (0x00000100U)
/** Trust level (VMMDEV_REQUESTOR_TRUST_MASK). */
#define VMMDEV_REQUESTOR_TRUST_NOT_GIVEN            (0x00000000U)
#define VMMDEV_REQUESTOR_TRUST_UNTRUSTED            (0x00001000U)
#define VMMDEV_REQUESTOR_TRUST_LOW                  (0x00002000U)
#define VMMDEV_REQUESTOR_TRUST_MEDIUM               (0x00003000U)
#define VMMDEV_REQUESTOR_TRUST_MEDIUM_PLUS          (0x00004000U)
#define VMMDEV_REQUESTOR_TRUST_HIGH                 (0x00005000U)
#define VMMDEV_REQUESTOR_TRUST_SYSTEM               (0x00006000U)
#define VMMDEV_REQUESTOR_TRUST_PROTECTED            (0x00007000U)
#define VMMDEV_REQUESTOR_TRUST_MASK                 (0x00007000U)
/** Requestor uses the less trusted user device node. */
#define VMMDEV_REQUESTOR_USER_DEVICE                (0x00008000U)
/** There is no user device node (true for Windows 98). */
#define VMMDEV_REQUESTOR_NO_USER_DEVICE             (0x00010000U)
/** @} */

/** Generic VMMDev request header. */
typedef struct VMMDevRequestHeader
{
    /** IN: Size of the structure in bytes (including body). */
    UINT32 size;
    /** IN: Version of the structure (VMMDEV_REQUEST_HEADER_VERSION). */
    UINT32 version;
    /** IN: Type of the request. */
    VMMDevRequestType requestType;
    /** OUT: VBox status code. */
    INT32  rc;
    /** Reserved field no.1. MBZ. */
    UINT32 reserved1;
    /** IN: Requestor information (VMMDEV_REQUESTOR_XXX). */
    UINT32 fRequestor;
} VMMDevRequestHeader;

/** @name Mouse capability bits (VMMDevReqMouseStatus::mouseFeatures).
 * @{ */
#define VMMDEV_MOUSE_GUEST_CAN_ABSOLUTE                 (1U << 0)
#define VMMDEV_MOUSE_HOST_WANTS_ABSOLUTE                (1U << 1)
#define VMMDEV_MOUSE_GUEST_NEEDS_HOST_CURSOR            (1U << 2)
#define VMMDEV_MOUSE_HOST_CANNOT_HWPOINTER              (1U << 3)
#define VMMDEV_MOUSE_NEW_PROTOCOL                       (1U << 4)
#define VMMDEV_MOUSE_HOST_RECHECKS_NEEDS_HOST_CURSOR    (1U << 5)
#define VMMDEV_MOUSE_HOST_HAS_ABS_DEV                   (1U << 6)
#define VMMDEV_MOUSE_GUEST_USES_FULL_STATE_PROTOCOL     (1U << 7)
#define VMMDEV_MOUSE_HOST_SUPPORTS_FULL_STATE_PROTOCOL  (1U << 8)
#define VMMDEV_MOUSE_MASK                               (0x000001ffU)
/** Guest changes the host is notified about. */
#define VMMDEV_MOUSE_NOTIFY_HOST_MASK \
    (VMMDEV_MOUSE_GUEST_CAN_ABSOLUTE | VMMDEV_MOUSE_GUEST_NEEDS_HOST_CURSOR)
/** The mask of the bits the guest is allowed to change. */
#define VMMDEV_MOUSE_GUEST_MASK \
    (VMMDEV_MOUSE_NOTIFY_HOST_MASK | VMMDEV_MOUSE_NEW_PROTOCOL | VMMDEV_MOUSE_GUEST_USES_FULL_STATE_PROTOCOL)
/** @} */

/** Absolute pointer range reported by the host (0..0xffff). */
#define VMMDEV_MOUSE_RANGE_MIN                          0
#define VMMDEV_MOUSE_RANGE_MAX                          0xffff

/** VMMDevReq_GetMouseStatus / VMMDevReq_SetMouseStatus. */
typedef struct
{
    VMMDevRequestHeader header;
    UINT32 mouseFeatures;
    INT32  pointerXPos;
    INT32  pointerYPos;
} VMMDevReqMouseStatus;

/** @name Host feature flags (VMMDevReqHostVersion::features).
 * @{ */
#define VMMDEV_HVF_HGCM_PHYS_PAGE_LIST                  (1U << 0)
#define VMMDEV_HVF_HGCM_EMBEDDED_BUFFERS                (1U << 1)
#define VMMDEV_HVF_HGCM_CONTIGUOUS_PAGE_LIST            (1U << 2)
#define VMMDEV_HVF_HGCM_NO_BOUNCE_PAGE_LIST             (1U << 3)
/** Events can be acknowledged by reading VMMDEV_PORT_OFF_REQUEST_FAST. */
#define VMMDEV_HVF_FAST_IRQ_ACK                         (1U << 31)
/** @} */

/** VMMDevReq_GetHostVersion. */
typedef struct
{
    VMMDevRequestHeader header;
    USHORT major;
    USHORT minor;
    UINT32 build;
    UINT32 revision;
    UINT32 features;
} VMMDevReqHostVersion;

/** VMMDevReq_GetHostTime. */
typedef struct
{
    VMMDevRequestHeader header;
    /** OUT: Milliseconds since the UNIX epoch. */
    UINT64 time;
} VMMDevReqHostTime;

/** VMMDevReq_AcknowledgeEvents. */
typedef struct
{
    VMMDevRequestHeader header;
    /** OUT: Pending events. */
    UINT32 events;
} VMMDevEvents;

/** VMMDevReq_CtlGuestFilterMask. */
typedef struct
{
    VMMDevRequestHeader header;
    UINT32 u32OrMask;
    UINT32 u32NotMask;
} VMMDevCtlGuestFilterMask;

/** VMMDevReq_ReportGuestInfo. */
typedef struct
{
    VMMDevRequestHeader header;
    UINT32 interfaceVersion;
    VBOXOSTYPE osType;
} VMMDevReportGuestInfo;

/** @name VMMDevReportGuestInfo2::additionsFeatures.
 * @{ */
#define VBOXGSTINFO2_F_REQUESTOR_INFO                   (0x00000001U)
/** @} */

/** VMMDevReq_ReportGuestInfo2. */
typedef struct
{
    VMMDevRequestHeader header;
    USHORT additionsMajor;
    USHORT additionsMinor;
    UINT32 additionsBuild;
    UINT32 additionsRevision;
    UINT32 additionsFeatures;
    char   szName[128];
} VMMDevReportGuestInfo2;

/** @name Guest capability bits (VMMDevReqGuestCapabilities2).
 * @{ */
#define VMMDEV_GUEST_SUPPORTS_SEAMLESS                  (1U << 0)
#define VMMDEV_GUEST_SUPPORTS_GUEST_HOST_WINDOW_MAPPING (1U << 1)
#define VMMDEV_GUEST_SUPPORTS_GRAPHICS                  (1U << 2)
#define VMMDEV_GUEST_CAPABILITIES_MASK                  (0x00000007U)
/** @} */

/** VMMDevReq_SetGuestCapabilities. */
typedef struct
{
    VMMDevRequestHeader header;
    UINT32 u32OrMask;
    UINT32 u32NotMask;
} VMMDevReqGuestCapabilities2;

/** Guest facility types. */
typedef enum
{
    VBoxGuestFacilityType_Unknown         = 0,
    VBoxGuestFacilityType_VBoxGuestDriver = 20,
    VBoxGuestFacilityType_AutoLogon       = 90,
    VBoxGuestFacilityType_VBoxService     = 100,
    VBoxGuestFacilityType_VBoxTrayClient  = 101,
    VBoxGuestFacilityType_Seamless        = 1000,
    VBoxGuestFacilityType_Graphics        = 1100,
    VBoxGuestFacilityType_MonitorAttach   = 1101,
    VBoxGuestFacilityType_All             = 0x7ffffffe,
    VBoxGuestFacilityType_SizeHack        = 0x7fffffff
} VBoxGuestFacilityType;

/** Guest facility states. */
typedef enum
{
    VBoxGuestFacilityStatus_Inactive    = 0,
    VBoxGuestFacilityStatus_Paused      = 1,
    VBoxGuestFacilityStatus_PreInit     = 20,
    VBoxGuestFacilityStatus_Init        = 30,
    VBoxGuestFacilityStatus_Active      = 50,
    VBoxGuestFacilityStatus_Terminating = 100,
    VBoxGuestFacilityStatus_Terminated  = 101,
    VBoxGuestFacilityStatus_Failed      = 800,
    VBoxGuestFacilityStatus_Unknown     = 999,
    VBoxGuestFacilityStatus_SizeHack    = 0x7fffffff
} VBoxGuestFacilityStatus;

typedef struct
{
    VBoxGuestFacilityType facility;
    VBoxGuestFacilityStatus status;
    UINT32 flags;
} VBoxGuestStatus;

/** VMMDevReq_ReportGuestStatus. */
typedef struct
{
    VMMDevRequestHeader header;
    VBoxGuestStatus guestStatus;
} VMMDevReportGuestStatus;

/** Guest power states. */
typedef enum
{
    VMMDevPowerState_Invalid   = 0,
    VMMDevPowerState_Pause     = 1,
    VMMDevPowerState_PowerOff  = 2,
    VMMDevPowerState_SaveState = 3,
    VMMDevPowerState_SizeHack  = 0x7fffffff
} VMMDevPowerState;

/** VMMDevReq_SetPowerStatus. */
typedef struct
{
    VMMDevRequestHeader header;
    VMMDevPowerState powerState;
} VMMDevPowerStateRequest;

/** VMMDevReq_GetDisplayChangeRequestEx: the resolution the host wants. */
typedef struct
{
    VMMDevRequestHeader header;
    /** OUT: Horizontal resolution, 0 = don't change. */
    UINT32 xres;
    /** OUT: Vertical resolution, 0 = don't change. */
    UINT32 yres;
    /** OUT: Bits per pixel, 0 = don't change. */
    UINT32 bpp;
    /** IN: VMMDEV_EVENT_DISPLAY_CHANGE_REQUEST when answering that event. */
    UINT32 eventAck;
    /** OUT: Display, 0 = primary. */
    UINT32 display;
    /** OUT: Origin of a secondary display. */
    UINT32 cxOrigin;
    UINT32 cyOrigin;
    /** OUT: Whether the origin changes. */
    BOOLEAN fChangeOrigin;
    /** OUT: Whether the display is enabled. */
    BOOLEAN fEnabled;
    UCHAR   abPadding[2];
} VMMDevDisplayChangeRequestEx;

/** Guest statistics (VMMDevReq_ReportGuestStats).  Memory sizes are in 4KB
 *  pages, loads in percent (0-100) over the last interval. */
typedef struct VBoxGuestStatistics
{
    /** Virtual CPU ID. */
    UINT32 u32CpuId;
    /** Valid fields (VBOX_GUEST_STAT_XXX). */
    UINT32 u32StatCaps;
    UINT32 u32CpuLoad_Idle;
    UINT32 u32CpuLoad_Kernel;
    UINT32 u32CpuLoad_User;
    UINT32 u32Threads;
    UINT32 u32Processes;
    UINT32 u32Handles;
    UINT32 u32MemoryLoad;
    UINT32 u32PageSize;
    UINT32 u32PhysMemTotal;
    UINT32 u32PhysMemAvail;
    UINT32 u32PhysMemBalloon;
    UINT32 u32MemCommitTotal;
    UINT32 u32MemKernelTotal;
    UINT32 u32MemKernelPaged;
    UINT32 u32MemKernelNonPaged;
    UINT32 u32MemSystemCache;
    UINT32 u32PageFileSize;
} VBoxGuestStatistics;

/** @name VBoxGuestStatistics::u32StatCaps
 * @{ */
#define VBOX_GUEST_STAT_CPU_LOAD_IDLE       (1U << 0)
#define VBOX_GUEST_STAT_CPU_LOAD_KERNEL     (1U << 1)
#define VBOX_GUEST_STAT_CPU_LOAD_USER       (1U << 2)
#define VBOX_GUEST_STAT_THREADS             (1U << 3)
#define VBOX_GUEST_STAT_PROCESSES           (1U << 4)
#define VBOX_GUEST_STAT_HANDLES             (1U << 5)
#define VBOX_GUEST_STAT_MEMORY_LOAD         (1U << 6)
#define VBOX_GUEST_STAT_PHYS_MEM_TOTAL      (1U << 7)
#define VBOX_GUEST_STAT_PHYS_MEM_AVAIL      (1U << 8)
#define VBOX_GUEST_STAT_PHYS_MEM_BALLOON    (1U << 9)
#define VBOX_GUEST_STAT_MEM_COMMIT_TOTAL    (1U << 10)
#define VBOX_GUEST_STAT_MEM_KERNEL_TOTAL    (1U << 11)
#define VBOX_GUEST_STAT_MEM_KERNEL_PAGED    (1U << 12)
#define VBOX_GUEST_STAT_MEM_KERNEL_NONPAGED (1U << 13)
#define VBOX_GUEST_STAT_MEM_SYSTEM_CACHE    (1U << 14)
#define VBOX_GUEST_STAT_PAGE_FILE_SIZE      (1U << 15)
/** @} */

/** VMMDevReq_ReportGuestStats. */
typedef struct
{
    VMMDevRequestHeader header;
    VBoxGuestStatistics guestStats;
} VMMDevReportGuestStats;

/** VMMDevReq_GetStatisticsChangeRequest. */
typedef struct
{
    VMMDevRequestHeader header;
    /** OUT: Reporting interval in seconds, 0 = don't report. */
    UINT32 u32StatInterval;
    /** IN: VMMDEV_EVENT_STATISTICS_INTERVAL_CHANGE_REQUEST when answering that event. */
    UINT32 eventAck;
} VMMDevGetStatisticsChangeRequest;

/** VMMDevReq_HeartbeatConfigure.  Must be exactly 24 + 12 bytes, the host
 *  refuses any other size with VERR_INVALID_PARAMETER. */
typedef struct
{
    VMMDevRequestHeader header;
    /** OUT: Heartbeat interval in nanoseconds. */
    UINT64  cNsInterval;
    /** IN: Whether the guest heartbeat is enabled. */
    BOOLEAN fEnabled;
    UCHAR   abPadding[3];
} VMMDevReqHeartbeat;

/** VMMDevReq_LogString. */
typedef struct
{
    VMMDevRequestHeader header;
    /** Variable length, zero terminated string. */
    char szString[1];
} VMMDevReqLogString;


/*********************************************************************************************************************************
*   HGCM                                                                                                                         *
*********************************************************************************************************************************/

/** @name VMMDevHGCMRequestHeader::fu32Flags.
 * @{ */
#define VBOX_HGCM_REQ_DONE                              (1U << 0)
#define VBOX_HGCM_REQ_CANCELLED                         (1U << 1)
/** @} */

/** Header common to all HGCM requests. */
typedef struct VMMDevHGCMRequestHeader
{
    VMMDevRequestHeader header;
    /** OUT: HGCM flags (VBOX_HGCM_REQ_XXX), updated by the host asynchronously. */
    UINT32 fu32Flags;
    /** OUT: Result code of the HGCM operation. */
    INT32  result;
} VMMDevHGCMRequestHeader;

typedef enum
{
    VMMDevHGCMLoc_Invalid            = 0,
    VMMDevHGCMLoc_LocalHost          = 1,
    VMMDevHGCMLoc_LocalHost_Existing = 2,
    VMMDevHGCMLoc_SizeHack           = 0x7fffffff
} HGCMServiceLocationType;

typedef struct
{
    char achName[128];
} HGCMServiceLocationHost;

typedef struct HGCMServiceLocation
{
    HGCMServiceLocationType type;
    union
    {
        HGCMServiceLocationHost host;
    } u;
} HGCMServiceLocation;

/** VMMDevReq_HGCMConnect. */
typedef struct
{
    VMMDevHGCMRequestHeader header;
    HGCMServiceLocation loc;
    /** OUT: Client identifier. */
    UINT32 u32ClientID;
} VMMDevHGCMConnect;

/** VMMDevReq_HGCMDisconnect. */
typedef struct
{
    VMMDevHGCMRequestHeader header;
    UINT32 u32ClientID;
} VMMDevHGCMDisconnect;

typedef enum
{
    VMMDevHGCMParmType_Invalid            = 0,
    VMMDevHGCMParmType_32bit              = 1,
    VMMDevHGCMParmType_64bit              = 2,
    VMMDevHGCMParmType_PhysAddr           = 3,
    VMMDevHGCMParmType_LinAddr            = 4,
    VMMDevHGCMParmType_LinAddr_In         = 5,
    VMMDevHGCMParmType_LinAddr_Out        = 6,
    VMMDevHGCMParmType_LinAddr_Locked     = 7,
    VMMDevHGCMParmType_LinAddr_Locked_In  = 8,
    VMMDevHGCMParmType_LinAddr_Locked_Out = 9,
    VMMDevHGCMParmType_PageList           = 10,
    VMMDevHGCMParmType_Embedded           = 11,
    VMMDevHGCMParmType_ContiguousPageList = 12,
    VMMDevHGCMParmType_SizeHack           = 0x7fffffff
} HGCMFunctionParameterType;

/** HGCM call parameter as seen by a 32-bit guest (12 bytes). */
typedef struct
{
    HGCMFunctionParameterType type;
    union
    {
        UINT32 value32;
        UINT64 value64;
        struct
        {
            UINT32 size;
            union
            {
                UINT32 physAddr;
                UINT32 linearAddr;
            } u;
        } Pointer;
    } u;
} HGCMFunctionParameter32;

/** VMMDevReq_HGCMCall32, followed by cParms HGCMFunctionParameter32 entries. */
typedef struct
{
    VMMDevHGCMRequestHeader header;
    UINT32 u32ClientID;
    UINT32 u32Function;
    UINT32 cParms;
} VMMDevHGCMCall;

/** VMMDevReq_HGCMCancel2.  The address grew to 64 bits in VirtualBox 7.1;
 *  older hosts check for a minimum size and ignore the upper half, so this
 *  form works with all hosts (upstream's VMMDevHGCMCancel2Old was 24+4). */
typedef struct
{
    VMMDevRequestHeader header;
    /** Physical address of the request to cancel. */
    UINT64 physReqToCancel;
} VMMDevHGCMCancel2;

/** Upper limit on the parameter count we accept for an HGCM call. */
#define VMMDEV_MAX_HGCM_PARMS                           32


/*********************************************************************************************************************************
*   Guest property HGCM service (used by VBGTest)                                                                                *
*********************************************************************************************************************************/

#define GUEST_PROP_SERVICE_NAME                         "VBoxGuestPropSvc"
#define GUEST_PROP_FN_GET_PROP                          1
#define GUEST_PROP_FN_GET_PROP_CPARMS                   4


/*********************************************************************************************************************************
*   Shared clipboard HGCM service, legacy protocol (used by VBoxTray)                                                            *
*********************************************************************************************************************************/

#define VBOX_SHCL_SERVICE_NAME                          "VBoxSharedClipboard"

/** @name Host messages (returned by VBOX_SHCL_GUEST_FN_MSG_OLD_GET_WAIT).
 * @{ */
/** The guest must disconnect and stop. */
#define VBOX_SHCL_HOST_MSG_QUIT                         1
/** The host wants the guest clipboard data in the given format. */
#define VBOX_SHCL_HOST_MSG_READ_DATA                    2
/** The host clipboard changed, the formats it now has are given. */
#define VBOX_SHCL_HOST_MSG_FORMATS_REPORT               3
/** @} */

/** @name Guest functions.
 * @{ */
/** Waits for a host message.  Parms: [0] message (32bit out), [1] formats (32bit out). */
#define VBOX_SHCL_GUEST_FN_MSG_OLD_GET_WAIT             1
/** Reports the guest clipboard formats.  Parms: [0] formats (32bit in). */
#define VBOX_SHCL_GUEST_FN_REPORT_FORMATS               2
/** Reads host clipboard data.  Parms: [0] format (32bit in), [1] data (buffer out),
 *  [2] size of the data (32bit out).  VINF_BUFFER_OVERFLOW if the buffer is too small. */
#define VBOX_SHCL_GUEST_FN_DATA_READ                    3
/** Sends guest clipboard data (answer to READ_DATA).  Parms: [0] format (32bit in),
 *  [1] data (buffer in). */
#define VBOX_SHCL_GUEST_FN_DATA_WRITE                   4
/** @} */

/** @name Clipboard formats (bit mask).
 * @{ */
#define VBOX_SHCL_FMT_NONE                              0
/** UTF-16LE text, zero terminated, CRLF line endings. */
#define VBOX_SHCL_FMT_UNICODETEXT                       (1U << 0)
#define VBOX_SHCL_FMT_BITMAP                            (1U << 1)
#define VBOX_SHCL_FMT_HTML                              (1U << 2)
#define VBOX_SHCL_FMT_URI_LIST                          (1U << 3)
#define VBOX_SHCL_FMT_VALID_MASK                        (0x0000000fU)
/** @} */


/*********************************************************************************************************************************
*   Drag and drop HGCM service, protocol v3 (used by VBoxTray)                                                                   *
*********************************************************************************************************************************/

#define VBOX_DND_SERVICE_NAME                           "VBoxDragAndDropSvc"

/** @name Host messages (returned by GUEST_DND_FN_GET_NEXT_HOST_MSG).  The
 *  guest fetches a message's parameters by calling the message number as
 *  the function.
 * @{ */
#define HOST_DND_FN_SET_MODE                            100
#define HOST_DND_FN_HG_EVT_ENTER                        200
#define HOST_DND_FN_HG_EVT_MOVE                         201
#define HOST_DND_FN_HG_EVT_LEAVE                        202
#define HOST_DND_FN_HG_EVT_DROPPED                      203
#define HOST_DND_FN_CANCEL                              204
#define HOST_DND_FN_HG_SND_DATA                         205
#define HOST_DND_FN_HG_SND_MORE_DATA                    206
#define HOST_DND_FN_HG_SND_DIR                          207
#define HOST_DND_FN_HG_SND_FILE_DATA                    208
#define HOST_DND_FN_HG_SND_FILE_HDR                     209
#define HOST_DND_FN_HG_SND_DATA_HDR                     210
#define HOST_DND_FN_GH_REQ_PENDING                      600
#define HOST_DND_FN_GH_EVT_DROPPED                      601
/** @} */

/** @name Guest functions.
 * @{ */
#define GUEST_DND_FN_CONNECT                            10
#define GUEST_DND_FN_DISCONNECT                         11
#define GUEST_DND_FN_REPORT_FEATURES                    12
#define GUEST_DND_FN_QUERY_FEATURES                     13
#define GUEST_DND_FN_GET_NEXT_HOST_MSG                  300
#define GUEST_DND_FN_HG_ACK_OP                          400
#define GUEST_DND_FN_HG_REQ_DATA                        401
#define GUEST_DND_FN_HG_EVT_PROGRESS                    402
#define GUEST_DND_FN_GH_ACK_PENDING                     500
#define GUEST_DND_FN_GH_SND_DATA                        501
#define GUEST_DND_FN_EVT_ERROR                          502
#define GUEST_DND_FN_GH_SND_DATA_HDR                    503
#define GUEST_DND_FN_GH_SND_DIR                         700
#define GUEST_DND_FN_GH_SND_FILE_DATA                   701
#define GUEST_DND_FN_GH_SND_FILE_HDR                    702
/** @} */

/** @name Object modes sent with GUEST_DND_FN_GH_SND_DIR/FILE_HDR (RTFMODE:
 *  type and Unix permissions; the host refuses files with mode 0).
 * @{ */
#define VBOX_DND_MODE_DIR                               0x41ed  /* RTFS_TYPE_DIRECTORY | 0755 */
#define VBOX_DND_MODE_FILE                              0x81a4  /* RTFS_TYPE_FILE | 0644 */
/** @} */

/** Protocol version sent with GUEST_DND_FN_CONNECT (deprecated since 6.1). */
#define VBOX_DND_PROTOCOL_VERSION                       3
/** Guest features: none.  The second GUEST_DND_FN_REPORT_FEATURES
 *  parameter must have VBOX_DND_GF_1_MUST_BE_ONE set. */
#define VBOX_DND_GF_NONE                                0
#define VBOX_DND_GF_1_MUST_BE_ONE                       0x8000000000000000ui64
/** Chunk size used for transfers (DND_DEFAULT_CHUNK_SIZE). */
#define VBOX_DND_CHUNK_SIZE                             (64 * 1024)
/** Separator of the format list in HOST_DND_FN_HG_EVT_ENTER. */
#define VBOX_DND_FORMATS_SEPARATOR                      "\r\n"

/** @name Actions (VBOX_DND_ACTION_XXX).
 * @{ */
#define VBOX_DND_ACTION_IGNORE                          0
#define VBOX_DND_ACTION_COPY                            (1U << 0)
#define VBOX_DND_ACTION_MOVE                            (1U << 1)
#define VBOX_DND_ACTION_LINK                            (1U << 2)
/** @} */

/** @name Progress status (GUEST_DND_FN_HG_EVT_PROGRESS).
 * @{ */
#define DND_PROGRESS_RUNNING                            1
#define DND_PROGRESS_COMPLETE                           2
#define DND_PROGRESS_CANCELLED                          3
#define DND_PROGRESS_ERROR                              4
/** @} */

#pragma pack(pop)

/** Compile time size check (C89: a negative array size fails to compile). */
#define VMMDEV_ASSERT_SIZE(type, cb)    typedef char type##_SizeCheck[sizeof(type) == (cb) ? 1 : -1]

/* Sizes from the AssertCompileSize statements in VirtualBox 7.2.22
   include/VBox/VMMDev.h; the host checks most of them. */
VMMDEV_ASSERT_SIZE(VMMDevRequestHeader, 24);
VMMDEV_ASSERT_SIZE(VMMDevReqMouseStatus, 24+12);
VMMDEV_ASSERT_SIZE(VMMDevReqHostVersion, 24+16);
VMMDEV_ASSERT_SIZE(VMMDevReqHostTime, 24+8);
VMMDEV_ASSERT_SIZE(VMMDevEvents, 24+4);
VMMDEV_ASSERT_SIZE(VMMDevCtlGuestFilterMask, 24+8);
VMMDEV_ASSERT_SIZE(VMMDevReportGuestInfo, 24+8);
VMMDEV_ASSERT_SIZE(VMMDevReportGuestInfo2, 24+144);
VMMDEV_ASSERT_SIZE(VMMDevReqGuestCapabilities2, 24+8);
VMMDEV_ASSERT_SIZE(VMMDevReportGuestStatus, 24+12);
VMMDEV_ASSERT_SIZE(VMMDevPowerStateRequest, 24+4);
VMMDEV_ASSERT_SIZE(VMMDevReqHeartbeat, 24+12);
VMMDEV_ASSERT_SIZE(VMMDevDisplayChangeRequestEx, 24+32);
VMMDEV_ASSERT_SIZE(VBoxGuestStatistics, 19*4);
VMMDEV_ASSERT_SIZE(VMMDevReportGuestStats, 24+19*4);
VMMDEV_ASSERT_SIZE(VMMDevGetStatisticsChangeRequest, 24+8);
VMMDEV_ASSERT_SIZE(VMMDevHGCMRequestHeader, 24+8);
VMMDEV_ASSERT_SIZE(VMMDevHGCMConnect, 32+132+4);
VMMDEV_ASSERT_SIZE(VMMDevHGCMDisconnect, 32+4);
VMMDEV_ASSERT_SIZE(VMMDevHGCMCall, 32+12);
VMMDEV_ASSERT_SIZE(HGCMFunctionParameter32, 4+8);
VMMDEV_ASSERT_SIZE(VMMDevHGCMCancel2, 24+8);

#endif /* !VBOXGUEST98_VMMDEV_H */
