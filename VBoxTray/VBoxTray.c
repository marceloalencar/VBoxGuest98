/** @file
 * VBoxTray - Guest Additions tray application for Windows 98/ME.
 *
 * Runs the service threads (mouse integration, time synchronization, shared
 * clipboard, guest statistics, display resizing, drag and drop, guest
 * properties, guest control, shared folder auto-mount, automatic logon) and
 * shows a tray icon with an Exit command.  Started before logon through the
 * RunServices registry key (see VBoxGst.inf), so the mouse and the other
 * services already work at the logon dialog; as a service process
 * (RegisterServiceProcess) it outlives logoffs and only stops when Windows
 * shuts down.  There is no taskbar before logon: the icon is added
 * when Explorer announces it (TaskbarCreated).
 */

#include "VBoxTray.h"
#include "VBoxTrayRes.h"
#include <shellapi.h>
#include <ole2.h>
#include <process.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/

/** Name of the mutex making sure only one instance runs. */
#define VBOXTRAY_MUTEX_NAME         "VBoxTray98SingleInstance"
/** Window class of the hidden window. */
#define VBOXTRAY_WND_CLASS          "VBoxTrayToolWndClass"
/** Tray icon notification message. */
#define WM_VBOXTRAY_NOTIFYICON      (WM_APP + 1)
/** Tray icon identifier. */
#define VBOXTRAY_ICON_ID            1
/** Exit menu command. */
#define IDM_VBOXTRAY_EXIT           100
/** How long to wait for the service threads on exit (ms). */
#define VBOXTRAY_THREAD_EXIT_MS     5000
/** How long to wait for the driver at startup (ms): on the boot that
 *  installs the device Windows starts it after the RunServices programs. */
#define VBOXTRAY_DRIVER_WAIT_MS     120000
/** How often to try opening the driver meanwhile (ms). */
#define VBOXTRAY_DRIVER_RETRY_MS    1000
/** RegisterServiceProcess (KERNEL32, Windows 9x only): keeps the process
 *  running across logoffs. */
#define VBOXTRAY_RSP_SIMPLE_SERVICE 1

/* Windows 98 (shell32 4.72) only accepts the original NOTIFYICONDATA. */
#ifndef NOTIFYICONDATA_V1_SIZE
# define NOTIFYICONDATA_V1_SIZE     sizeof(NOTIFYICONDATA)
#endif


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/

/** A service run in its own thread. */
typedef struct VBOXTRAYSERVICE
{
    const char *pszName;
    /** Called before any thread starts (optional). */
    void (*pfnInit)(void);
    unsigned (__stdcall *pfnThread)(void *pvUser);
    /** Wakes the thread up on shutdown, if polling g_fVBoxTrayShutdown isn't enough (optional). */
    void (*pfnStop)(void);
    HANDLE hThread;
} VBOXTRAYSERVICE;


/*********************************************************************************************************************************
*   Global Variables                                                                                                             *
*********************************************************************************************************************************/

volatile LONG g_fVBoxTrayShutdown = FALSE;
HWND g_hwndVBoxTray = NULL;

static VBOXTRAYSERVICE g_aServices[] =
{
    { "Mouse",     NULL,              VBoxMouseThread,     NULL,              NULL },
    { "TimeSync",  NULL,              VBoxTimeSyncThread,  NULL,              NULL },
    { "Clipboard", VBoxClipboardInit, VBoxClipboardThread, VBoxClipboardStop, NULL },
    { "Stats",     NULL,              VBoxStatsThread,     NULL,              NULL },
    { "Display",   NULL,              VBoxDisplayThread,   NULL,              NULL },
    { "Seamless",  NULL,              VBoxSeamlessThread,  NULL,              NULL },
    { "DnD",       VBoxDnDInit,       VBoxDnDThread,       VBoxDnDStop,       NULL },
    { "VMInfo",    VBoxVMInfoInit,    VBoxVMInfoThread,    VBoxVMInfoStop,    NULL },
    { "GstCtrl",   VBoxGstCtrlInit,   VBoxGstCtrlThread,   VBoxGstCtrlStop,   NULL },
    { "AutoMount", VBoxAutoMountInit, VBoxAutoMountThread, VBoxAutoMountStop, NULL },
    { "AutoLogon", NULL,              VBoxAutoLogonThread, NULL,              NULL },
};

static HINSTANCE g_hInstance;
/** The small (tray) icon. */
static HICON g_hIconSmall;
/** "TaskbarCreated" message, sent when Explorer restarts. */
static UINT g_uMsgTaskbarCreated;
/** Device handle for logging, shared by all threads (the writes don't block). */
static HANDLE g_hVBoxTrayLog = INVALID_HANDLE_VALUE;


/**
 * Writes a formatted line to the release log (see VBoxTray.h).
 */
void __cdecl VBoxTrayLogRel(const char *pszFormat, ...)
{
    /* wvsprintf writes at most 1024 characters. */
    static const char s_szPrefix[] = "VBoxTray: ";
    char szLine[sizeof(s_szPrefix) + 1024];
    va_list va;
    int cch;

    if (g_hVBoxTrayLog == INVALID_HANDLE_VALUE)
        return;

    memcpy(szLine, s_szPrefix, sizeof(s_szPrefix) - 1);
    va_start(va, pszFormat);
    cch = wvsprintf(&szLine[sizeof(s_szPrefix) - 1], pszFormat, va);
    va_end(va);
    if (cch > 0)
        VbglR3WriteLog(g_hVBoxTrayLog, szLine, sizeof(s_szPrefix) - 1 + cch);
}


/**
 * Adds or removes the tray icon.
 */
static void vboxTrayIcon(HWND hwnd, DWORD dwMessage)
{
    NOTIFYICONDATA Nid;

    memset(&Nid, 0, sizeof(Nid));
    Nid.cbSize           = NOTIFYICONDATA_V1_SIZE;
    Nid.hWnd             = hwnd;
    Nid.uID              = VBOXTRAY_ICON_ID;
    Nid.uFlags           = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    Nid.uCallbackMessage = WM_VBOXTRAY_NOTIFYICON;
    Nid.hIcon            = g_hIconSmall;
    lstrcpyn(Nid.szTip, "VBoxGuest98 " VBOXGUEST98_VERSION_STRING, 64);
    Shell_NotifyIcon(dwMessage, &Nid);
}

/**
 * Shows the tray icon context menu.
 */
static void vboxTrayShowMenu(HWND hwnd)
{
    POINT pt;
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu)
        return;

    AppendMenu(hMenu, MF_STRING, IDM_VBOXTRAY_EXIT, "E&xit");
    GetCursorPos(&pt);
    /* Needed for the menu to close when clicking elsewhere (KB135788). */
    SetForegroundWindow(hwnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    PostMessage(hwnd, WM_NULL, 0, 0);
    DestroyMenu(hMenu);
}

/**
 * Window procedure of the hidden window.
 */
static LRESULT CALLBACK vboxTrayWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    LRESULT lResult;

    if (VBoxClipboardWndProc(hwnd, uMsg, wParam, lParam, &lResult))
        return lResult;

    switch (uMsg)
    {
        case WM_CREATE:
            vboxTrayIcon(hwnd, NIM_ADD);
            return 0;

        case WM_VBOXTRAY_NOTIFYICON:
            if (lParam == WM_RBUTTONUP)
                vboxTrayShowMenu(hwnd);
            return 0;

        case WM_COMMAND:
            if (LOWORD(wParam) == IDM_VBOXTRAY_EXIT)
                DestroyWindow(hwnd);
            return 0;

        case WM_ENDSESSION:
            /* A logoff ends the session of the user only; the services stay. */
            if (wParam && (lParam & ENDSESSION_LOGOFF))
                VBoxTrayLogRel("the user logs off\n");
            else if (wParam)
                DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            vboxTrayIcon(hwnd, NIM_DELETE);
            PostQuitMessage(0);
            return 0;

        default:
            if (uMsg == g_uMsgTaskbarCreated && g_uMsgTaskbarCreated)
            {
                vboxTrayIcon(hwnd, NIM_ADD);
                return 0;
            }
            return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
}

/**
 * Initializes all services, before the window is created.
 */
static void vboxTrayInitServices(void)
{
    unsigned i;
    for (i = 0; i < sizeof(g_aServices) / sizeof(g_aServices[0]); i++)
        if (g_aServices[i].pfnInit)
            g_aServices[i].pfnInit();
}

/**
 * Starts all service threads.
 */
static void vboxTrayStartServices(void)
{
    unsigned i;
    for (i = 0; i < sizeof(g_aServices) / sizeof(g_aServices[0]); i++)
    {
        unsigned idThread;
        g_aServices[i].hThread = (HANDLE)_beginthreadex(NULL, 0, g_aServices[i].pfnThread, NULL, 0, &idThread);
        if (!g_aServices[i].hThread)
            VBoxTrayLogRel("%s: thread creation failed\n", g_aServices[i].pszName);
    }
}

/**
 * Tells the service threads to stop and waits for them.
 */
static void vboxTrayStopServices(void)
{
    HANDLE ahThreads[sizeof(g_aServices) / sizeof(g_aServices[0])];
    DWORD cThreads = 0;
    unsigned i;

    InterlockedExchange((LPLONG)&g_fVBoxTrayShutdown, TRUE);

    for (i = 0; i < sizeof(g_aServices) / sizeof(g_aServices[0]); i++)
        if (g_aServices[i].hThread)
        {
            if (g_aServices[i].pfnStop)
                g_aServices[i].pfnStop();
            ahThreads[cThreads++] = g_aServices[i].hThread;
        }
    if (cThreads)
        WaitForMultipleObjects(cThreads, ahThreads, TRUE, VBOXTRAY_THREAD_EXIT_MS);

    for (i = 0; i < sizeof(g_aServices) / sizeof(g_aServices[0]); i++)
        if (g_aServices[i].hThread)
        {
            DWORD dwExit = 0;
            if (!GetExitCodeThread(g_aServices[i].hThread, &dwExit) || dwExit == STILL_ACTIVE)
                VBoxTrayLogRel("%s: thread did not stop\n", g_aServices[i].pszName);
            CloseHandle(g_aServices[i].hThread);
            g_aServices[i].hThread = NULL;
        }
}

/**
 * Opens the driver, waiting for it when Windows hasn't started it yet.
 *
 * @returns Device handle, INVALID_HANDLE_VALUE when it never came.
 */
static HANDLE vboxTrayWaitForDriver(void)
{
    DWORD dwStart = GetTickCount();
    for (;;)
    {
        HANDLE hDevice = VbglR3Open();
        if (hDevice != INVALID_HANDLE_VALUE || GetTickCount() - dwStart >= VBOXTRAY_DRIVER_WAIT_MS)
            return hDevice;
        Sleep(VBOXTRAY_DRIVER_RETRY_MS);
    }
}

/**
 * Makes this a service process, which Windows 9x doesn't end at logoff.
 */
static void vboxTrayRegisterServiceProcess(void)
{
    typedef DWORD (WINAPI *PFNREGISTERSERVICEPROCESS)(DWORD idProcess, DWORD dwType);
    PFNREGISTERSERVICEPROCESS pfnRegisterServiceProcess;

    pfnRegisterServiceProcess = (PFNREGISTERSERVICEPROCESS)GetProcAddress(GetModuleHandle("KERNEL32.DLL"),
                                                                          "RegisterServiceProcess");
    if (!pfnRegisterServiceProcess || !pfnRegisterServiceProcess(0, VBOXTRAY_RSP_SIMPLE_SERVICE))
        VBoxTrayLogRel("RegisterServiceProcess failed, VBoxTray will stop at logoff\n");
}

/**
 * Reports the VBoxTray facility status to the host.
 */
static void vboxTrayReportStatus(VBoxGuestFacilityStatus enmStatus)
{
    HANDLE hDevice = VbglR3Open();
    if (hDevice != INVALID_HANDLE_VALUE)
    {
        VbglR3ReportFacilityStatus(hDevice, VBoxGuestFacilityType_VBoxTrayClient, enmStatus);
        VbglR3Close(hDevice);
    }
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR pszCmdLine, int nCmdShow)
{
    WNDCLASS WndCls;
    HANDLE hMutex;
    HANDLE hDevice;
    HWND hwnd;
    MSG Msg;
    HRESULT hrOle;

    g_hInstance = hInstance;

    hMutex = CreateMutex(NULL, FALSE, VBOXTRAY_MUTEX_NAME);
    if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(hMutex);
        return 0;
    }

    /* Nothing to do without the driver (not running in VirtualBox?). */
    hDevice = vboxTrayWaitForDriver();
    if (hDevice == INVALID_HANDLE_VALUE)
    {
        if (hMutex)
            CloseHandle(hMutex);
        return 1;
    }
    /* Keep this one open for logging. */
    g_hVBoxTrayLog = hDevice;
    VBoxTrayLogRel("VBoxTray %s (VirtualBox %s r%u) started\n", VBOXGUEST98_VERSION_STRING,
                   VBOX_VERSION_STRING, VBOX_SVN_REV);
    vboxTrayRegisterServiceProcess();

    g_uMsgTaskbarCreated = RegisterWindowMessage("TaskbarCreated");

    /* The tray wants the 16x16 image; LoadIcon would scale down the 32x32 one. */
    g_hIconSmall = (HICON)LoadImage(hInstance, MAKEINTRESOURCE(IDI_VBOXTRAY), IMAGE_ICON,
                                    GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (!g_hIconSmall)
        g_hIconSmall = LoadIcon(NULL, IDI_APPLICATION);

    memset(&WndCls, 0, sizeof(WndCls));
    WndCls.lpfnWndProc   = vboxTrayWndProc;
    WndCls.hInstance     = hInstance;
    WndCls.hIcon         = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_VBOXTRAY));
    WndCls.hCursor       = LoadCursor(NULL, IDC_ARROW);
    WndCls.lpszClassName = VBOXTRAY_WND_CLASS;
    if (!RegisterClass(&WndCls))
    {
        VBoxTrayLogRel("RegisterClass failed, error %lu\n", GetLastError());
        VbglR3Close(g_hVBoxTrayLog);
        if (hMutex)
            CloseHandle(hMutex);
        return 1;
    }

    /* The clipboard puts host files on the clipboard as an OLE data object,
       served by this thread. */
    hrOle = OleInitialize(NULL);
    if (FAILED(hrOle))
        VBoxTrayLogRel("OleInitialize failed, hr=%#lx\n", hrOle);

    vboxTrayReportStatus(VBoxGuestFacilityStatus_Init);
    vboxTrayInitServices();

    /* The services talk to the window, so create it first. */
    hwnd = CreateWindowEx(WS_EX_TOOLWINDOW, VBOXTRAY_WND_CLASS, "VBoxTray", WS_POPUP,
                          0, 0, 0, 0, NULL, NULL, hInstance, NULL);
    if (hwnd)
    {
        g_hwndVBoxTray = hwnd;
        vboxTrayStartServices();
        vboxTrayReportStatus(VBoxGuestFacilityStatus_Active);
        while (GetMessage(&Msg, NULL, 0, 0) > 0)
        {
            TranslateMessage(&Msg);
            DispatchMessage(&Msg);
        }
    }

    vboxTrayReportStatus(VBoxGuestFacilityStatus_Terminating);
    vboxTrayStopServices();
    vboxTrayReportStatus(VBoxGuestFacilityStatus_Terminated);
    if (SUCCEEDED(hrOle))
        OleUninitialize();

    VBoxTrayLogRel("stopped\n");
    hDevice = g_hVBoxTrayLog;
    g_hVBoxTrayLog = INVALID_HANDLE_VALUE;
    VbglR3Close(hDevice);

    UnregisterClass(VBOXTRAY_WND_CLASS, hInstance);
    DestroyIcon(g_hIconSmall); /* No-op for the shared fallback icon. */
    if (hMutex)
        CloseHandle(hMutex);
    return 0;
}
