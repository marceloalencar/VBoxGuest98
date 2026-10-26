/** @file
 * VBoxGuest98 - Interface between the VBoxGst driver and its clients.
 *
 * Shared by the driver, user mode programs (VBGTest, the future VBoxTray) and
 * kernel mode clients (the future VBoxMous filter driver).
 *
 * NOTE: This is NOT binary compatible with the VBGL_IOCTL_XXX interface of
 *       upstream VirtualBox.  It is a deliberately small interface that is
 *       easy to implement on WDM 1.0.  All structures are packed to 4 bytes
 *       and all I/O controls use METHOD_BUFFERED.
 *
 * User mode: include <windows.h> and <winioctl.h> before this header.
 * Kernel mode: include <wdm.h> before this header.
 * Exactly one translation unit must include <initguid.h> first in order to
 * instantiate GUID_DEVINTERFACE_VBOXGUEST98.
 */

#ifndef VBOXGUEST98_H
#define VBOXGUEST98_H

/** VirtualBox version the guest additions track.  These are also used by the
 *  resource scripts, which only see this part of the header (RC_INVOKED). */
#define VBOX_VERSION_MAJOR              7
#define VBOX_VERSION_MINOR              2
#define VBOX_VERSION_BUILD              22
#define VBOX_SVN_REV                    175245
#define VBOX_VERSION_STRING             "7.2.22"

#ifndef RC_INVOKED

#include "VMMDev.h"

#pragma pack(push, 4)

/*********************************************************************************************************************************
*   Versions and names                                                                                                           *
*********************************************************************************************************************************/

/** Version of this interface, high word is major (incompatible changes). */
#define VBOXGUEST98_IOCTL_VERSION       (0x00010003U)

/** Native device name. */
#define VBOXGUEST98_DEVICE_NAME_NT      L"\\Device\\VBoxGuest"
/** DOS device name (symbolic link). */
#define VBOXGUEST98_DEVICE_NAME_DOS     L"\\DosDevices\\VBoxGuest"

/** Device interface GUID, used by user mode to locate the device with SetupDi.
 *  {719B4B48-C5DF-4474-A8E7-AF615CA3BC83} */
DEFINE_GUID(GUID_DEVINTERFACE_VBOXGUEST98,
            0x719b4b48, 0xc5df, 0x4474, 0xa8, 0xe7, 0xaf, 0x61, 0x5c, 0xa3, 0xbc, 0x83);

/** Special timeout value: wait forever. */
#define VBOXGUEST98_WAIT_INFINITE       (0xffffffffU)

/** Largest raw VMMDev request accepted by IOCTL_VBOXGUEST98_VMMDEV_REQUEST. */
#define VBOXGUEST98_MAX_VMMDEV_REQUEST_SIZE (16384U)
/** Largest data area accepted by IOCTL_VBOXGUEST98_HGCM_CALL (256 KB before
 *  interface 1.3); room for a 2560x1440 32 bpp clipboard image.  The driver
 *  bounces the buffers through nonpaged memory, so big calls may fail with
 *  VERR_NO_MEMORY on guests with little RAM. */
#define VBOXGUEST98_MAX_HGCM_DATA_SIZE  (16U * 1024 * 1024)
/** Number of HGCM clients a single session may have connected at once. */
#define VBOXGUEST98_MAX_HGCM_CLIENTS    (32)
/** Largest text accepted by IOCTL_VBOXGUEST98_WRITE_LOG. */
#define VBOXGUEST98_MAX_LOG_SIZE        (4096U)


/*********************************************************************************************************************************
*   I/O control codes                                                                                                            *
*********************************************************************************************************************************/

#define VBOXGUEST98_IOCTL_CODE(Function) \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 2048 + (Function), METHOD_BUFFERED, FILE_ANY_ACCESS)

/** Get driver, interface and host versions.  Out: VBOXGUEST98VERSIONINFO. */
#define IOCTL_VBOXGUEST98_GET_VERSION_INFO          VBOXGUEST98_IOCTL_CODE(0)
/** Perform a raw VMMDev request.
 *  In/out: a VMMDevRequestHeader based request, header.size bytes long.
 *  Requests the driver manages itself (event acknowledging, filter mask,
 *  capabilities, mouse status, guest info, HGCM, heartbeat) are refused with
 *  STATUS_ACCESS_DENIED; use the dedicated I/O controls instead. */
#define IOCTL_VBOXGUEST98_VMMDEV_REQUEST            VBOXGUEST98_IOCTL_CODE(1)
/** Wait for VMMDev events.  In/out: VBOXGUEST98WAITEVENTS. */
#define IOCTL_VBOXGUEST98_WAIT_FOR_EVENTS           VBOXGUEST98_IOCTL_CODE(2)
/** Wake up all IOCTL_VBOXGUEST98_WAIT_FOR_EVENTS waiters of this handle. */
#define IOCTL_VBOXGUEST98_INTERRUPT_WAITS           VBOXGUEST98_IOCTL_CODE(3)
/** Change the events this handle is interested in.  In: VBOXGUEST98CHANGEMASK. */
#define IOCTL_VBOXGUEST98_CHANGE_FILTER_MASK        VBOXGUEST98_IOCTL_CODE(4)
/** Change the guest capabilities (VMMDEV_GUEST_SUPPORTS_XXX) this handle
 *  reports.  In: VBOXGUEST98CHANGEMASK. */
#define IOCTL_VBOXGUEST98_CHANGE_GUEST_CAPABILITIES VBOXGUEST98_IOCTL_CODE(5)
/** Set the mouse features (VMMDEV_MOUSE_GUEST_MASK bits) this handle
 *  requires.  In: VBOXGUEST98MOUSESTATUS. */
#define IOCTL_VBOXGUEST98_SET_MOUSE_STATUS          VBOXGUEST98_IOCTL_CODE(6)
/** Connect to an HGCM service.  In/out: VBOXGUEST98HGCMCONNECT. */
#define IOCTL_VBOXGUEST98_HGCM_CONNECT              VBOXGUEST98_IOCTL_CODE(7)
/** Disconnect from an HGCM service.  In/out: VBOXGUEST98HGCMDISCONNECT. */
#define IOCTL_VBOXGUEST98_HGCM_DISCONNECT           VBOXGUEST98_IOCTL_CODE(8)
/** Call an HGCM service function.  In/out: VBOXGUEST98HGCMCALL followed by
 *  cParms VBOXGUEST98HGCMPARM entries and the data area. */
#define IOCTL_VBOXGUEST98_HGCM_CALL                 VBOXGUEST98_IOCTL_CODE(9)
/** Write text to the release log (VBox.log on the host, through the VMMDev
 *  backdoor port; since interface 1.2).  In: the text, no terminator; lines
 *  should end with a newline. */
#define IOCTL_VBOXGUEST98_WRITE_LOG                 VBOXGUEST98_IOCTL_CODE(10)

/** @name Internal I/O controls (IRP_MJ_INTERNAL_DEVICE_CONTROL).
 * Only for kernel mode clients, must be sent at PASSIVE_LEVEL.
 * @{ */
/** Install (or remove, pfnNotify == NULL) the mouse position notification
 *  callback.  In: VBOXGUEST98MOUSENOTIFY.  There is only one callback, a new
 *  one replaces the previous one. */
#define IOCTL_VBOXGUEST98_INTERNAL_SET_MOUSE_NOTIFY VBOXGUEST98_IOCTL_CODE(64)
/** Set the mouse features required by the kernel mode client.
 *  In: VBOXGUEST98MOUSESTATUS. */
#define IOCTL_VBOXGUEST98_INTERNAL_SET_MOUSE_STATUS VBOXGUEST98_IOCTL_CODE(65)
/** @} */


/*********************************************************************************************************************************
*   I/O control structures                                                                                                       *
*********************************************************************************************************************************/

/** IOCTL_VBOXGUEST98_GET_VERSION_INFO output. */
typedef struct VBOXGUEST98VERSIONINFO
{
    /** VBOXGUEST98_IOCTL_VERSION. */
    UINT32 u32InterfaceVersion;
    /** Guest additions version. */
    USHORT uDriverMajor;
    USHORT uDriverMinor;
    UINT32 uDriverBuild;
    UINT32 uDriverRevision;
    /** Host (VirtualBox) version. */
    USHORT uHostMajor;
    USHORT uHostMinor;
    UINT32 uHostBuild;
    UINT32 uHostRevision;
    /** Host feature flags (VMMDEV_HVF_XXX). */
    UINT32 fHostFeatures;
    /** Guest OS type reported to the host (VBOXOSTYPE). */
    UINT32 enmOsType;
    /** Events always enabled by the driver. */
    UINT32 fFixedEvents;
    /** Heartbeat interval in milliseconds, 0 if disabled. */
    UINT32 cMsHeartbeatInterval;
    /** VBox status of the heartbeat setup (since interface 1.1). */
    INT32  rcHeartbeat;
} VBOXGUEST98VERSIONINFO;

/** IOCTL_VBOXGUEST98_WAIT_FOR_EVENTS input/output. */
typedef struct VBOXGUEST98WAITEVENTS
{
    /** IN: Timeout in milliseconds, VBOXGUEST98_WAIT_INFINITE or 0 (poll). */
    UINT32 cMsTimeout;
    /** IN: Events to wait for (VMMDEV_EVENT_XXX).  OUT: The events that
     *  occurred, 0 on timeout/interruption. */
    UINT32 fEvents;
    /** OUT: VINF_SUCCESS, VERR_TIMEOUT or VERR_INTERRUPTED. */
    INT32  rc;
} VBOXGUEST98WAITEVENTS;

/** IOCTL_VBOXGUEST98_CHANGE_FILTER_MASK and
 *  IOCTL_VBOXGUEST98_CHANGE_GUEST_CAPABILITIES input. */
typedef struct VBOXGUEST98CHANGEMASK
{
    /** Bits to set. */
    UINT32 fOrMask;
    /** Bits to clear (applied after fOrMask). */
    UINT32 fNotMask;
} VBOXGUEST98CHANGEMASK;

/** IOCTL_VBOXGUEST98_SET_MOUSE_STATUS and
 *  IOCTL_VBOXGUEST98_INTERNAL_SET_MOUSE_STATUS input. */
typedef struct VBOXGUEST98MOUSESTATUS
{
    /** VMMDEV_MOUSE_GUEST_MASK bits. */
    UINT32 fFeatures;
} VBOXGUEST98MOUSESTATUS;

/** IOCTL_VBOXGUEST98_HGCM_CONNECT input/output. */
typedef struct VBOXGUEST98HGCMCONNECT
{
    /** IN: Service name, zero terminated. */
    char   szServiceName[128];
    /** OUT: Client ID. */
    UINT32 u32ClientID;
    /** OUT: VBox status code. */
    INT32  rc;
} VBOXGUEST98HGCMCONNECT;

/** IOCTL_VBOXGUEST98_HGCM_DISCONNECT input/output. */
typedef struct VBOXGUEST98HGCMDISCONNECT
{
    /** IN: Client ID. */
    UINT32 u32ClientID;
    /** OUT: VBox status code. */
    INT32  rc;
} VBOXGUEST98HGCMDISCONNECT;

/** @name VBOXGUEST98HGCMPARM::enmType
 * @{ */
#define VBOXGUEST98_HGCM_PARM_32BIT         1
#define VBOXGUEST98_HGCM_PARM_64BIT         2
#define VBOXGUEST98_HGCM_PARM_BUFFER_INOUT  4
#define VBOXGUEST98_HGCM_PARM_BUFFER_IN     5
#define VBOXGUEST98_HGCM_PARM_BUFFER_OUT    6
/** @} */

/** HGCM call parameter. */
typedef struct VBOXGUEST98HGCMPARM
{
    /** VBOXGUEST98_HGCM_PARM_XXX. */
    UINT32 enmType;
    /** Buffers: size in bytes (updated on return). */
    UINT32 cb;
    /** Buffers: offset of the data relative to the start of the
     *  VBOXGUEST98HGCMCALL structure. */
    UINT32 off;
    UINT32 u32Reserved;
    /** 32/64-bit values (updated on return). */
    UINT64 u64Value;
} VBOXGUEST98HGCMPARM;

/** IOCTL_VBOXGUEST98_HGCM_CALL input/output header. */
typedef struct VBOXGUEST98HGCMCALL
{
    /** IN: Client ID. */
    UINT32 u32ClientID;
    /** IN: Service function number. */
    UINT32 u32Function;
    /** IN: Timeout in milliseconds or VBOXGUEST98_WAIT_INFINITE. */
    UINT32 cMsTimeout;
    /** IN: Number of VBOXGUEST98HGCMPARM entries following this header. */
    UINT32 cParms;
    /** OUT: VBox status code of the call (host result if the request itself
     *  succeeded). */
    INT32  rc;
} VBOXGUEST98HGCMCALL;

/** Returns the parameter array of a VBOXGUEST98HGCMCALL. */
#define VBOXGUEST98_HGCM_CALL_PARMS(pCall) ((VBOXGUEST98HGCMPARM *)((pCall) + 1))

/** Mouse position notification callback, called at DISPATCH_LEVEL with the
 *  current host mouse status. */
typedef VOID (__stdcall *PFNVBOXGUEST98MOUSENOTIFY)(PVOID pvUser, UINT32 fFeatures, INT32 x, INT32 y);

/** IOCTL_VBOXGUEST98_INTERNAL_SET_MOUSE_NOTIFY input. */
typedef struct VBOXGUEST98MOUSENOTIFY
{
    PFNVBOXGUEST98MOUSENOTIFY pfnNotify;
    PVOID pvUser;
} VBOXGUEST98MOUSENOTIFY;

#pragma pack(pop)

#endif /* !RC_INVOKED */

#endif /* !VBOXGUEST98_H */
