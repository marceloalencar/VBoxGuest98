/** @file
 * VBoxTray - Guest Additions tray application for Windows 98/ME, internal header.
 *
 * Layout:
 *  - VBoxTray.c   WinMain, hidden window, tray icon, service threads.
 *  - Mouse.c      Mouse pointer integration (absolute positioning).
 *  - TimeSync.c   Guest clock synchronization with the host.
 *  - Clipboard.c  Shared clipboard (text).
 *  - Stats.c      Guest statistics (memory, processes, CPU load).
 *  - Display.c    Resizing the guest display to the host window.
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
int    VbglR3ChangeFilterMask(HANDLE hDevice, UINT32 fOrMask, UINT32 fNotMask);
int    VbglR3WaitForEvents(HANDLE hDevice, UINT32 fMask, UINT32 cMsTimeout, UINT32 *pfEvents);
int    VbglR3SetMouseStatus(HANDLE hDevice, UINT32 fFeatures);
int    VbglR3GetMouseStatus(HANDLE hDevice, UINT32 *pfFeatures, INT32 *px, INT32 *py);
int    VbglR3GetHostTime(HANDLE hDevice, UINT64 *pmsUnixEpoch);
int    VbglR3ReportFacilityStatus(HANDLE hDevice, VBoxGuestFacilityType enmFacility, VBoxGuestFacilityStatus enmStatus);
int    VbglR3HgcmConnect(HANDLE hDevice, const char *pszService, UINT32 *pidClient);
int    VbglR3HgcmDisconnect(HANDLE hDevice, UINT32 idClient);
int    VbglR3HgcmCall(HANDLE hDevice, VBOXGUEST98HGCMCALL *pCall, DWORD cbCall);
int    VbglR3StatQueryInterval(HANDLE hDevice, DWORD *pcMsInterval);
int    VbglR3StatReport(HANDLE hDevice, const VBoxGuestStatistics *pStats);
int    VbglR3ChangeGuestCapabilities(HANDLE hDevice, UINT32 fOrMask, UINT32 fNotMask);
int    VbglR3GetDisplayChangeRequest(HANDLE hDevice, UINT32 *pcx, UINT32 *pcy, UINT32 *pcBits, UINT32 *piDisplay,
                                     BOOL *pfEnabled, BOOL fAck);


/*********************************************************************************************************************************
*   Services                                                                                                                     *
*********************************************************************************************************************************/

/** Set when the application is shutting down; the service threads poll it. */
extern volatile LONG g_fVBoxTrayShutdown;
/** The hidden VBoxTray window (created before the services start). */
extern HWND g_hwndVBoxTray;

unsigned __stdcall VBoxMouseThread(void *pvUser);
unsigned __stdcall VBoxTimeSyncThread(void *pvUser);
unsigned __stdcall VBoxStatsThread(void *pvUser);
unsigned __stdcall VBoxDisplayThread(void *pvUser);

void     VBoxClipboardInit(void);
void     VBoxClipboardStop(void);
unsigned __stdcall VBoxClipboardThread(void *pvUser);
BOOL     VBoxClipboardWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT *plResult);

#endif /* !VBOXTRAY_H */
