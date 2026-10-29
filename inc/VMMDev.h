/** @file
 * VMMDev - Guest <-> VirtualBox host protocol definitions.
 *
 * Subset of include/VBox/VMMDev.h, include/VBox/VMMDevCoreTypes.h,
 * include/VBox/ostypes.h, include/VBox/HostServices/GuestPropertySvc.h,
 * include/VBox/HostServices/VBoxClipboardSvc.h,
 * include/VBox/GuestHost/SharedClipboard.h, include/VBox/shflsvc.h,
 * include/VBox/err.h and include/iprt/err.h from VirtualBox 7.2.22 (r175245),
 * rewritten in C89 so it builds with Visual C++ 6.0 and the Windows 98 DDK.  Values and structure
 * sizes were checked against those headers; the sizes are also enforced at
 * compile time at the end of this file.
 *
 * This header has no kernel dependencies and is shared by the driver
 * (VBoxGst), the shared folders VxD (VBoxSF) and user mode programs
 * (VBoxTray).  Include <wdm.h> or <windows.h> before it (VBoxSF defines the
 * few types it needs itself).
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
#define VERR_ACCESS_DENIED              (-38)
#define VERR_INTERRUPTED                (-39)
#define VERR_TIMEOUT                    (-40)
#define VERR_BUFFER_OVERFLOW            (-41)
#define VERR_TOO_MUCH_DATA              (-42)
#define VERR_TRY_AGAIN                  (-52)
#define VERR_OUT_OF_RANGE               (-54)
#define VERR_CANCELLED                  (-70)
#define VERR_NOT_FOUND                  (-78)
#define VERR_FILE_IO_ERROR              (-100)
#define VERR_OPEN_FAILED                (-101)
#define VERR_FILE_NOT_FOUND             (-102)
#define VERR_ALREADY_EXISTS             (-105)
#define VERR_TOO_MANY_OPEN_FILES        (-106)
#define VERR_SEEK                       (-107)
#define VERR_NEGATIVE_SEEK              (-108)
#define VERR_PATH_NOT_FOUND             (-103)
#define VERR_INVALID_NAME               (-104)
#define VERR_READ_ERROR                 (-111)
#define VERR_WRITE_ERROR                (-112)
#define VERR_WRITE_PROTECT              (-113)
#define VERR_SHARING_VIOLATION          (-114)
#define VERR_FILE_LOCK_VIOLATION        (-116)
#define VERR_FILENAME_TOO_LONG          (-120)
#define VERR_DIR_NOT_EMPTY              (-125)
#define VERR_NOT_A_DIRECTORY            (-126)
#define VERR_IS_A_DIRECTORY             (-127)
#define VERR_DISK_FULL                  (-152)
#define VERR_NO_MORE_FILES              (-201)
#define VERR_NO_DATA                    (-304)
#define VERR_INTERNAL_ERROR             (-225)
#define VERR_VM_RESTORED                (-1025)
#define VERR_HGCM_SERVICE_NOT_FOUND     (-2900)
#define VINF_BUFFER_OVERFLOW            (41)
#define VINF_TRY_AGAIN                  (52)
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

/** VMMDevReq_GetSessionId. */
typedef struct
{
    VMMDevRequestHeader header;
    /** OUT: Unique session ID, different after each start, reset or restore of the VM. */
    UINT64 idSession;
} VMMDevReqSessionId;

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

/** Seamless modes (VMMDevSeamlessChangeRequest::mode). */
typedef enum VMMDevSeamlessMode
{
    /** Normal mode, the whole guest screen is shown. */
    VMMDev_Seamless_Disabled            = 0,
    /** Only the visible region reported by the guest is shown. */
    VMMDev_Seamless_Visible_Region      = 1,
    /** Each guest window in its own host window (not used by the Windows guests). */
    VMMDev_Seamless_Host_Window         = 2,
    VMMDev_Seamless_SizeHack            = 0x7fffffff
} VMMDevSeamlessMode;

/** Seamless mode the host wants (VMMDevReq_GetSeamlessChangeRequest). */
typedef struct VMMDevSeamlessChangeRequest
{
    VMMDevRequestHeader header;
    /** OUT: The mode (VMMDevSeamlessMode). */
    UINT32 mode;
    /** IN: VMMDEV_EVENT_SEAMLESS_MODE_CHANGE_REQUEST when answering that event. */
    UINT32 eventAck;
} VMMDevSeamlessChangeRequest;

/** Rectangle in guest screen pixels, right and bottom exclusive (RTRECT). */
typedef struct RTRECT
{
    INT32 xLeft;
    INT32 yTop;
    INT32 xRight;
    INT32 yBottom;
} RTRECT;

/** Visible region for seamless mode (VMMDevReq_VideoSetVisibleRegion).  The
 *  request is sized for cRect rectangles: header.size =
 *  VMMDEV_VISIBLE_REGION_SIZE(cRect). */
typedef struct VMMDevVideoSetVisibleRegion
{
    VMMDevRequestHeader header;
    /** IN: Number of rectangles. */
    UINT32 cRect;
    /** IN: The rectangles, cRect of them. */
    RTRECT Rect[1];
} VMMDevVideoSetVisibleRegion;

/** Size of a VMMDevVideoSetVisibleRegion request with cRect rectangles. */
#define VMMDEV_VISIBLE_REGION_SIZE(cRect)   (24 + 4 + (cRect) * 16)

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
*   Guest property HGCM service (used by VBoxTray)                                                                               *
*********************************************************************************************************************************/

#define GUEST_PROP_SERVICE_NAME                         "VBoxGuestPropSvc"
/** Maximum length of a property name, terminator included. */
#define GUEST_PROP_MAX_NAME_LEN                         64
/** Maximum length of a property value, terminator included. */
#define GUEST_PROP_MAX_VALUE_LEN                        1024

/** @name Guest functions.  Strings are UTF-8 and zero terminated.
 * @{ */
/** Reads a property.  Parms: [0] name (buffer in), [1] value, then the flags
 *  (buffer out), [2] timestamp (64bit out), [3] size needed (32bit out).
 *  VERR_BUFFER_OVERFLOW if the buffer is too small, VERR_NOT_FOUND if not set. */
#define GUEST_PROP_FN_GET_PROP                          1
#define GUEST_PROP_FN_GET_PROP_CPARMS                   4
/** Sets a property.  Parms: [0] name, [1] value, [2] flags (buffers in). */
#define GUEST_PROP_FN_SET_PROP                          2
#define GUEST_PROP_FN_SET_PROP_CPARMS                   3
/** Sets a property keeping its flags.  Parms: [0] name, [1] value (buffers in). */
#define GUEST_PROP_FN_SET_PROP_VALUE                    3
#define GUEST_PROP_FN_SET_PROP_VALUE_CPARMS             2
/** Deletes a property.  Parms: [0] name (buffer in). */
#define GUEST_PROP_FN_DEL_PROP                          4
#define GUEST_PROP_FN_DEL_PROP_CPARMS                   1
/** @} */


/*********************************************************************************************************************************
*   Shared clipboard HGCM service (used by VBoxTray)                                                                             *
*********************************************************************************************************************************/

#define VBOX_SHCL_SERVICE_NAME                          "VBoxSharedClipboard"

/** @name Host messages (VBOX_SHCL_GUEST_FN_MSG_OLD_GET_WAIT, or MSG_PEEK_WAIT +
 *  MSG_GET when both sides have VBOX_SHCL_GF_0_CONTEXT_ID).
 * @{ */
/** The guest must disconnect and stop. */
#define VBOX_SHCL_HOST_MSG_QUIT                         1
/** The host wants the guest clipboard data in the given format (legacy).
 *  MSG_GET parms: [0] message (32bit in) / 0, [1] format (32bit out). */
#define VBOX_SHCL_HOST_MSG_READ_DATA                    2
/** The host clipboard changed, the formats it now has are given.  MSG_GET
 *  parms: [0] message (32bit in) / context, [1] formats (32bit out). */
#define VBOX_SHCL_HOST_MSG_FORMATS_REPORT               3
/** A waiting MSG_PEEK_WAIT was cancelled (VINF_TRY_AGAIN). */
#define VBOX_SHCL_HOST_MSG_CANCELED                     4
/** The host wants the guest clipboard data, with a context ID for the
 *  answer.  MSG_GET parms: [0] message (64bit in) / context (64bit out),
 *  [1] format (32bit out). */
#define VBOX_SHCL_HOST_MSG_READ_DATA_CID                5
/** A file transfer status.  MSG_GET parms: [0] message (64bit in) / the
 *  transfer's context (64bit out), [1] SHCLTRANSFERDIR_XXX (host's point of
 *  view), [2] SHCLTRANSFERSTATUS_XXX, [3] status code, [4] flags. */
#define VBOX_SHCL_HOST_MSG_TRANSFER_STATUS              50
/** Requests of a transfer of guest files (the guest answers with the
 *  matching GUEST_FN_XXX_WRITE or GUEST_FN_REPLY).  MSG_GET parms: [0]
 *  message (64bit in) / context (64bit out), then like the guest function
 *  of the same name. */
#define VBOX_SHCL_HOST_MSG_TRANSFER_ROOT_LIST_HDR_READ  51
#define VBOX_SHCL_HOST_MSG_TRANSFER_ROOT_LIST_ENTRY_READ 53
#define VBOX_SHCL_HOST_MSG_TRANSFER_LIST_OPEN           55
#define VBOX_SHCL_HOST_MSG_TRANSFER_LIST_CLOSE          56
#define VBOX_SHCL_HOST_MSG_TRANSFER_LIST_HDR_READ       57
#define VBOX_SHCL_HOST_MSG_TRANSFER_LIST_ENTRY_READ     59
#define VBOX_SHCL_HOST_MSG_TRANSFER_OBJ_OPEN            61
#define VBOX_SHCL_HOST_MSG_TRANSFER_OBJ_CLOSE           62
/** Parms: [0] context, [1] handle (64bit), [2] size (32bit), [3] flags. */
#define VBOX_SHCL_HOST_MSG_TRANSFER_OBJ_READ            63
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
 *  [1] data (buffer in); with VBOX_SHCL_GF_0_CONTEXT_ID: [0] context (64bit
 *  in), [1] format (32bit in), [2] data (buffer in). */
#define VBOX_SHCL_GUEST_FN_DATA_WRITE                   4
/** Reports the guest features and gets the host's.  Parms: [0] features 0
 *  (64bit in/out), [1] features 1 (64bit in, VBOX_SHCL_GF_1_MUST_BE_ONE,
 *  comes back cleared). */
#define VBOX_SHCL_GUEST_FN_REPORT_FEATURES              6
/** Waits for a host message.  Parms: [0] VM session ID (64bit in, 0 = no
 *  check) / message (64bit out), [1] parameter count (32bit out). */
#define VBOX_SHCL_GUEST_FN_MSG_PEEK_WAIT                9
/** Fetches the message peeked at; parm [0] carries its ID on input. */
#define VBOX_SHCL_GUEST_FN_MSG_GET                      10
/** Answers the host; for a transfer status: [0] context (64bit), [1]
 *  VBOX_SHCL_TX_REPLYMSGTYPE_TRANSFER_STATUS, [2] status code, [3] payload
 *  (buffer in, empty), [4] SHCLTRANSFERSTATUS_XXX.  For the other reply
 *  types [4] is the list or object handle (64bit). */
#define VBOX_SHCL_GUEST_FN_REPLY                        11
/** Reads the transfer's root list header.  Parms: [0] context (64bit),
 *  [1] flags (32bit), [2] entry count (64bit out). */
#define VBOX_SHCL_GUEST_FN_ROOT_LIST_HDR_READ           12
/** Like ROOT_LIST_HDR_READ, giving the guest's (in). */
#define VBOX_SHCL_GUEST_FN_ROOT_LIST_HDR_WRITE          13
/** Reads a root list entry.  Parms: [0] context (64bit), [1] VBOX_SHCL_INFO_F_XXX,
 *  [2] index (64bit), [3] name (buffer out, UTF-8), [4] info size (32bit),
 *  [5] info (buffer out, SHCLFSOBJINFO). */
#define VBOX_SHCL_GUEST_FN_ROOT_LIST_ENTRY_READ         14
/** Like ROOT_LIST_ENTRY_READ, giving the guest's (in). */
#define VBOX_SHCL_GUEST_FN_ROOT_LIST_ENTRY_WRITE        15
/** Opens a directory list.  Parms: [0] context (64bit), [1] flags, [2]
 *  filter, [3] path (buffers in, the path relative to the transfer), [4]
 *  handle (64bit out). */
#define VBOX_SHCL_GUEST_FN_LIST_OPEN                    16
/** Closes a list.  Parms: [0] context (64bit), [1] handle (64bit). */
#define VBOX_SHCL_GUEST_FN_LIST_CLOSE                   17
/** Reads a list header.  Parms: [0] context (64bit), [1] handle (64bit),
 *  [2] flags, [3] features (32bit out), [4] entry count (64bit out),
 *  [5] total size (64bit out). */
#define VBOX_SHCL_GUEST_FN_LIST_HDR_READ                18
/** Like LIST_HDR_READ, giving the guest's (in). */
#define VBOX_SHCL_GUEST_FN_LIST_HDR_WRITE               19
/** Reads a list entry.  Parms: [0] context (64bit), [1] handle (64bit),
 *  [2] VBOX_SHCL_INFO_F_XXX, [3] name, [4] info size, [5] info. */
#define VBOX_SHCL_GUEST_FN_LIST_ENTRY_READ              20
/** Like LIST_ENTRY_READ, giving the guest's (in). */
#define VBOX_SHCL_GUEST_FN_LIST_ENTRY_WRITE             21
/** Opens a file.  Parms: [0] context (64bit), [1] handle (64bit out), [2]
 *  path (buffer in), [3] SHCL_OBJ_CF_XXX. */
#define VBOX_SHCL_GUEST_FN_OBJ_OPEN                     22
/** Closes a file.  Parms: [0] context (64bit), [1] handle (64bit). */
#define VBOX_SHCL_GUEST_FN_OBJ_CLOSE                    23
/** Reads from a file.  Parms: [0] context (64bit), [1] handle (64bit), [2]
 *  size (32bit in/out), [3] data (buffer out), [4] checksum size, [5]
 *  checksum (empty). */
#define VBOX_SHCL_GUEST_FN_OBJ_READ                     24
/** Like OBJ_READ, giving guest file data (in; the answer to OBJ_READ). */
#define VBOX_SHCL_GUEST_FN_OBJ_WRITE                    25
/** Cancels the caller's pending MSG_PEEK_WAIT.  No parms. */
#define VBOX_SHCL_GUEST_FN_MSG_CANCEL                   26
/** Agrees on the transfer chunk size.  Parms: [0] largest (32bit in/out),
 *  [1] preferred (32bit in/out, 0 = the host picks). */
#define VBOX_SHCL_GUEST_FN_NEGOTIATE_CHUNK_SIZE         28
/** @} */

/** @name File transfers.
 * @{ */
#define VBOX_SHCL_TX_REPLYMSGTYPE_TRANSFER_STATUS       1
#define VBOX_SHCL_TX_REPLYMSGTYPE_LIST_OPEN             2
#define VBOX_SHCL_TX_REPLYMSGTYPE_LIST_CLOSE            3
#define VBOX_SHCL_TX_REPLYMSGTYPE_OBJ_OPEN              4
#define VBOX_SHCL_TX_REPLYMSGTYPE_OBJ_CLOSE             5
#define VBOX_SHCL_INFO_F_FSOBJINFO                      (1U << 0)
#define SHCL_OBJ_CF_ACCESS_READ                         0x00001000U
#define SHCL_OBJ_CF_ACCESS_DENYWRITE                    0x00008000U
#define SHCLTRANSFERSTATUS_INITIALIZED                  1
#define SHCLTRANSFERSTATUS_UNINITIALIZED                2
#define SHCLTRANSFERSTATUS_STARTED                      3
#define SHCLTRANSFERSTATUS_COMPLETED                    4
#define SHCLTRANSFERSTATUS_CANCELED                     5
#define SHCLTRANSFERSTATUS_KILLED                       6
#define SHCLTRANSFERSTATUS_ERROR                        7
#define SHCLTRANSFERSTATUS_REQUESTED                    8
/** Host to guest is TO_REMOTE from the host's point of view. */
#define SHCLTRANSFERDIR_FROM_REMOTE                     1
#define SHCLTRANSFERDIR_TO_REMOTE                       2
/** Context IDs: session (16 bits), transfer (16 bits), event (32 bits). */
#define VBOX_SHCL_CONTEXTID_GET_TRANSFER(idContext)     ((UINT32)(((idContext) >> 32) & 0xffff))
/** @} */

/** @name Feature bits (VBOX_SHCL_GUEST_FN_REPORT_FEATURES), 64bit.
 * @{ */
/** Guest: uses MSG_PEEK_WAIT / MSG_GET and answers with context IDs. */
#define VBOX_SHCL_GF_0_CONTEXT_ID                       ((UINT64)1 << 0)
/** Guest: does file transfers (needs VBOX_SHCL_GF_0_CONTEXT_ID too). */
#define VBOX_SHCL_GF_0_TRANSFERS                        ((UINT64)1 << 1)
#define VBOX_SHCL_GF_1_MUST_BE_ONE                      ((UINT64)1 << 63)
/** Host: the same. */
#define VBOX_SHCL_HF_0_CONTEXT_ID                       ((UINT64)1 << 0)
#define VBOX_SHCL_HF_0_TRANSFERS                        ((UINT64)1 << 1)
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


/*********************************************************************************************************************************
*   Guest control HGCM service (used by VBoxTray)                                                                                *
*********************************************************************************************************************************/

#define VBOX_GUESTCTRL_SERVICE_NAME                     "VBoxGuestControlSvc"

/** @name Context IDs: session (5 bits), object (11 bits), count (16 bits).
 * @{ */
#define VBOX_GUESTCTRL_CONTEXTID_MAKE_SESSION(uSession) ((UINT32)((uSession) & 0x1f) << 27)
#define VBOX_GUESTCTRL_CONTEXTID_GET_SESSION(uContextID) (((uContextID) >> 27) & 0x1f)
/** @} */

/** @name Host messages (returned by GUEST_MSG_PEEK_WAIT, fetched with GUEST_MSG_GET).
 * @{ */
#define HOST_MSG_CANCEL_PENDING_WAITS                   0
/** Parms: [0] context, [1] protocol, [2] user, [3] password, [4] domain
 *  (buffers out), [5] flags. */
#define HOST_MSG_SESSION_CREATE                         20
/** Sent to the master and to the session.  Parms: [0] context, [1] flags. */
#define HOST_MSG_SESSION_CLOSE                          21
/** Parms (protocol 2): [0] context, [1] command, [2] flags, [3] argument
 *  count, [4] arguments (Bourne shell quoted), [5] variable count, [6] size
 *  of the environment, [7] environment, [8] time limit (ms), [9] priority,
 *  [10] affinity count, [11] affinity, [12] working directory (only with
 *  VBOX_GUESTCTRL_HF_0_PROCESS_CWD).  Buffers are out, strings UTF-8. */
#define HOST_MSG_EXEC_CMD                               100
#define HOST_MSG_EXEC_CMD_CPARMS                        12
/** Parms: [0] context, [1] PID, [2] flags (GUEST_PROC_IN_FLAG_XXX), [3] data
 *  (buffer out), [4] size of the data (VERR_BUFFER_OVERFLOW if bigger). */
#define HOST_MSG_EXEC_SET_INPUT                         101
/** Parms: [0] context, [1] PID, [2] handle (GUEST_PROC_OUT_H_XXX), [3] flags. */
#define HOST_MSG_EXEC_GET_OUTPUT                        102
/** Parms: [0] context, [1] PID. */
#define HOST_MSG_EXEC_TERMINATE                         110
/** Parms: [0] context, [1] PID, [2] flags, [3] timeout. */
#define HOST_MSG_EXEC_WAIT_FOR                          120
/** Parms: [0] context, [1] file name, [2] access ("r", "w", "r+", "a", ...),
 *  [3] disposition ("ca", "ce", "oc", "oa", "oe", "ot"), [4] sharing
 *  (buffers out), [5] creation mode, [6] offset (64bit).  The handle is
 *  the context's object ID. */
#define HOST_MSG_FILE_OPEN                              240
/** Parms: [0] context, [1] handle. */
#define HOST_MSG_FILE_CLOSE                             241
/** Parms: [0] context, [1] handle, [2] bytes to read. */
#define HOST_MSG_FILE_READ                              250
/** Parms: [0] context, [1] handle, [2] offset (64bit), [3] bytes to read. */
#define HOST_MSG_FILE_READ_AT                           251
/** Parms: [0] context, [1] handle, [2] size, [3] data (buffer out). */
#define HOST_MSG_FILE_WRITE                             260
/** Parms: [0] context, [1] handle, [2] offset (64bit), [3] size, [4] data (buffer out). */
#define HOST_MSG_FILE_WRITE_AT                          261
/** Parms: [0] context, [1] handle, [2] GUEST_FILE_SEEKTYPE_XXX, [3] offset (64bit). */
#define HOST_MSG_FILE_SEEK                              270
/** Parms: [0] context, [1] handle. */
#define HOST_MSG_FILE_TELL                              271
/** Parms: [0] context, [1] handle, [2] new size (64bit). */
#define HOST_MSG_FILE_SET_SIZE                          272
/** Parms: [0] context, [1] file name.  Answered with GUEST_MSG_REPLY. */
#define HOST_MSG_FILE_REMOVE                            273
/** Parms: [0] context, [1] path, [2] filter, [3] flags, [4] attributes to
 *  add, [5] read flags.  The handle is the context's object ID. */
#define HOST_MSG_DIR_OPEN                               310
/** Parms: [0] context, [1] handle. */
#define HOST_MSG_DIR_CLOSE                              311
/** Parms: [0] context, [1] handle. */
#define HOST_MSG_DIR_READ                               312
/** Parms: [0] context, [1] handle. */
#define HOST_MSG_DIR_REWIND                             313
/** Parms: [0] context, [1] path, [2] mode, [3] GSTCTL_CREATEDIRECTORY_F_XXX.
 *  Answered with GUEST_MSG_REPLY. */
#define HOST_MSG_DIR_CREATE                             314
/** Parms: [0] context, [1] handle, [2] entries wanted, [3] flags. */
#define HOST_MSG_DIR_LIST                               315
/** Parms: [0] context, [1] path, [2] DIRREMOVEREC_FLAG_XXX.  Answered with
 *  GUEST_MSG_REPLY. */
#define HOST_MSG_DIR_REMOVE                             320
/** Parms: [0] context, [1] source, [2] destination, [3] PATHRENAME_FLAG_XXX.
 *  Answered with GUEST_MSG_REPLY. */
#define HOST_MSG_PATH_RENAME                            330
/** Parms: [0] context.  Answered with GUEST_MSG_REPLY, the path as payload. */
#define HOST_MSG_PATH_USER_DOCUMENTS                    331
#define HOST_MSG_PATH_USER_HOME                         332
/** Parms: [0] context, [1] GUEST_SHUTDOWN_FLAG_XXX.  Answered with
 *  GUEST_MSG_REPLY before shutting down. */
#define HOST_MSG_SHUTDOWN                               333
/** Parms: [0] context, [1] path, [2] attributes to add, [3] GSTCTL_PATH_F_XXX. */
#define HOST_MSG_FS_OBJ_QUERY_INFO                      334
/** Parms: [0] context, [1] template, [2] directory, [3] GSTCTL_CREATETEMP_F_XXX,
 *  [4] mode. */
#define HOST_MSG_FS_CREATE_TEMP                         335
/** Parms: [0] context, [1] path. */
#define HOST_MSG_FS_QUERY_INFO                          336
/** Parms: [0] context, [1] flags.  Answered with GUEST_MSG_REPLY, the mount
 *  points as payload, each zero terminated. */
#define HOST_MSG_MOUNT_POINTS                           337
/** @} */

/** @name Guest functions.
 * @{ */
/** Cancels the caller's pending GUEST_MSG_PEEK_WAIT.  No parms. */
#define GUEST_MSG_CANCEL                                2
/** General answer.  Parms: [0] context, [1] type (0), [2] status, [3]
 *  payload (buffer in). */
#define GUEST_MSG_REPLY                                 11
/** Waits for a host message.  Parms: [0] VM session ID (64bit in, 0 = no
 *  restore check) / message (64bit out), [1] parameter count (32bit out).
 *  VERR_VM_RESTORED (new session ID in [0]) after a restore. */
#define GUEST_MSG_PEEK_WAIT                             7
/** Fetches the message peeked at; parm [0] is its ID on input. */
#define GUEST_MSG_GET                                   8
/** Drops a message.  Parms: [0] status for the host (32bit), [1] message
 *  ID (32bit, UINT32_MAX = the first one). */
#define GUEST_MSG_SKIP                                  9
/** Makes the caller the master (the root that handles sessions).  No parms. */
#define GUEST_MSG_MAKE_ME_MASTER                        13
/** Announces a session before its client accepts it (master only).  Parms:
 *  [0] session ID (32bit), [1] one time key (buffer in, 64 to 16384 bytes). */
#define GUEST_MSG_SESSION_PREPARE                       14
/** Binds the caller to a prepared session.  Parms as GUEST_MSG_SESSION_PREPARE. */
#define GUEST_MSG_SESSION_ACCEPT                        16
/** Session status.  Parms: [0] context, [1] GUEST_SESSION_NOTIFYTYPE_XXX,
 *  [2] result (32bit). */
#define GUEST_MSG_SESSION_NOTIFY                        20
/** Reports the guest features and gets the host's.  Parms: [0] features 0
 *  (64bit in/out), [1] features 1 (64bit in, VBOX_GUESTCTRL_GF_1_MUST_BE_ONE,
 *  comes back cleared). */
#define GUEST_MSG_REPORT_FEATURES                       22
/** Program output (answer to HOST_MSG_EXEC_GET_OUTPUT).  Parms: [0] context,
 *  [1] PID, [2] handle, [3] flags, [4] data (buffer in, may be empty). */
#define GUEST_MSG_EXEC_OUTPUT                           100
/** Process status.  Parms: [0] context, [1] PID, [2] PROC_STS_XXX, [3] flags
 *  (exit code or error status), [4] data (buffer in). */
#define GUEST_MSG_EXEC_STATUS                           101
/** Input status (answer to HOST_MSG_EXEC_SET_INPUT).  Parms: [0] context,
 *  [1] PID, [2] INPUT_STS_XXX, [3] flags (status code on error), [4] bytes
 *  written. */
#define GUEST_MSG_EXEC_INPUT_STATUS                     102
/** Directory answers.  Parms: [0] context, [1] GUEST_DIR_NOTIFYTYPE_XXX,
 *  [2] status, then by type: OPEN [3] handle; READ [3] GSTCTLDIRENTRYEX,
 *  [4] user, [5] groups (buffers in); LIST [3] entry count, [4] entries
 *  (GSTCTLDIRENTRYLISTHDR, entry, user, groups each). */
#define GUEST_MSG_DIR_NOTIFY                            230
/** File answers.  Parms: [0] context, [1] GUEST_FILE_NOTIFYTYPE_XXX, [2]
 *  status, then by type: OPEN [3] handle; READ_OFFSET [3] data, [4] new
 *  offset (64bit); WRITE_OFFSET [3] bytes written, [4] new offset; SEEK,
 *  TELL, SET_SIZE [3] offset or size (64bit). */
#define GUEST_MSG_FILE_NOTIFY                           240
/** File system answers.  Parms: [0] context, [1] GUEST_FS_NOTIFYTYPE_XXX,
 *  [2] status, then by type: CREATE_TEMP [3] path; QUERY_OBJ_INFO [3]
 *  GSTCTLFSOBJINFO, [4] user, [5] groups; QUERY_INFO [3] GSTCTLFSINFO. */
#define GUEST_MSG_FS_NOTIFY                             241
/** @} */

/** @name GUEST_MSG_SESSION_NOTIFY types.
 * @{ */
#define GUEST_SESSION_NOTIFYTYPE_ERROR                  1
#define GUEST_SESSION_NOTIFYTYPE_STARTED                11
/** Terminated normally. */
#define GUEST_SESSION_NOTIFYTYPE_TEN                    20
/** @} */

/** @name GUEST_MSG_EXEC_STATUS process states.
 * @{ */
#define PROC_STS_STARTED                                1
/** Terminated normally, flags = exit code. */
#define PROC_STS_TEN                                    2
/** Terminated by the time limit. */
#define PROC_STS_TOK                                    5
/** Hit the time limit but couldn't be terminated. */
#define PROC_STS_TOA                                    6
/** Ended by the host or because the guest service stops. */
#define PROC_STS_DWN                                    7
/** Couldn't be started, flags = VBox status code. */
#define PROC_STS_ERROR                                  8
/** @} */

/** @name GUEST_MSG_EXEC_INPUT_STATUS states.
 * @{ */
#define INPUT_STS_WRITTEN                               1
#define INPUT_STS_ERROR                                 20
/** The program has no input (any more). */
#define INPUT_STS_TERMINATED                            21
/** @} */

/** HOST_MSG_EXEC_SET_INPUT flag: the last input, close the program's input. */
#define GUEST_PROC_IN_FLAG_EOF                          (1U << 0)

/** @name Output handles (HOST_MSG_EXEC_GET_OUTPUT).
 * @{ */
#define GUEST_PROC_OUT_H_STDOUT                         1
#define GUEST_PROC_OUT_H_STDERR                         2
/** @} */

/** @name GUEST_MSG_DIR_NOTIFY types.
 * @{ */
#define GUEST_DIR_NOTIFYTYPE_OPEN                       10
#define GUEST_DIR_NOTIFYTYPE_CLOSE                      20
#define GUEST_DIR_NOTIFYTYPE_READ                       21
#define GUEST_DIR_NOTIFYTYPE_REWIND                     22
#define GUEST_DIR_NOTIFYTYPE_LIST                       23
/** @} */

/** @name GUEST_MSG_FILE_NOTIFY types.
 * @{ */
#define GUEST_FILE_NOTIFYTYPE_ERROR                     1
#define GUEST_FILE_NOTIFYTYPE_OPEN                      10
#define GUEST_FILE_NOTIFYTYPE_CLOSE                     20
#define GUEST_FILE_NOTIFYTYPE_READ                      30
#define GUEST_FILE_NOTIFYTYPE_READ_OFFSET               31
#define GUEST_FILE_NOTIFYTYPE_WRITE                     40
#define GUEST_FILE_NOTIFYTYPE_WRITE_OFFSET              41
#define GUEST_FILE_NOTIFYTYPE_SEEK                      50
#define GUEST_FILE_NOTIFYTYPE_TELL                      60
#define GUEST_FILE_NOTIFYTYPE_SET_SIZE                  61
/** @} */

/** @name GUEST_MSG_FS_NOTIFY types.
 * @{ */
#define GUEST_FS_NOTIFYTYPE_CREATE_TEMP                 1
#define GUEST_FS_NOTIFYTYPE_QUERY_OBJ_INFO              2
#define GUEST_FS_NOTIFYTYPE_QUERY_INFO                  3
/** @} */

/** @name HOST_MSG_FILE_SEEK methods.
 * @{ */
#define GUEST_FILE_SEEKTYPE_BEGIN                       1
#define GUEST_FILE_SEEKTYPE_CURRENT                     4
#define GUEST_FILE_SEEKTYPE_END                         8
/** @} */

/** @name File operation flags.
 * @{ */
#define GSTCTL_CREATEDIRECTORY_F_PARENTS                (1U << 0)
#define DIRREMOVEREC_FLAG_RECURSIVE                     (1U << 0)
#define DIRREMOVEREC_FLAG_CONTENT_AND_DIR               (1U << 1)
#define DIRREMOVEREC_FLAG_CONTENT_ONLY                  (1U << 2)
#define PATHRENAME_FLAG_REPLACE                         (1U << 0)
#define GSTCTL_CREATETEMP_F_DIRECTORY                   (1U << 0)
#define GUEST_SHUTDOWN_FLAG_POWER_OFF                   (1U << 0)
#define GUEST_SHUTDOWN_FLAG_REBOOT                      (1U << 1)
#define GUEST_SHUTDOWN_FLAG_FORCE                       (1U << 2)
/** @} */

/** @name GSTCTLFSOBJATTR::fMode bits (RTFS_XXX).
 * @{ */
#define RTFS_UNIX_ALL_ACCESS_PERMS                      0000777U
#define RTFS_TYPE_DIRECTORY                             0040000U
#define RTFS_TYPE_FILE                                  0100000U
/** The DOS attributes (FILE_ATTRIBUTE_XXX) are kept shifted by this. */
#define RTFS_DOS_SHIFT                                  16
/** @} */

/** @name GSTCTLFSOBJATTR::enmAdditional values.
 * @{ */
#define GSTCTLFSOBJATTRADD_NOTHING                      1
#define GSTCTLFSOBJATTRADD_UNIX                         2
/** @} */

/** File system object attributes (RTFSOBJATTR). */
typedef struct GSTCTLFSOBJATTR
{
    /** RTFS_TYPE_XXX, RTFS_UNIX_XXX and the DOS attributes. */
    UINT32 fMode;
    /** GSTCTLFSOBJATTRADD_XXX. */
    UINT32 enmAdditional;
    union
    {
        /** GSTCTLFSOBJATTRADD_UNIX. */
        struct
        {
            UINT32 uid;
            UINT32 gid;
            UINT32 cHardlinks;
            UINT32 INodeIdDevice;
            UINT64 INodeId;
            UINT32 fFlags;
            UINT32 GenerationId;
            UINT32 Device;
        } Unix;
        UCHAR  abReserveSpace[128];
    } u;
} GSTCTLFSOBJATTR;

/** File system object information (RTFSOBJINFO); times are nanoseconds
 *  since 1970-01-01 UTC (RTTIMESPEC). */
typedef struct GSTCTLFSOBJINFO
{
    INT64 cbObject;
    INT64 cbAllocated;
    INT64 AccessTime;
    INT64 ModificationTime;
    INT64 ChangeTime;
    INT64 BirthTime;
    GSTCTLFSOBJATTR Attr;
} GSTCTLFSOBJINFO;

#pragma pack(push, 1)
/** Directory entry (RTDIRENTRYEX); sent only up to the name's terminator. */
typedef struct GSTCTLDIRENTRYEX
{
    GSTCTLFSOBJINFO Info;
    /** Length of the 8.3 name in UTF-16 units. */
    USHORT cwcShortName;
    WCHAR  wszShortName[14];
    /** Length of the name (UTF-8), without the terminator. */
    USHORT cbName;
    char   szName[260];
} GSTCTLDIRENTRYEX;

/** Header of each entry of a HOST_MSG_DIR_LIST answer. */
typedef struct GSTCTLDIRENTRYLISTHDR
{
    UINT32 cbDirEntryEx;
    /** Sizes of the user and groups strings, terminators included. */
    UINT32 cbUser;
    UINT32 cbGroups;
} GSTCTLDIRENTRYLISTHDR;

/** File system information (HOST_MSG_FS_QUERY_INFO); sent with the mount
 *  point appended. */
typedef struct GSTCTLFSINFO
{
    UINT64 cbFree;
    UINT64 cbTotalSize;
    UINT32 cbBlockSize;
    UINT32 cbSectorSize;
    UINT32 uSerialNumber;
    UINT32 fFlags;
    UINT32 fFeatures;
    UINT32 cMaxComponent;
    char   szName[255];
    char   szLabel[255];
    USHORT cbMountpoint;
    char   szMountpoint[1];
} GSTCTLFSINFO;
#pragma pack(pop)

/** @name HOST_MSG_EXEC_CMD flags.
 * @{ */
#define GUEST_PROC_CREATE_FLAG_HIDDEN                   (1U << 2)
/** The host reads the standard output / error. */
#define GUEST_PROC_CREATE_FLAG_WAIT_STDOUT              (1U << 4)
#define GUEST_PROC_CREATE_FLAG_WAIT_STDERR              (1U << 5)
/** The arguments are passed as given, not Bourne shell quoted. */
#define GUEST_PROC_CREATE_FLAG_UNQUOTED_ARGS            (1U << 7)
/** @} */

/** @name Feature bits (GUEST_MSG_REPORT_FEATURES), 64bit.
 * @{ */
/** Guest: handles HOST_MSG_FILE_SET_SIZE. */
#define VBOX_GUESTCTRL_GF_0_SET_SIZE                    ((UINT64)1 << 0)
/** Guest: handles HOST_MSG_SHUTDOWN. */
#define VBOX_GUESTCTRL_GF_0_SHUTDOWN                    ((UINT64)1 << 3)
/** Guest: the arguments start with argv[0]. */
#define VBOX_GUESTCTRL_GF_0_PROCESS_ARGV0               ((UINT64)1 << 1)
/** Guest: handles the file, directory and file system messages itself,
 *  instead of the host running toolbox commands (vbox_ls, vbox_stat, ...). */
#define VBOX_GUESTCTRL_GF_0_TOOLBOX_AS_CMDS             ((UINT64)1 << 4)
/** Guest: HOST_MSG_EXEC_CMD may carry a working directory. */
#define VBOX_GUESTCTRL_GF_0_PROCESS_CWD                 ((UINT64)1 << 5)
/** Guest: handles HOST_MSG_MOUNT_POINTS. */
#define VBOX_GUESTCTRL_GF_0_MOUNT_POINTS_ENUM           ((UINT64)1 << 6)
#define VBOX_GUESTCTRL_GF_1_MUST_BE_ONE                 ((UINT64)1 << 63)
/** Host: sends argv[0] as the first argument. */
#define VBOX_GUESTCTRL_HF_0_PROCESS_ARGV0               ((UINT64)1 << 1)
/** Host: understands GUEST_FILE_NOTIFYTYPE_READ_OFFSET / _WRITE_OFFSET. */
#define VBOX_GUESTCTRL_HF_0_NOTIFY_RDWR_OFFSET          ((UINT64)1 << 0)
/** Host: sends the working directory. */
#define VBOX_GUESTCTRL_HF_0_PROCESS_CWD                 ((UINT64)1 << 2)
/** @} */

/*********************************************************************************************************************************
*   Shared folders HGCM service (used by VBoxSF)                                                                                 *
*********************************************************************************************************************************/

#define SHFL_SERVICE_NAME                               "VBoxSharedFolders"
/** Longest shared folder name, in characters. */
#define SHFL_MAX_LEN                                    256
/** Most mappings the host gives a guest. */
#define SHFL_MAX_MAPPINGS                               64

/** @name Guest functions (include/VBox/shflsvc.h).  Strings are SHFLSTRING,
 *  UTF-16 unless the client did SHFL_FN_SET_UTF8.
 * @{ */
/** Lists the mappings.  Parms: [0] SHFL_MF_XXX flags (32bit in), [1] number
 *  of entries (32bit in/out), [2] SHFLMAPPING array (buffer out).
 *  VINF_BUFFER_OVERFLOW if there are more. */
#define SHFL_FN_QUERY_MAPPINGS                          1
#define SHFL_FN_QUERY_MAPPINGS_CPARMS                   3
/** Gets a mapping's name.  Parms: [0] root (32bit in), [1] SHFLSTRING (buffer out). */
#define SHFL_FN_QUERY_MAP_NAME                          2
#define SHFL_FN_QUERY_MAP_NAME_CPARMS                   2
#define SHFL_FN_CREATE                                  3
#define SHFL_FN_CLOSE                                   4
#define SHFL_FN_READ                                    5
#define SHFL_FN_WRITE                                   6
#define SHFL_FN_LOCK                                    7
#define SHFL_FN_LIST                                    8
#define SHFL_FN_INFORMATION                             9
#define SHFL_FN_REMOVE                                  11
/** Unmaps a folder.  Parms: [0] root (32bit in). */
#define SHFL_FN_UNMAP_FOLDER                            13
#define SHFL_FN_UNMAP_FOLDER_CPARMS                     1
#define SHFL_FN_RENAME                                  14
#define SHFL_FN_FLUSH                                   15
#define SHFL_FN_SET_UTF8                                16
/** Maps a folder by name.  Parms: [0] SHFLSTRING name (buffer in), [1] root
 *  (32bit out), [2] path delimiter (32bit in), [3] case sensitive (32bit in). */
#define SHFL_FN_MAP_FOLDER                              17
#define SHFL_FN_MAP_FOLDER_CPARMS                       4
#define SHFL_FN_QUERY_MAP_INFO                          21
#define SHFL_FN_WAIT_FOR_MAPPINGS_CHANGES               22
#define SHFL_FN_CANCEL_MAPPINGS_CHANGES_WAITS           23
#define SHFL_FN_SET_FILE_SIZE                           24
#define SHFL_FN_QUERY_FEATURES                          25
#define SHFL_FN_CLOSE_AND_REMOVE                        28
#define SHFL_FN_SET_ERROR_STYLE                         29
/** @} */

/** @name SHFL_FN_QUERY_MAPPINGS flags
 * @{ */
#define SHFL_MF_UCS2                                    0x00000000
#define SHFL_MF_UTF8                                    0x00000001
#define SHFL_MF_AUTOMOUNT                               0x00000010
/** @} */

/** Root handle of a mapping. */
typedef UINT32 SHFLROOT;

/** Shared folders string: sizes in bytes, the string is zero terminated. */
typedef struct SHFLSTRING
{
    /** Size of String in bytes. */
    USHORT u16Size;
    /** Length of String in bytes, terminator excluded. */
    USHORT u16Length;
    union
    {
        char   ach[1];
        WCHAR  utf16[1];
    } String;
} SHFLSTRING;
/** Size of SHFLSTRING without the string. */
#define SHFLSTRING_HEADER_SIZE                          4

/** A mapping in the SHFL_FN_QUERY_MAPPINGS output. */
typedef struct SHFLMAPPING
{
    /** SHFL_MS_NEW (always). */
    UINT32 u32Status;
    SHFLROOT root;
} SHFLMAPPING;

/** Host handle of an open file or directory. */
typedef UINT64 SHFLHANDLE;
#define SHFL_HANDLE_NIL                                 (~(SHFLHANDLE)0)
#define SHFL_HANDLE_ROOT                                ((SHFLHANDLE)0)

/** @name More guest functions: parameters.
 * @{ */
/** Opens, creates or looks up an object.  Parms: [0] root (32bit in),
 *  [1] SHFLSTRING path (buffer in), [2] SHFLCREATEPARMS (buffer in/out). */
#define SHFL_FN_CREATE_CPARMS                           3
/** Closes a handle.  Parms: [0] root (32bit in), [1] handle (64bit in). */
#define SHFL_FN_CLOSE_CPARMS                            2
/** Reads.  Parms: [0] root (32bit in), [1] handle (64bit in), [2] offset
 *  (64bit in), [3] bytes (32bit in/out), [4] data (buffer out). */
#define SHFL_FN_READ_CPARMS                             5
/** Lists a directory.  Parms: [0] root (32bit in), [1] handle (64bit in),
 *  [2] SHFL_LIST_XXX (32bit in), [3] buffer size (32bit in/out),
 *  [4] SHFLSTRING filter (buffer in, may be empty), [5] SHFLDIRINFO entries
 *  (buffer out), [6] 1 while there are more (32bit out), [7] number of
 *  entries (32bit out).  VERR_NO_MORE_FILES at the end. */
#define SHFL_FN_LIST_CPARMS                             8
/** Gets or sets information.  Parms: [0] root (32bit in), [1] handle
 *  (64bit in), [2] SHFL_INFO_XXX (32bit in), [3] size (32bit in/out),
 *  [4] SHFLFSOBJINFO, SHFLVOLINFO or SHFLSTRING (buffer in/out).  Setting
 *  SHFL_INFO_FILE changes the times that aren't 0 and fMode if it isn't 0. */
#define SHFL_FN_INFORMATION_CPARMS                      5
/** Writes.  Parms: [0] root (32bit in), [1] handle (64bit in), [2] offset
 *  (64bit in/out), [3] bytes (32bit in/out), [4] data (buffer in). */
#define SHFL_FN_WRITE_CPARMS                            5
/** Removes a file or directory.  Parms: [0] root (32bit in), [1] SHFLSTRING
 *  path (buffer in), [2] SHFL_REMOVE_XXX (32bit in). */
#define SHFL_FN_REMOVE_CPARMS                           3
/** Renames.  Parms: [0] root (32bit in), [1] SHFLSTRING source (buffer in),
 *  [2] SHFLSTRING destination (buffer in), [3] SHFL_RENAME_XXX (32bit in). */
#define SHFL_FN_RENAME_CPARMS                           4
/** Flushes a file.  Parms: [0] root (32bit in), [1] handle (64bit in). */
#define SHFL_FN_FLUSH_CPARMS                            2
/** Sets the size of a file.  Parms: [0] root (32bit in), [1] handle
 *  (64bit in), [2] new size (64bit in). */
#define SHFL_FN_SET_FILE_SIZE_CPARMS                    3
/** Gets a mapping's name and mount point.  Parms: [0] root (32bit in),
 *  [1] SHFLSTRING name (buffer in/out), [2] SHFLSTRING mount point (buffer
 *  in/out), [3] SHFL_MIQF_XXX in / SHFL_MIF_XXX out (64bit), [4] root ID
 *  version (32bit out). */
#define SHFL_FN_QUERY_MAP_INFO_CPARMS                   5
/** Waits for the mappings to change.  Parms: [0] version (32bit in/out).
 *  VINF_TRY_AGAIN after a restore, VERR_CANCELLED when cancelled. */
#define SHFL_FN_WAIT_FOR_MAPPINGS_CHANGES_CPARMS        1
/** Cancels the client's SHFL_FN_WAIT_FOR_MAPPINGS_CHANGES calls.  No parms. */
#define SHFL_FN_CANCEL_MAPPINGS_CHANGES_WAITS_CPARMS    0
/** @} */

/** Locks or unlocks a byte range.  Parms: [0] root (32bit in), [1] handle
 *  (64bit in), [2] offset (64bit in), [3] length (64bit in), [4] SHFL_LOCK_XXX
 *  (32bit in).  VERR_FILE_LOCK_VIOLATION if it is locked already. */
#define SHFL_FN_LOCK_CPARMS                             5

/** @name SHFL_FN_LOCK flags
 * @{ */
#define SHFL_LOCK_CANCEL                                0x0
#define SHFL_LOCK_SHARED                                0x1
#define SHFL_LOCK_EXCLUSIVE                             0x2
#define SHFL_LOCK_NOWAIT                                0x0
#define SHFL_LOCK_PARTIAL                               0x0
/** @} */

/** @name SHFL_FN_QUERY_MAP_INFO flags
 * @{ */
/** In: the guest prefers drive letters as mount points. */
#define SHFL_MIQF_DRIVE_LETTER                          ((UINT64)1 << 0)
/** Out: writable. */
#define SHFL_MIF_WRITABLE                               ((UINT64)1 << 0)
/** Out: to be mounted automatically. */
#define SHFL_MIF_AUTO_MOUNT                             ((UINT64)1 << 1)
/** @} */

/** @name SHFL_FN_REMOVE flags
 * @{ */
#define SHFL_REMOVE_FILE                                0x1
#define SHFL_REMOVE_DIR                                 0x2
/** @} */

/** @name SHFL_FN_RENAME flags
 * @{ */
#define SHFL_RENAME_FILE                                0x1
#define SHFL_RENAME_DIR                                 0x2
#define SHFL_RENAME_REPLACE_IF_EXISTS                   0x4
/** @} */

/** DOS attribute "normal" (FILE_ATTRIBUTE_NORMAL) in fMode, so that clearing
 *  all attributes still is a change for SHFL_FN_INFORMATION. */
#define RTFS_DOS_NT_NORMAL                              (0x0080U << RTFS_DOS_SHIFT)

/** @name SHFL_FN_LIST flags
 * @{ */
#define SHFL_LIST_NONE                                  0
#define SHFL_LIST_RETURN_ONE                            1
#define SHFL_LIST_RESTART                               2
/** @} */

/** @name SHFL_FN_INFORMATION flags
 * @{ */
#define SHFL_INFO_GET                                   0x00
#define SHFL_INFO_SET                                   0x01
#define SHFL_INFO_NAME                                  0x02
#define SHFL_INFO_SIZE                                  0x04
#define SHFL_INFO_FILE                                  0x08
#define SHFL_INFO_VOLUME                                0x10
/** @} */

/** @name SHFLCREATEPARMS::CreateFlags (SHFL_CF_XXX)
 * @{ */
#define SHFL_CF_LOOKUP                                  0x00000001
#define SHFL_CF_DIRECTORY                               0x00000004
#define SHFL_CF_ACT_OPEN_IF_EXISTS                      0x00000000
#define SHFL_CF_ACT_FAIL_IF_EXISTS                      0x00000010
#define SHFL_CF_ACT_REPLACE_IF_EXISTS                   0x00000020
#define SHFL_CF_ACT_OVERWRITE_IF_EXISTS                 0x00000030
#define SHFL_CF_ACT_CREATE_IF_NEW                       0x00000000
#define SHFL_CF_ACT_FAIL_IF_NEW                         0x00000100
#define SHFL_CF_ACCESS_READ                             0x00001000
#define SHFL_CF_ACCESS_WRITE                            0x00002000
#define SHFL_CF_ACCESS_DENYNONE                         0x00000000
#define SHFL_CF_ACCESS_DENYREAD                         0x00004000
#define SHFL_CF_ACCESS_DENYWRITE                        0x00008000
#define SHFL_CF_ACCESS_ATTR_READ                        0x00010000
#define SHFL_CF_ACCESS_ATTR_WRITE                       0x00020000
/** @} */

/** @name SHFLCREATEPARMS::Result (SHFLCREATERESULT)
 * @{ */
#define SHFL_NO_RESULT                                  0
#define SHFL_PATH_NOT_FOUND                             1
#define SHFL_FILE_NOT_FOUND                             2
#define SHFL_FILE_EXISTS                                3
#define SHFL_FILE_CREATED                               4
#define SHFL_FILE_REPLACED                              5
/** @} */

#pragma pack(push, 1)
/** Object attributes (SHFLFSOBJATTR): fMode has the RTFS_XXX bits, the DOS
 *  attributes shifted by RTFS_DOS_SHIFT. */
typedef struct SHFLFSOBJATTR
{
    UINT32 fMode;
    /** SHFLFSOBJATTRADD_XXX, 1 (nothing) from VBoxSF's requests. */
    UINT32 enmAdditional;
    UCHAR  abUnion[36];
} SHFLFSOBJATTR;

/** Object information (SHFLFSOBJINFO); times are nanoseconds since
 *  1970-01-01 UTC (RTTIMESPEC). */
typedef struct SHFLFSOBJINFO
{
    INT64 cbObject;
    INT64 cbAllocated;
    INT64 AccessTime;
    INT64 ModificationTime;
    INT64 ChangeTime;
    INT64 BirthTime;
    SHFLFSOBJATTR Attr;
} SHFLFSOBJINFO;

/** SHFL_FN_CREATE parameters and results. */
typedef struct SHFLCREATEPARMS
{
    /** OUT: the handle, SHFL_HANDLE_NIL if none. */
    SHFLHANDLE Handle;
    /** OUT: SHFL_XXX result (SHFLCREATERESULT). */
    UINT32 Result;
    /** IN: SHFL_CF_XXX. */
    UINT32 CreateFlags;
    /** IN: attributes for a new object; OUT: the object's. */
    SHFLFSOBJINFO Info;
} SHFLCREATEPARMS;
#pragma pack(pop)

/** A SHFL_FN_LIST entry; the next one follows the name's String at
 *  offset SHFLDIRINFO_NAME_OFFSET + name.u16Size. */
typedef struct SHFLDIRINFO
{
    SHFLFSOBJINFO Info;
    /** Length of the 8.3 name in characters, 0 if the host has none. */
    USHORT cucShortName;
    WCHAR  uszShortName[14];
    SHFLSTRING name;
} SHFLDIRINFO;
/** Offset of SHFLDIRINFO::name.String. */
#define SHFLDIRINFO_NAME_OFFSET                         (92 + 2 + 28 + SHFLSTRING_HEADER_SIZE)

/** Volume information (SHFL_INFO_VOLUME). */
typedef struct SHFLVOLINFO
{
    INT64  ullTotalAllocationBytes;
    INT64  ullAvailableAllocationBytes;
    UINT32 ulBytesPerAllocationUnit;
    UINT32 ulBytesPerSector;
    UINT32 ulSerial;
    /** SHFLFSPROPERTIES. */
    UINT32 cbMaxComponent;
    UCHAR  fRemote;
    UCHAR  fCaseSensitive;
    UCHAR  fReadOnly;
    UCHAR  fSupportsUnicode;
    UCHAR  fCompressed;
    UCHAR  fFileCompression;
    UCHAR  abPadding[2];
} SHFLVOLINFO;


#pragma pack(pop)

/** Compile time size check (C89: a negative array size fails to compile). */
#define VMMDEV_ASSERT_SIZE(type, cb)    typedef char type##_SizeCheck[sizeof(type) == (cb) ? 1 : -1]

/* Sizes from the AssertCompileSize statements in VirtualBox 7.2.22
   include/VBox/VMMDev.h; the host checks most of them. */
VMMDEV_ASSERT_SIZE(VMMDevRequestHeader, 24);
VMMDEV_ASSERT_SIZE(VMMDevReqMouseStatus, 24+12);
VMMDEV_ASSERT_SIZE(VMMDevReqHostVersion, 24+16);
VMMDEV_ASSERT_SIZE(VMMDevReqHostTime, 24+8);
VMMDEV_ASSERT_SIZE(VMMDevReqSessionId, 24+8);
VMMDEV_ASSERT_SIZE(VMMDevEvents, 24+4);
VMMDEV_ASSERT_SIZE(VMMDevCtlGuestFilterMask, 24+8);
VMMDEV_ASSERT_SIZE(VMMDevReportGuestInfo, 24+8);
VMMDEV_ASSERT_SIZE(VMMDevReportGuestInfo2, 24+144);
VMMDEV_ASSERT_SIZE(VMMDevReqGuestCapabilities2, 24+8);
VMMDEV_ASSERT_SIZE(VMMDevReportGuestStatus, 24+12);
VMMDEV_ASSERT_SIZE(VMMDevPowerStateRequest, 24+4);
VMMDEV_ASSERT_SIZE(VMMDevReqHeartbeat, 24+12);
VMMDEV_ASSERT_SIZE(VMMDevDisplayChangeRequestEx, 24+32);
VMMDEV_ASSERT_SIZE(VMMDevSeamlessChangeRequest, 24+8);
VMMDEV_ASSERT_SIZE(RTRECT, 16);
VMMDEV_ASSERT_SIZE(VMMDevVideoSetVisibleRegion, 24+4+16);
/* Sizes from the AssertCompileSize statements in include/VBox/GuestHost/GuestControl.h. */
VMMDEV_ASSERT_SIZE(GSTCTLFSOBJATTR, 136);
VMMDEV_ASSERT_SIZE(GSTCTLFSOBJINFO, 184);
VMMDEV_ASSERT_SIZE(GSTCTLDIRENTRYEX, 184+292);
VMMDEV_ASSERT_SIZE(GSTCTLDIRENTRYLISTHDR, 12);
VMMDEV_ASSERT_SIZE(GSTCTLFSINFO, 553);
VMMDEV_ASSERT_SIZE(VBoxGuestStatistics, 19*4);
VMMDEV_ASSERT_SIZE(VMMDevReportGuestStats, 24+19*4);
VMMDEV_ASSERT_SIZE(VMMDevGetStatisticsChangeRequest, 24+8);
VMMDEV_ASSERT_SIZE(VMMDevHGCMRequestHeader, 24+8);
VMMDEV_ASSERT_SIZE(VMMDevHGCMConnect, 32+132+4);
VMMDEV_ASSERT_SIZE(VMMDevHGCMDisconnect, 32+4);
VMMDEV_ASSERT_SIZE(VMMDevHGCMCall, 32+12);
VMMDEV_ASSERT_SIZE(HGCMFunctionParameter32, 4+8);
VMMDEV_ASSERT_SIZE(VMMDevHGCMCancel2, 24+8);
/* Sizes from the AssertCompileSize statements in include/VBox/shflsvc.h. */
VMMDEV_ASSERT_SIZE(SHFLSTRING, 6);
VMMDEV_ASSERT_SIZE(SHFLMAPPING, 8);
VMMDEV_ASSERT_SIZE(SHFLFSOBJATTR, 44);
VMMDEV_ASSERT_SIZE(SHFLFSOBJINFO, 92);
VMMDEV_ASSERT_SIZE(SHFLCREATEPARMS, 108);
VMMDEV_ASSERT_SIZE(SHFLDIRINFO, SHFLDIRINFO_NAME_OFFSET + 2);
VMMDEV_ASSERT_SIZE(SHFLVOLINFO, 40);

#endif /* !VBOXGUEST98_VMMDEV_H */
