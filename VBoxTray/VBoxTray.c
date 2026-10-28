/** @file
 * VBoxTray - Guest Additions tray application for Windows 98/ME.
 *
 * Runs the service threads (mouse integration, time synchronization, shared
 * clipboard, guest statistics, display resizing, drag and drop, guest
 * properties) and shows a tray icon with an Exit command.  Started at logon
 * through the Run registry key (see VBoxGst.inf).
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
 * Converts UTF-8 to UTF-16 (invalid sequences become U+FFFD; characters
 * outside the BMP become surrogate pairs).
 *
 * @returns Zero terminated text (malloc'ed), NULL if out of memory.
 */
static WCHAR *vboxTrayUtf8ToUtf16(const char *pch, DWORD cch)
{
    const BYTE *pb = (const BYTE *)pch;
    WCHAR *pwszRet = (WCHAR *)malloc((cch + 1) * sizeof(WCHAR));
    DWORD off = 0;
    DWORD cwc = 0;

    if (!pwszRet)
        return NULL;
    while (off < cch)
    {
        BYTE b = pb[off++];
        DWORD uc;
        DWORD cTrail;
        DWORD ucMin;

        if (b < 0x80)
        {
            pwszRet[cwc++] = b;
            continue;
        }
        if ((b & 0xe0) == 0xc0)
        {
            uc = b & 0x1f;
            cTrail = 1;
            ucMin = 0x80;
        }
        else if ((b & 0xf0) == 0xe0)
        {
            uc = b & 0x0f;
            cTrail = 2;
            ucMin = 0x800;
        }
        else if ((b & 0xf8) == 0xf0)
        {
            uc = b & 0x07;
            cTrail = 3;
            ucMin = 0x10000;
        }
        else
        {
            pwszRet[cwc++] = 0xfffd;
            continue;
        }
        while (cTrail && off < cch && (pb[off] & 0xc0) == 0x80)
        {
            uc = (uc << 6) | (pb[off++] & 0x3f);
            cTrail--;
        }
        if (cTrail || uc < ucMin || uc > 0x10ffff || (uc >= 0xd800 && uc <= 0xdfff))
            pwszRet[cwc++] = 0xfffd;
        else if (uc >= 0x10000)
        {
            /* Room: a 4 byte sequence makes 2 UTF-16 units. */
            uc -= 0x10000;
            pwszRet[cwc++] = (WCHAR)(0xd800 + (uc >> 10));
            pwszRet[cwc++] = (WCHAR)(0xdc00 + (uc & 0x3ff));
        }
        else
            pwszRet[cwc++] = (WCHAR)uc;
    }
    pwszRet[cwc] = 0;
    return pwszRet;
}

/**
 * Converts UTF-8 to the ANSI code page.
 *
 * @returns Zero terminated text (malloc'ed), NULL if out of memory.
 * @param   chDefault   Replaces characters the code page doesn't have.
 */
char *VBoxTrayUtf8ToAnsi(const char *pch, DWORD cch, char chDefault)
{
    char szDefault[2];
    WCHAR *pwsz;
    char *pszRet = NULL;
    int cb;

    szDefault[0] = chDefault;
    szDefault[1] = '\0';
    pwsz = vboxTrayUtf8ToUtf16(pch, cch);
    if (!pwsz)
        return NULL;
    cb = WideCharToMultiByte(CP_ACP, 0, pwsz, -1, NULL, 0, szDefault, NULL);
    if (cb > 0)
    {
        pszRet = (char *)malloc(cb);
        if (pszRet && !WideCharToMultiByte(CP_ACP, 0, pwsz, -1, pszRet, cb, szDefault, NULL))
        {
            free(pszRet);
            pszRet = NULL;
        }
    }
    free(pwsz);
    return pszRet;
}

/**
 * Converts text in the ANSI code page to UTF-8.
 *
 * @returns Zero terminated UTF-8 (malloc'ed), NULL on failure.
 */
char *VBoxTrayAnsiToUtf8(const char *psz)
{
    WCHAR *pwsz;
    BYTE *pbRet;
    DWORD off = 0;
    int cwc;
    int i;

    cwc = MultiByteToWideChar(CP_ACP, 0, psz, -1, NULL, 0);
    if (cwc <= 0)
        return NULL;
    pwsz = (WCHAR *)malloc(cwc * sizeof(WCHAR));
    pbRet = (BYTE *)malloc(cwc * 3); /* Up to 3 bytes per UTF-16 unit, 4 per surrogate pair. */
    if (!pwsz || !pbRet || !MultiByteToWideChar(CP_ACP, 0, psz, -1, pwsz, cwc))
    {
        free(pwsz);
        free(pbRet);
        return NULL;
    }

    for (i = 0; i < cwc; i++)
    {
        DWORD uc = pwsz[i];
        if (   uc >= 0xd800 && uc <= 0xdbff
            && i + 1 < cwc && pwsz[i + 1] >= 0xdc00 && pwsz[i + 1] <= 0xdfff)
        {
            uc = 0x10000 + ((uc - 0xd800) << 10) + (pwsz[i + 1] - 0xdc00);
            i++;
        }
        if (uc < 0x80)
            pbRet[off++] = (BYTE)uc;
        else if (uc < 0x800)
        {
            pbRet[off++] = (BYTE)(0xc0 | (uc >> 6));
            pbRet[off++] = (BYTE)(0x80 | (uc & 0x3f));
        }
        else if (uc < 0x10000)
        {
            pbRet[off++] = (BYTE)(0xe0 | (uc >> 12));
            pbRet[off++] = (BYTE)(0x80 | ((uc >> 6) & 0x3f));
            pbRet[off++] = (BYTE)(0x80 | (uc & 0x3f));
        }
        else
        {
            pbRet[off++] = (BYTE)(0xf0 | (uc >> 18));
            pbRet[off++] = (BYTE)(0x80 | ((uc >> 12) & 0x3f));
            pbRet[off++] = (BYTE)(0x80 | ((uc >> 6) & 0x3f));
            pbRet[off++] = (BYTE)(0x80 | (uc & 0x3f));
        }
    }
    free(pwsz);
    return (char *)pbRet; /* The terminator was converted too. */
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
            if (wParam)
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
    hDevice = VbglR3Open();
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
