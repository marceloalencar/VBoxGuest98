/** @file
 * VBoxTray - Guest Additions tray application for Windows 98/ME, internal header.
 *
 * Layout:
 *  - VBoxTray.c   WinMain, hidden window, tray icon, service threads.
 *  - Mouse.c      Mouse pointer integration (absolute positioning).
 *  - TimeSync.c   Guest clock synchronization with the host.
 *  - Clipboard.c  Shared clipboard (text, images, HTML).
 *  - Stats.c      Guest statistics (memory, processes, CPU load).
 *  - Display.c    Resizing the guest display to the host window.
 *  - Seamless.c   Seamless mode (visible region of the guest windows).
 *  - DnD.c        Drag and drop between the host and the guest (DnDHgcm.c:
 *                 its HGCM calls, DnDTransfer.c: transfers, DnDUtil.c: text
 *                 conversions, DnDObj.c: OLE objects).
 *  - GstCtrl.c    Guest control: programs started from the host (GstCtrlFile.c:
 *                 files, directories and file systems).
 *  - VMInfo.c     Guest properties with the OS, user and network information
 *                 (GuestProp.c: the VBoxGuestPropSvc calls).
 *  - VbglR3.c     Access to the VBoxGst driver (VBoxGuest98.h interface).
 */

#ifndef VBOXTRAY_H
#define VBOXTRAY_H

#include <windows.h>
#include <winioctl.h>

#include "VBoxGuest98.h"


/*********************************************************************************************************************************
*   Driver access (VbglR3.c)                                                                                                     *
*********************************************************************************************************************************/

HANDLE VbglR3Open(void);
HANDLE VbglR3OpenEx(BOOL fOverlapped);
void   VbglR3Close(HANDLE hDevice);
int    VbglR3WriteLog(HANDLE hDevice, const char *pch, DWORD cch);
int    VbglR3ChangeFilterMask(HANDLE hDevice, UINT32 fOrMask, UINT32 fNotMask);
int    VbglR3WaitForEvents(HANDLE hDevice, UINT32 fMask, UINT32 cMsTimeout, UINT32 *pfEvents);
int    VbglR3SetMouseStatus(HANDLE hDevice, UINT32 fFeatures);
int    VbglR3GetMouseStatus(HANDLE hDevice, UINT32 *pfFeatures, INT32 *px, INT32 *py);
int    VbglR3GetHostTime(HANDLE hDevice, UINT64 *pmsUnixEpoch);
int    VbglR3QuerySessionId(HANDLE hDevice, UINT64 *pidSession);
int    VbglR3ReportFacilityStatus(HANDLE hDevice, VBoxGuestFacilityType enmFacility, VBoxGuestFacilityStatus enmStatus);
int    VbglR3HgcmConnect(HANDLE hDevice, const char *pszService, UINT32 *pidClient);
int    VbglR3HgcmDisconnect(HANDLE hDevice, UINT32 idClient);
int    VbglR3HgcmCall(HANDLE hDevice, VBOXGUEST98HGCMCALL *pCall, DWORD cbCall);

/** HGCM call being built: header, parameters, then the buffers. */
typedef struct VBGLR3HGCMMSG
{
    VBOXGUEST98HGCMCALL *pCall;
    DWORD                cbCall;
    /** Offset of the next unused data byte. */
    DWORD                offNext;
} VBGLR3HGCMMSG;

BOOL   VbglR3HgcmMsgInit(VBGLR3HGCMMSG *pMsg, UINT32 idClient, UINT32 uFunction, UINT32 cParms, UINT32 cMsTimeout,
                         DWORD cbData);
void   VbglR3HgcmMsgSetU32(VBGLR3HGCMMSG *pMsg, UINT32 iParm, UINT32 u32);
void   VbglR3HgcmMsgSetU64(VBGLR3HGCMMSG *pMsg, UINT32 iParm, UINT64 u64);
BYTE  *VbglR3HgcmMsgSetBuf(VBGLR3HGCMMSG *pMsg, UINT32 iParm, UINT32 enmType, DWORD cb, const void *pvIn);
UINT32 VbglR3HgcmMsgGetU32(VBGLR3HGCMMSG *pMsg, UINT32 iParm);
UINT64 VbglR3HgcmMsgGetU64(VBGLR3HGCMMSG *pMsg, UINT32 iParm);
UINT32 VbglR3HgcmMsgGetBufSize(VBGLR3HGCMMSG *pMsg, UINT32 iParm);
int    VbglR3HgcmMsgCall(HANDLE hDevice, VBGLR3HGCMMSG *pMsg);
void   VbglR3HgcmMsgFree(VBGLR3HGCMMSG *pMsg);
int    VbglR3StatQueryInterval(HANDLE hDevice, DWORD *pcMsInterval);
int    VbglR3StatReport(HANDLE hDevice, const VBoxGuestStatistics *pStats);
int    VbglR3ChangeGuestCapabilities(HANDLE hDevice, UINT32 fOrMask, UINT32 fNotMask);
int    VbglR3GetDisplayChangeRequest(HANDLE hDevice, UINT32 *pcx, UINT32 *pcy, UINT32 *pcBits, UINT32 *piDisplay,
                                     BOOL *pfEnabled, BOOL fAck);
int    VbglR3SeamlessGetMode(HANDLE hDevice, UINT32 *penmMode, BOOL fAck);
int    VbglR3SeamlessSendRects(HANDLE hDevice, const RTRECT *paRects, UINT32 cRects);


/*********************************************************************************************************************************
*   Services                                                                                                                     *
*********************************************************************************************************************************/

/** Set when the application is shutting down; the service threads poll it. */
extern volatile LONG g_fVBoxTrayShutdown;
/** The hidden VBoxTray window (created before the services start). */
extern HWND g_hwndVBoxTray;

/** Writes a formatted line (wsprintf format, ending with a newline) to the
 *  release log, prefixed with "VBoxTray: ".  Any thread may call it. */
void __cdecl VBoxTrayLogRel(const char *pszFormat, ...);
/** Converts ANSI code page text to zero terminated UTF-8 (malloc'ed), NULL on failure. */
char *VBoxTrayAnsiToUtf8(const char *psz);
/** Converts cch bytes of UTF-8 to zero terminated ANSI code page text (malloc'ed), NULL if out of
 *  memory; characters the code page doesn't have become chDefault. */
char *VBoxTrayUtf8ToAnsi(const char *pch, DWORD cch, char chDefault);

unsigned __stdcall VBoxMouseThread(void *pvUser);
unsigned __stdcall VBoxTimeSyncThread(void *pvUser);
unsigned __stdcall VBoxStatsThread(void *pvUser);
unsigned __stdcall VBoxDisplayThread(void *pvUser);
unsigned __stdcall VBoxSeamlessThread(void *pvUser);

void     VBoxClipboardInit(void);
void     VBoxClipboardStop(void);
unsigned __stdcall VBoxClipboardThread(void *pvUser);
BOOL     VBoxClipboardWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT *plResult);

void     VBoxDnDInit(void);
void     VBoxDnDStop(void);
unsigned __stdcall VBoxDnDThread(void *pvUser);

void     VBoxVMInfoInit(void);
void     VBoxVMInfoStop(void);
unsigned __stdcall VBoxVMInfoThread(void *pvUser);

void     VBoxGstCtrlInit(void);
void     VBoxGstCtrlStop(void);
unsigned __stdcall VBoxGstCtrlThread(void *pvUser);


/*********************************************************************************************************************************
*   Guest properties (GuestProp.c)                                                                                               *
*********************************************************************************************************************************/

int VBoxGuestPropWrite(HANDLE hDevice, UINT32 idClient, const char *pszName, const char *pszValue, const char *pszFlags);
int VBoxGuestPropRead(HANDLE hDevice, UINT32 idClient, const char *pszName, char *pszValue, DWORD cbValue);

#endif /* !VBOXTRAY_H */
