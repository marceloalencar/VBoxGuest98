/** @file
 * VBoxTray - Automatic logon with credentials from the host.
 *
 * `VBoxManage controlvm <vm> setcredentials` stores logon credentials in the
 * VMMDev; the official Additions log on with them through VBoxGINA (which
 * fills the MSGINA dialog) or VBoxCredProv.  Windows 98 has neither, but
 * VBoxTray already runs at the logon dialog (RunServices), and nothing keeps
 * one process from filling another's dialog there.  So while a logon dialog
 * is up the host is asked for credentials; when it has some they are read
 * and cleared (they are used once), typed into the dialog and OK is pressed.
 * The host clears them on every reset too, so they are set while the guest
 * is at, or on its way to, the logon dialog.
 *
 * The logon dialogs are MPREXE.EXE's: "Windows Logon" (user and password)
 * and the Microsoft Network one (user, password and, with a domain logon,
 * domain).  The latter is MPR's network password template, which programs
 * also show to ask for the password of a resource, in their own process.
 */

#include "VBoxTray.h"
#include <tlhelp32.h>
#include <stdlib.h>
#include <string.h>


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/

/** How often to look for a logon dialog and ask the host (ms). */
#define VBOXAUTOLOGON_POLL_MS       500
/** The process that shows the logon dialogs. */
#define VBOXAUTOLOGON_MPREXE        "MPREXE.EXE"

/** Control IDs of the "Windows Logon" dialog. */
#define VBOXAUTOLOGON_WIN_USER      30
#define VBOXAUTOLOGON_WIN_PASSWORD  31
/** Control IDs of the Microsoft Network logon dialog. */
#define VBOXAUTOLOGON_NET_USER      21
#define VBOXAUTOLOGON_NET_PASSWORD  23
#define VBOXAUTOLOGON_NET_DOMAIN    25


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/

/** A logon dialog found on the screen. */
typedef struct VBOXAUTOLOGONDLG
{
    HWND    hwnd;
    HWND    hwndUser;
    HWND    hwndPassword;
    /** NULL when the dialog has no (visible) domain field. */
    HWND    hwndDomain;
} VBOXAUTOLOGONDLG;


/**
 * Returns the control if it is a visible edit control, password style or not
 * as asked.
 */
static HWND vboxAutoLogonEdit(HWND hwndDlg, int idCtrl, BOOL fPassword)
{
    char szClass[16];
    HWND hwnd = GetDlgItem(hwndDlg, idCtrl);

    if (   !hwnd
        || !IsWindowVisible(hwnd)
        || !GetClassName(hwnd, szClass, sizeof(szClass))
        || lstrcmpi(szClass, "Edit"))
        return NULL;
    if (!(GetWindowLong(hwnd, GWL_STYLE) & ES_PASSWORD) != !fPassword)
        return NULL;
    return hwnd;
}

/**
 * Checks whether the process is MPREXE.EXE.
 */
static BOOL vboxAutoLogonIsMprExe(DWORD idProcess)
{
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 Entry;
    BOOL fRet = FALSE;

    if (hSnap == INVALID_HANDLE_VALUE)
        return FALSE;
    Entry.dwSize = sizeof(Entry);
    if (Process32First(hSnap, &Entry))
        do
        {
            if (Entry.th32ProcessID == idProcess)
            {
                /* szExeFile is the full path on Windows 9x. */
                const char *pszName = strrchr(Entry.szExeFile, '\\');
                fRet = !lstrcmpi(pszName ? pszName + 1 : Entry.szExeFile, VBOXAUTOLOGON_MPREXE);
                break;
            }
        } while (Process32Next(hSnap, &Entry));
    CloseHandle(hSnap);
    return fRet;
}

/**
 * EnumWindows callback looking for a logon dialog.
 */
static BOOL CALLBACK vboxAutoLogonEnumProc(HWND hwnd, LPARAM lParam)
{
    VBOXAUTOLOGONDLG *pDlg = (VBOXAUTOLOGONDLG *)lParam;
    char szClass[16];
    DWORD idProcess = 0;
    HWND hwndUser;
    HWND hwndPassword;
    HWND hwndDomain = NULL;

    if (   !IsWindowVisible(hwnd)
        || !GetClassName(hwnd, szClass, sizeof(szClass))
        || lstrcmp(szClass, "#32770"))
        return TRUE;

    hwndUser     = vboxAutoLogonEdit(hwnd, VBOXAUTOLOGON_WIN_USER, FALSE);
    hwndPassword = vboxAutoLogonEdit(hwnd, VBOXAUTOLOGON_WIN_PASSWORD, TRUE);
    if (!hwndUser || !hwndPassword)
    {
        hwndUser     = vboxAutoLogonEdit(hwnd, VBOXAUTOLOGON_NET_USER, FALSE);
        hwndPassword = vboxAutoLogonEdit(hwnd, VBOXAUTOLOGON_NET_PASSWORD, TRUE);
        hwndDomain   = vboxAutoLogonEdit(hwnd, VBOXAUTOLOGON_NET_DOMAIN, FALSE);
    }
    if (!hwndUser || !hwndPassword)
        return TRUE;

    GetWindowThreadProcessId(hwnd, &idProcess);
    if (!vboxAutoLogonIsMprExe(idProcess))
        return TRUE;

    pDlg->hwnd         = hwnd;
    pDlg->hwndUser     = hwndUser;
    pDlg->hwndPassword = hwndPassword;
    pDlg->hwndDomain   = hwndDomain;
    return FALSE;
}

/**
 * Overwrites memory that held a password; volatile so the compiler doesn't
 * drop the stores to memory about to be freed or go out of scope.
 */
static void vboxAutoLogonWipe(void *pv, DWORD cb)
{
    volatile BYTE *pb = (volatile BYTE *)pv;
    while (cb--)
        *pb++ = 0;
}

/**
 * Puts UTF-8 text from the host into an edit control.
 */
static void vboxAutoLogonSetText(HWND hwnd, const char *pszUtf8)
{
    char *psz = VBoxTrayUtf8ToAnsi(pszUtf8, lstrlen(pszUtf8), '?');
    if (psz)
    {
        SendMessage(hwnd, WM_SETTEXT, 0, (LPARAM)psz);
        vboxAutoLogonWipe(psz, lstrlen(psz));
        free(psz);
    }
}

/**
 * Logs on through the dialog with the host's credentials, if it has any.
 * Reading them clears them, so each set of credentials gets one try (a wrong
 * password leaves the dialog up for the user or the next set).
 */
static void vboxAutoLogonTry(HANDLE hDevice, VBOXAUTOLOGONDLG *pDlg)
{
    VMMDevCredentials Creds;
    int rc;

    if (RT_FAILURE(VbglR3CredentialsQueryAvailability(hDevice)))
        return;
    rc = VbglR3CredentialsRetrieve(hDevice, &Creds);
    if (RT_FAILURE(rc))
        VBoxTrayLogRel("AutoLogon: reading the credentials failed, rc=%d\n", rc);
    else if (Creds.u32Flags & VMMDEV_CREDENTIALS_NOLOCALLOGON)
        VBoxTrayLogRel("AutoLogon: the host's credentials are not for a local logon, ignored\n");
    else
    {
        vboxAutoLogonSetText(pDlg->hwndUser, Creds.szUserName);
        vboxAutoLogonSetText(pDlg->hwndPassword, Creds.szPassword);
        if (pDlg->hwndDomain && Creds.szDomain[0])
            vboxAutoLogonSetText(pDlg->hwndDomain, Creds.szDomain);
        PostMessage(pDlg->hwnd, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), (LPARAM)GetDlgItem(pDlg->hwnd, IDOK));
        VBoxTrayLogRel("AutoLogon: logging on as '%s'%s\n", Creds.szUserName,
                       pDlg->hwndDomain ? " (domain logon)" : "");
    }
    vboxAutoLogonWipe(&Creds, sizeof(Creds));
}

/**
 * Automatic logon thread.
 *
 * @returns 0 on normal exit, 1 if the driver could not be used.
 * @param   pvUser      Unused.
 */
unsigned __stdcall VBoxAutoLogonThread(void *pvUser)
{
    HANDLE hDevice;

    hDevice = VbglR3Open();
    if (hDevice == INVALID_HANDLE_VALUE)
        return 1;

    while (!g_fVBoxTrayShutdown)
    {
        VBOXAUTOLOGONDLG Dlg;

        memset(&Dlg, 0, sizeof(Dlg));
        EnumWindows(vboxAutoLogonEnumProc, (LPARAM)&Dlg);
        if (Dlg.hwnd)
            vboxAutoLogonTry(hDevice, &Dlg);
        Sleep(VBOXAUTOLOGON_POLL_MS);
    }

    VbglR3Close(hDevice);
    return 0;
}
