/** @file
 * VBoxGst - VirtualBox Guest Additions driver for Windows 98/ME, internal header.
 *
 * Layout (mirrors upstream src/VBox/Additions/common/VBoxGuest):
 *  - Driver.c     DriverEntry, AddDevice, unload, create/close/cleanup.
 *  - PnP.c        PnP and power IRPs, hardware resource setup/teardown.
 *  - Interrupt.c  ISR and DPC.
 *  - Common.c     OS independent core: host init, sessions, events, heartbeat.
 *  - Hgcm.c       HGCM connect/disconnect/call.
 *  - Ioctl.c      Device control and internal device control dispatch.
 *  - VbglR0.c     VMMDev request allocation and submission.
 *  - Log.c        Release logging through the VMMDev backdoor port.
 */

#ifndef VBOXGST_H
#define VBOXGST_H

#include <wdm.h>

#include "VBoxGuest98.h"


/*********************************************************************************************************************************
*   VBGL ring-0 request layer (VbglR0.c)                                                                                         *
*********************************************************************************************************************************/

/** Global VBGL ring-0 data. */
typedef struct VBGLDATA
{
    /** I/O port to issue requests to. */
    USHORT portVMMDev;
    /** VMMDev adapter memory region if available. */
    VMMDevMemory volatile *pVMMDevMemory;
    /** DMA adapter used for allocating physically contiguous requests. */
    PDMA_ADAPTER pDmaAdapter;
    /** The host version data. */
    VMMDevReqHostVersion hostVersion;
} VBGLDATA;

extern VBGLDATA g_vbgldata;

int     VbglR0InitPrimary(PDEVICE_OBJECT pPdo, USHORT portVMMDev, VMMDevMemory volatile *pVMMDevMemory, UINT32 *pfFeatures);
void    VbglR0TerminatePrimary(void);
int     VbglR0GRAlloc(VMMDevRequestHeader **ppReq, ULONG cbReq, VMMDevRequestType enmReqType);
int     VbglR0GRPerform(VMMDevRequestHeader *pReq);
void    VbglR0GRFree(VMMDevRequestHeader *pReq);
UINT32  VbglR0GRPhysAddr(VMMDevRequestHeader *pReq);


/*********************************************************************************************************************************
*   Device extension and sessions                                                                                                *
*********************************************************************************************************************************/

/** Per bit usage counters, used to combine the masks of all sessions. */
typedef struct VBOXGUESTBITUSAGETRACER
{
    /** Number of sessions using each bit. */
    UINT32 acPerBitUsage[32];
    /** Combined mask: bits with a non-zero usage count. */
    UINT32 fMask;
} VBOXGUESTBITUSAGETRACER;

struct VBOXGUESTSESSION;

/** A thread waiting for events or for an HGCM request to complete. */
typedef struct VBOXGUESTWAIT
{
    /** Entry in VBOXGUESTDEVEXT::WaitList. */
    LIST_ENTRY ListEntry;
    /** Signalled when the wait is satisfied or interrupted. */
    KEVENT Event;
    /** The session the waiter belongs to. */
    struct VBOXGUESTSESSION *pSession;
    /** Events the waiter is interested in (0 for HGCM waits). */
    UINT32 fReqEvents;
    /** Events delivered to the waiter. */
    UINT32 fResEvents;
    /** The HGCM request being waited on (NULL for event waits). */
    VMMDevHGCMRequestHeader volatile *pHGCMReq;
    /** Set when the wait was interrupted. */
    BOOLEAN fInterrupted;
} VBOXGUESTWAIT;
typedef VBOXGUESTWAIT *PVBOXGUESTWAIT;

/** A client session (one per file object, plus one for kernel clients). */
typedef struct VBOXGUESTSESSION
{
    /** Entry in VBOXGUESTDEVEXT::SessionList. */
    LIST_ENTRY ListEntry;
    /** Whether this session belongs to a user mode handle. */
    BOOLEAN fUserSession;
    /** VMMDEV_REQUESTOR_XXX put in every request made for this session. */
    UINT32 fRequestor;
    /** Events this session wants to receive. */
    UINT32 fEventFilter;
    /** Guest capabilities this session reports. */
    UINT32 fCapabilities;
    /** Mouse features this session requires. */
    UINT32 fMouseStatus;
    /** HGCM clients connected by this session (0 = free slot). */
    UINT32 aHGCMClientIds[VBOXGUEST98_MAX_HGCM_CLIENTS];
} VBOXGUESTSESSION;
typedef VBOXGUESTSESSION *PVBOXGUESTSESSION;

/** VBOXGUESTDEVEXT::uInitState values. */
typedef enum VBOXGUESTDEVEXTINITSTATE
{
    VBOXGUESTDEVEXT_INIT_STATE_INVALID = 0,
    VBOXGUESTDEVEXT_INIT_STATE_FUNDAMENT,
    VBOXGUESTDEVEXT_INIT_STATE_RESOURCES,
    VBOXGUESTDEVEXT_INIT_STATE_DELETED
} VBOXGUESTDEVEXTINITSTATE;

/** VBox guest device (data) extension, the OS independent part. */
typedef struct VBOXGUESTDEVEXT
{
    /** VBOXGUESTDEVEXT_INIT_STATE_XXX. */
    VBOXGUESTDEVEXTINITSTATE uInitState;
    /** The guest OS type reported to the host. */
    VBOXOSTYPE enmOsType;
    /** The base of the adapter I/O ports. */
    USHORT IOPortBase;
    /** Pointer to the mapping of the VMMDev adapter memory. */
    VMMDevMemory volatile *pVMMDevMemory;
    /** Host feature flags (VMMDEV_HVF_XXX). */
    UINT32 fHostFeatures;
    /** Events that are always enabled, whatever the sessions want. */
    UINT32 fFixedEvents;

    /** Preallocated event acknowledge request, used by the ISR. */
    VMMDevEvents *pIrqAckEvents;
    /** Preallocated mouse status request, used by the DPC. */
    VMMDevReqMouseStatus *pMouseStatusReq;
    /** Preallocated power off request, used during shutdown. */
    VMMDevPowerStateRequest *pPowerStateReq;
    /** Events acknowledged by the ISR, not yet processed by the DPC.
     *  Only accessed with the vgdrvAtomic helpers. */
    UINT32 volatile fIsrEvents;

    /** Spinlock protecting WaitList and fPendingEvents. */
    KSPIN_LOCK EventSpinLock;
    /** Threads waiting for events or HGCM completion (VBOXGUESTWAIT). */
    LIST_ENTRY WaitList;
    /** Events that no waiter has consumed yet. */
    UINT32 fPendingEvents;

    /** Mutex serializing session list and host mask updates (PASSIVE_LEVEL). */
    KMUTEX SessionMutex;
    /** All open sessions (VBOXGUESTSESSION). */
    LIST_ENTRY SessionList;
    /** Number of open user mode sessions. */
    ULONG cUserSessions;
    /** Session used by kernel mode clients through internal I/O controls. */
    PVBOXGUESTSESSION pKernelSession;
    /** Combined event filter of all sessions. */
    VBOXGUESTBITUSAGETRACER EventFilterTracker;
    /** Combined guest capabilities of all sessions. */
    VBOXGUESTBITUSAGETRACER CapabilitiesTracker;
    /** Combined mouse features of all sessions. */
    VBOXGUESTBITUSAGETRACER MouseStatusTracker;

    /** Spinlock protecting the mouse notification callback. Required since the
     *  consumer is called from the DPC and not the ISR. */
    KSPIN_LOCK MouseEventAccessSpinLock;
    /** Mouse notification callback (VBoxMous). */
    PFNVBOXGUEST98MOUSENOTIFY pfnMouseNotify;
    /** User argument for pfnMouseNotify. */
    PVOID pvMouseNotifyUser;

    /** Preallocated heartbeat request, used by the heartbeat DPC. */
    VMMDevRequestHeader *pHeartbeatReq;
    /** Heartbeat timer. */
    KTIMER HeartbeatTimer;
    /** Heartbeat DPC. */
    KDPC HeartbeatDpc;
} VBOXGUESTDEVEXT;
/** Pointer to the VBoxGuest driver data. */
typedef VBOXGUESTDEVEXT *PVBOXGUESTDEVEXT;

/** PnP state of the device (VBOXGUESTDEVEXTWIN::enmDevState). */
typedef enum VGDRVWINDEVSTATE
{
    VGDRVWINDEVSTATE_STOPPED = 0,
    VGDRVWINDEVSTATE_STARTED,
    VGDRVWINDEVSTATE_SURPRISE_REMOVED,
    VGDRVWINDEVSTATE_REMOVED
} VGDRVWINDEVSTATE;

/** Subclassing the device extension for adding windows-specific bits. */
typedef struct VBOXGUESTDEVEXTWIN
{
    /** The common device extension core. */
    VBOXGUESTDEVEXT Core;

    /** Our functional device object. */
    PDEVICE_OBJECT pDeviceObject;
    /** The physical device object (created by the PCI bus driver). */
    PDEVICE_OBJECT pPhysicalDeviceObject;
    /** Top of the stack. */
    PDEVICE_OBJECT pNextLowerDriver;
    /** PnP state. */
    VGDRVWINDEVSTATE enmDevState;

    /** Interrupt object pointer. */
    PKINTERRUPT pInterruptObject;
    /** Device interrupt level. */
    ULONG uInterruptLevel;
    /** Device interrupt vector. */
    ULONG uInterruptVector;
    /** Affinity mask. */
    KAFFINITY fInterruptAffinity;
    /** LevelSensitive or Latched. */
    KINTERRUPT_MODE enmInterruptMode;

    /** I/O port base and length. */
    PHYSICAL_ADDRESS uIoPortPhysAddr;
    ULONG cbIoPort;
    /** Physical address of VMMDev memory. */
    PHYSICAL_ADDRESS uVmmDevMemoryPhysAddr;
    /** Length of VMMDev memory. */
    ULONG cbVmmDevMemory;
    /** Kernel mapping of the VMMDev memory (MmMapIoSpace). */
    PVOID pvMMIOBase;

    /** Symbolic link name of the registered device interface. */
    UNICODE_STRING InterfaceName;

    /** Configuration (driver registry key): tell the host to power off the
     *  VM when Windows shuts down ("PowerOffOnShutdown", default 0). */
    BOOLEAN fPowerOffOnShutdown;
} VBOXGUESTDEVEXTWIN;
typedef VBOXGUESTDEVEXTWIN *PVBOXGUESTDEVEXTWIN;


/*********************************************************************************************************************************
*   Common core (Common.c)                                                                                                       *
*********************************************************************************************************************************/

int  VGDrvCommonInitDevExtFundament(PVBOXGUESTDEVEXT pDevExt, VBOXOSTYPE enmOsType);
void VGDrvCommonDeleteDevExtFundament(PVBOXGUESTDEVEXT pDevExt);
int  VGDrvCommonInitDevExtResources(PVBOXGUESTDEVEXT pDevExt, PDEVICE_OBJECT pPdo, USHORT IOPortBase,
                                    void *pvMMIOBase, UINT32 cbMMIO, UINT32 fFixedEvents);
void VGDrvCommonDeleteDevExtResources(PVBOXGUESTDEVEXT pDevExt);
void VGDrvCommonProcessEvents(PVBOXGUESTDEVEXT pDevExt, UINT32 fEvents);
void VGDrvCommonReportPowerOff(PVBOXGUESTDEVEXT pDevExt);

int  VGDrvCommonCreateSession(PVBOXGUESTDEVEXT pDevExt, BOOLEAN fUserSession, PVBOXGUESTSESSION *ppSession);
void VGDrvCommonCloseSession(PVBOXGUESTDEVEXT pDevExt, PVBOXGUESTSESSION pSession);
int  VGDrvCommonSetSessionEventFilter(PVBOXGUESTDEVEXT pDevExt, PVBOXGUESTSESSION pSession,
                                      UINT32 fOrMask, UINT32 fNotMask);
int  VGDrvCommonSetSessionCapabilities(PVBOXGUESTDEVEXT pDevExt, PVBOXGUESTSESSION pSession,
                                       UINT32 fOrMask, UINT32 fNotMask);
int  VGDrvCommonSetSessionMouseStatus(PVBOXGUESTDEVEXT pDevExt, PVBOXGUESTSESSION pSession, UINT32 fFeatures);
int  VGDrvCommonSetMouseNotifyCallback(PVBOXGUESTDEVEXT pDevExt, PFNVBOXGUEST98MOUSENOTIFY pfnNotify, PVOID pvUser);

int  VGDrvCommonWaitForEvents(PVBOXGUESTDEVEXT pDevExt, PVBOXGUESTSESSION pSession,
                              UINT32 fReqEvents, UINT32 cMsTimeout, UINT32 *pfEvents);
int  VGDrvCommonWaitForHgcm(PVBOXGUESTDEVEXT pDevExt, PVBOXGUESTSESSION pSession,
                            VMMDevHGCMRequestHeader volatile *pHGCMReq, UINT32 cMsTimeout);
void VGDrvCommonInterruptWaits(PVBOXGUESTDEVEXT pDevExt, PVBOXGUESTSESSION pSession);


/*********************************************************************************************************************************
*   HGCM (Hgcm.c)                                                                                                                *
*********************************************************************************************************************************/

int  VGDrvHgcmConnect(PVBOXGUESTDEVEXT pDevExt, PVBOXGUESTSESSION pSession, const char *pszService, UINT32 *pidClient);
int  VGDrvHgcmDisconnect(PVBOXGUESTDEVEXT pDevExt, PVBOXGUESTSESSION pSession, UINT32 idClient);
int  VGDrvHgcmCall(PVBOXGUESTDEVEXT pDevExt, PVBOXGUESTSESSION pSession, VBOXGUEST98HGCMCALL *pCall, ULONG cbCall);
void VGDrvHgcmDisconnectSessionClients(PVBOXGUESTDEVEXT pDevExt, PVBOXGUESTSESSION pSession);


/*********************************************************************************************************************************
*   Windows specific (Driver.c, PnP.c, Interrupt.c, Ioctl.c)                                                                     *
*********************************************************************************************************************************/

NTSTATUS NTAPI  vgdrvWinPnP(PDEVICE_OBJECT pDevObj, PIRP pIrp);
NTSTATUS NTAPI  vgdrvWinPower(PDEVICE_OBJECT pDevObj, PIRP pIrp);
BOOLEAN  NTAPI  vgdrvWinIsrHandler(PKINTERRUPT pInterrupt, PVOID pvContext);
VOID     NTAPI  vgdrvWinDpcHandler(PKDPC pDpc, PDEVICE_OBJECT pDevObj, PIRP pIrp, PVOID pvContext);
NTSTATUS NTAPI  vgdrvWinIoCtl(PDEVICE_OBJECT pDevObj, PIRP pIrp);
NTSTATUS NTAPI  vgdrvWinInternalIoCtl(PDEVICE_OBJECT pDevObj, PIRP pIrp);
NTSTATUS        vgdrvWinCompleteRequest(PIRP pIrp, NTSTATUS rcNt, ULONG_PTR cbInfo);
void            vgdrvWinDeleteDevice(PVBOXGUESTDEVEXTWIN pDevExt);


/*********************************************************************************************************************************
*   Logging (Log.c)                                                                                                              *
*********************************************************************************************************************************/

void VGDrvLogInit(void);
void VGDrvLogWrite(const char *pch, ULONG cch);
void __cdecl VGDrvLogRel(const char *pszFormat, ...);

/** Debug logging, checked builds only: VGDRV_LOG(("format\n", args)). */
#if DBG
# define VGDRV_LOG(a)   VGDrvLogRel a
#else
# define VGDRV_LOG(a)   do { } while (0)
#endif


/*********************************************************************************************************************************
*   Helpers                                                                                                                      *
*********************************************************************************************************************************/

/** Converts a VBox status code to an NT status code. */
NTSTATUS vgdrvRcToNtStatus(int rc);

/** Full memory barrier (xchg is implicitly locked). */
__forceinline void vgdrvMemoryBarrier(void)
{
    LONG lDummy;
    __asm xchg [lDummy], eax
}

/** Atomically ORs fBits into *pu32.  Safe at any IRQL including in the ISR.
 *  (InterlockedOr is not available on Windows 98.) */
__forceinline void vgdrvAtomicOrU32(UINT32 volatile *pu32, UINT32 fBits)
{
    __asm
    {
        mov     ecx, [pu32]
        mov     eax, [fBits]
        lock or [ecx], eax
    }
}

/** Atomically exchanges *pu32 with u32New, returning the old value. */
__forceinline UINT32 vgdrvAtomicXchgU32(UINT32 volatile *pu32, UINT32 u32New)
{
    UINT32 u32Old;
    __asm
    {
        mov     ecx, [pu32]
        mov     eax, [u32New]
        xchg    [ecx], eax
        mov     [u32Old], eax
    }
    return u32Old;
}

#endif /* !VBOXGST_H */
