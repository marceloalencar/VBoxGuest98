/** @file
 * VBoxTray - Automatic mounting of shared folders on drive letters.
 *
 * Follows upstream VBoxServiceAutoMount.cpp on Windows: every shared folder
 * the host marks for auto-mounting gets \\VBOXSVR\<name> on the drive letter
 * the host asks for when that one is free, otherwise on the last free letter
 * (Z: downwards); shared folders that go away are unmounted, and the mounts
 * stay when VBoxTray exits.  SHFL_FN_WAIT_FOR_MAPPINGS_CHANGES wakes the
 * thread up when the host changes the shared folders.
 *
 * Windows 98 and ME have no network provider for VBoxSF, so instead of
 * WNetAddConnection2 the letters are redirected with INT 21h 5F03h/5F04h,
 * sent to IFSMgr through its Win32 interface (IFS_IOCTL_21, as MSNET32 does).
 * Such a redirection is system wide and outlives the process.  VBoxSF (the
 * shared folders VxD) has to be loaded for it to work.
 */
#include "VBoxTray.h"
#include <string.h>


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/

/** IFSMgr's Win32 DeviceIoControl code for an INT 21h function (IFS.H). */
#define VBOXAUTOMOUNT_IFS_IOCTL_21  100
/** Server part of the mounted names. */
#define VBOXAUTOMOUNT_PREFIX        "\\\\VBOXSVR\\"
/** Most shared folders handled. */
#define VBOXAUTOMOUNT_MAX           SHFL_MAX_MAPPINGS
/** How often to look again if the host can't wait for changes (ms). */
#define VBOXAUTOMOUNT_POLL_MS       10000
/** Size of the SHFLSTRING buffers for names and mount points. */
#define VBOXAUTOMOUNT_CB_STRING     (SHFLSTRING_HEADER_SIZE + (SHFL_MAX_LEN + 1) * sizeof(WCHAR))


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/

/** IFS_IOCTL_21 input and output (struct win32apireq of IFS.H). */
typedef struct VBOXAUTOMOUNTINT21
{
    DWORD dwProvider;
    DWORD dwEax;
    DWORD dwEbx;
    DWORD dwEcx;
    DWORD dwEdx;
    DWORD dwEsi;
    DWORD dwEdi;
    DWORD dwEbp;
    WORD  wError;
    WORD  wPad;
} VBOXAUTOMOUNTINT21;

/** A shared folder mounted (or found mounted) by us. */
typedef struct VBOXAUTOMOUNTENTRY
{
    char chDrive;
    char szName[SHFL_MAX_LEN + 1];
} VBOXAUTOMOUNTENTRY;


/*********************************************************************************************************************************
*   Global Variables                                                                                                             *
*********************************************************************************************************************************/

/** Guards g_idAutoMountClient against VBoxAutoMountStop. */
static CRITICAL_SECTION g_AutoMountCritSect;
static HANDLE g_hAutoMountDevice = INVALID_HANDLE_VALUE;
static UINT32 g_idAutoMountClient;
/** Signaled by VBoxAutoMountStop (for the polling fallback). */
static HANDLE g_hAutoMountEvent;
/** The mounts we look after. */
static VBOXAUTOMOUNTENTRY g_aAutoMounts[VBOXAUTOMOUNT_MAX];
static unsigned g_cAutoMounts;


/*********************************************************************************************************************************
*   Drive redirection through IFSMgr                                                                                             *
*********************************************************************************************************************************/

/**
 * Runs an INT 21h redirection function in IFSMgr.
 *
 * @returns 0 or the DOS error code.
 */
static unsigned vboxAutoMountInt21(HANDLE hIfsMgr, VBOXAUTOMOUNTINT21 *pReq)
{
    DWORD cb = 0;
    pReq->dwProvider = 0xffffffff;  /* ANYPROID */
    if (!DeviceIoControl(hIfsMgr, VBOXAUTOMOUNT_IFS_IOCTL_21, pReq, sizeof(*pReq), pReq, sizeof(*pReq), &cb, NULL))
        return GetLastError() ? GetLastError() : ERROR_GEN_FAILURE;
    return pReq->wError;
}

/**
 * Finds the drive letter a remote name is redirected to (INT 21h 5F02h).
 *
 * @returns The letter, 0 if none.
 */
static char vboxAutoMountFindRedirection(HANDLE hIfsMgr, const char *pszRemote)
{
    char szLocal[16];
    char szRemote[128];
    VBOXAUTOMOUNTINT21 Req;
    DWORD iEntry;

    for (iEntry = 0; iEntry < 64; iEntry++)
    {
        memset(&Req, 0, sizeof(Req));
        szLocal[0] = szRemote[0] = '\0';
        Req.dwEax = 0x5f02;
        Req.dwEbx = iEntry;
        Req.dwEsi = (DWORD)szLocal;
        Req.dwEdi = (DWORD)szRemote;
        if (vboxAutoMountInt21(hIfsMgr, &Req))
            break;
        szLocal[sizeof(szLocal) - 1] = szRemote[sizeof(szRemote) - 1] = '\0';
        if (szLocal[1] == ':' && !lstrcmpi(szRemote, pszRemote))
            return szLocal[0] >= 'a' && szLocal[0] <= 'z' ? (char)(szLocal[0] - 'a' + 'A') : szLocal[0];
    }
    return 0;
}

/**
 * Redirects a drive letter to a remote name (INT 21h 5F03h).
 *
 * @returns 0 or the DOS error code.
 */
static unsigned vboxAutoMountRedirect(HANDLE hIfsMgr, char chDrive, const char *pszRemote)
{
    char szLocal[3];
    char szRemote[sizeof(VBOXAUTOMOUNT_PREFIX) + SHFL_MAX_LEN * 2 + 2];
    VBOXAUTOMOUNTINT21 Req;
    int cch = lstrlen(pszRemote);

    if (cch >= (int)sizeof(szRemote) - 1)
        return ERROR_BAD_NETPATH;
    szLocal[0] = chDrive;
    szLocal[1] = ':';
    szLocal[2] = '\0';
    memcpy(szRemote, pszRemote, cch + 1);
    szRemote[cch + 1] = '\0';               /* No password. */
    memset(&Req, 0, sizeof(Req));
    Req.dwEax = 0x5f03;
    Req.dwEbx = 4;                          /* Disk. */
    Req.dwEsi = (DWORD)szLocal;
    Req.dwEdi = (DWORD)szRemote;
    return vboxAutoMountInt21(hIfsMgr, &Req);
}

/**
 * Cancels the redirection of a drive letter (INT 21h 5F04h).
 *
 * @returns 0 or the DOS error code.
 */
static unsigned vboxAutoMountCancel(HANDLE hIfsMgr, char chDrive)
{
    char szLocal[3];
    VBOXAUTOMOUNTINT21 Req;

    szLocal[0] = chDrive;
    szLocal[1] = ':';
    szLocal[2] = '\0';
    memset(&Req, 0, sizeof(Req));
    Req.dwEax = 0x5f04;
    Req.dwEsi = (DWORD)szLocal;
    return vboxAutoMountInt21(hIfsMgr, &Req);
}


/*********************************************************************************************************************************
*   Shared folders service calls                                                                                                 *
*********************************************************************************************************************************/

/**
 * Converts a SHFLSTRING (UTF-16) to the OEM code page, as DOS names are.
 */
static void vboxAutoMountFromShfl(const SHFLSTRING *pStr, ULONG cbBuf, char *psz, int cch)
{
    int cwc = pStr->u16Length / sizeof(WCHAR);
    if ((ULONG)pStr->u16Length + SHFLSTRING_HEADER_SIZE > cbBuf)
        cwc = 0;
    cwc = WideCharToMultiByte(CP_OEMCP, 0, pStr->String.utf16, cwc, psz, cch - 1, NULL, NULL);
    psz[cwc > 0 ? cwc : 0] = '\0';
}

/**
 * Gets the shared folders to auto-mount (SHFL_FN_QUERY_MAPPINGS with
 * SHFL_MF_AUTOMOUNT).
 *
 * @returns VBox status code.
 */
static int vboxAutoMountQueryMappings(SHFLMAPPING *paMappings, UINT32 *pcMappings)
{
    VBGLR3HGCMMSG Msg;
    BYTE *pb;
    UINT32 cMappings;
    int rc;

    if (!VbglR3HgcmMsgInit(&Msg, g_idAutoMountClient, SHFL_FN_QUERY_MAPPINGS, SHFL_FN_QUERY_MAPPINGS_CPARMS,
                           VBOXGUEST98_WAIT_INFINITE, VBOXAUTOMOUNT_MAX * sizeof(SHFLMAPPING)))
        return VERR_NO_MEMORY;
    VbglR3HgcmMsgSetU32(&Msg, 0, SHFL_MF_UCS2 | SHFL_MF_AUTOMOUNT);
    VbglR3HgcmMsgSetU32(&Msg, 1, VBOXAUTOMOUNT_MAX);
    pb = VbglR3HgcmMsgSetBuf(&Msg, 2, VBOXGUEST98_HGCM_PARM_BUFFER_OUT, VBOXAUTOMOUNT_MAX * sizeof(SHFLMAPPING), NULL);
    rc = VbglR3HgcmMsgCall(g_hAutoMountDevice, &Msg);
    cMappings = 0;
    if (RT_SUCCESS(rc))
    {
        cMappings = VbglR3HgcmMsgGetU32(&Msg, 1);
        if (cMappings > VBOXAUTOMOUNT_MAX)
            cMappings = VBOXAUTOMOUNT_MAX;
        memcpy(paMappings, pb, cMappings * sizeof(SHFLMAPPING));
    }
    *pcMappings = cMappings;
    VbglR3HgcmMsgFree(&Msg);
    return rc;
}

/**
 * Gets the name, mount point and flags of a shared folder
 * (SHFL_FN_QUERY_MAP_INFO).
 *
 * @returns VBox status code.
 */
static int vboxAutoMountQueryInfo(SHFLROOT idRoot, char *pszName, int cchName, char *pszMountPoint, int cchMountPoint,
                                  UINT64 *pfFlags)
{
    VBGLR3HGCMMSG Msg;
    SHFLSTRING *pName, *pMountPoint;
    int rc;

    if (!VbglR3HgcmMsgInit(&Msg, g_idAutoMountClient, SHFL_FN_QUERY_MAP_INFO, SHFL_FN_QUERY_MAP_INFO_CPARMS,
                           VBOXGUEST98_WAIT_INFINITE, 2 * VBOXAUTOMOUNT_CB_STRING))
        return VERR_NO_MEMORY;
    VbglR3HgcmMsgSetU32(&Msg, 0, idRoot);
    pName = (SHFLSTRING *)VbglR3HgcmMsgSetBuf(&Msg, 1, VBOXGUEST98_HGCM_PARM_BUFFER_INOUT, VBOXAUTOMOUNT_CB_STRING, NULL);
    pName->u16Size = VBOXAUTOMOUNT_CB_STRING - SHFLSTRING_HEADER_SIZE;
    pMountPoint = (SHFLSTRING *)VbglR3HgcmMsgSetBuf(&Msg, 2, VBOXGUEST98_HGCM_PARM_BUFFER_INOUT,
                                                    VBOXAUTOMOUNT_CB_STRING, NULL);
    pMountPoint->u16Size = VBOXAUTOMOUNT_CB_STRING - SHFLSTRING_HEADER_SIZE;
    VbglR3HgcmMsgSetU64(&Msg, 3, SHFL_MIQF_DRIVE_LETTER);
    VbglR3HgcmMsgSetU32(&Msg, 4, 0);
    rc = VbglR3HgcmMsgCall(g_hAutoMountDevice, &Msg);
    if (RT_SUCCESS(rc))
    {
        vboxAutoMountFromShfl(pName, VBOXAUTOMOUNT_CB_STRING, pszName, cchName);
        vboxAutoMountFromShfl(pMountPoint, VBOXAUTOMOUNT_CB_STRING, pszMountPoint, cchMountPoint);
        *pfFlags = VbglR3HgcmMsgGetU64(&Msg, 3);
    }
    VbglR3HgcmMsgFree(&Msg);
    return rc;
}

/**
 * Waits for the host to change the shared folders
 * (SHFL_FN_WAIT_FOR_MAPPINGS_CHANGES).
 *
 * @returns VBox status code: VINF_SUCCESS on a change, VINF_TRY_AGAIN after
 *          a restore, VERR_CANCELLED when stopped.
 * @param   puVersion   The configuration version known, updated.
 */
static int vboxAutoMountWaitForChanges(UINT32 *puVersion)
{
    VBGLR3HGCMMSG Msg;
    int rc;

    if (!VbglR3HgcmMsgInit(&Msg, g_idAutoMountClient, SHFL_FN_WAIT_FOR_MAPPINGS_CHANGES,
                           SHFL_FN_WAIT_FOR_MAPPINGS_CHANGES_CPARMS, VBOXGUEST98_WAIT_INFINITE, 0))
        return VERR_NO_MEMORY;
    VbglR3HgcmMsgSetU32(&Msg, 0, *puVersion);
    rc = VbglR3HgcmMsgCall(g_hAutoMountDevice, &Msg);
    if (RT_SUCCESS(rc))
        *puVersion = VbglR3HgcmMsgGetU32(&Msg, 0);
    VbglR3HgcmMsgFree(&Msg);
    return rc;
}


/*********************************************************************************************************************************
*   Mounting                                                                                                                     *
*********************************************************************************************************************************/

/**
 * Picks a free drive letter: the host's choice if it is free, else the last
 * free one from Z: down to D:.
 *
 * @returns The letter, 0 if none is free.
 */
static char vboxAutoMountPickLetter(const char *pszMountPoint)
{
    DWORD fDrives = GetLogicalDrives();
    char chDrive;

    if (   pszMountPoint[0] && pszMountPoint[1] == ':'
        && ((pszMountPoint[0] >= 'A' && pszMountPoint[0] <= 'Z') || (pszMountPoint[0] >= 'a' && pszMountPoint[0] <= 'z')))
    {
        chDrive = (char)(pszMountPoint[0] & ~0x20);
        if (!(fDrives & (1UL << (chDrive - 'A'))))
            return chDrive;
    }
    for (chDrive = 'Z'; chDrive >= 'D'; chDrive--)
        if (!(fDrives & (1UL << (chDrive - 'A'))))
            return chDrive;
    return 0;
}

/**
 * Mounts the new auto-mount shared folders and unmounts the ones gone.
 */
static void vboxAutoMountUpdate(HANDLE hIfsMgr)
{
    static SHFLMAPPING s_aMappings[VBOXAUTOMOUNT_MAX];
    static char s_aszNames[VBOXAUTOMOUNT_MAX][SHFL_MAX_LEN + 1];
    UINT32 cMappings, i;
    unsigned iMount;
    int rc;

    rc = vboxAutoMountQueryMappings(s_aMappings, &cMappings);
    if (RT_FAILURE(rc))
    {
        VBoxTrayLogRel("AutoMount: listing the shared folders failed, rc=%d\n", rc);
        return;
    }

    /* Mount the ones we don't have. */
    for (i = 0; i < cMappings; i++)
    {
        char szMountPoint[SHFL_MAX_LEN + 1];
        char szRemote[sizeof(VBOXAUTOMOUNT_PREFIX) + SHFL_MAX_LEN * 2];
        UINT64 fFlags = 0;
        char chDrive;
        unsigned uErr;

        s_aszNames[i][0] = '\0';
        rc = vboxAutoMountQueryInfo(s_aMappings[i].root, s_aszNames[i], sizeof(s_aszNames[i]),
                                    szMountPoint, sizeof(szMountPoint), &fFlags);
        if (RT_FAILURE(rc) || !s_aszNames[i][0] || !(fFlags & SHFL_MIF_AUTO_MOUNT))
        {
            s_aszNames[i][0] = '\0';
            continue;
        }
        for (iMount = 0; iMount < g_cAutoMounts; iMount++)
            if (!lstrcmpi(g_aAutoMounts[iMount].szName, s_aszNames[i]))
                break;
        if (iMount < g_cAutoMounts || g_cAutoMounts >= VBOXAUTOMOUNT_MAX)
            continue;

        wsprintf(szRemote, "%s%s", VBOXAUTOMOUNT_PREFIX, s_aszNames[i]);
        chDrive = vboxAutoMountFindRedirection(hIfsMgr, szRemote);
        if (chDrive)
            VBoxTrayLogRel("AutoMount: '%s' is already on %c:\n", s_aszNames[i], chDrive);
        else
        {
            chDrive = vboxAutoMountPickLetter(szMountPoint);
            if (!chDrive)
            {
                VBoxTrayLogRel("AutoMount: no free drive letter for '%s'\n", s_aszNames[i]);
                continue;
            }
            uErr = vboxAutoMountRedirect(hIfsMgr, chDrive, szRemote);
            if (uErr)
            {
                VBoxTrayLogRel("AutoMount: mounting '%s' on %c: failed, error %u\n", s_aszNames[i], chDrive, uErr);
                continue;
            }
            VBoxTrayLogRel("AutoMount: mounted '%s' on %c:\n", s_aszNames[i], chDrive);
        }
        g_aAutoMounts[g_cAutoMounts].chDrive = chDrive;
        lstrcpyn(g_aAutoMounts[g_cAutoMounts].szName, s_aszNames[i], sizeof(g_aAutoMounts[0].szName));
        g_cAutoMounts++;
    }

    /* Unmount ours that aren't shared (or auto-mounted) any more. */
    for (iMount = 0; iMount < g_cAutoMounts; )
    {
        for (i = 0; i < cMappings; i++)
            if (s_aszNames[i][0] && !lstrcmpi(g_aAutoMounts[iMount].szName, s_aszNames[i]))
                break;
        if (i < cMappings)
        {
            iMount++;
            continue;
        }
        VBoxTrayLogRel("AutoMount: unmounting '%s' from %c: (%u)\n", g_aAutoMounts[iMount].szName,
                       g_aAutoMounts[iMount].chDrive, vboxAutoMountCancel(hIfsMgr, g_aAutoMounts[iMount].chDrive));
        g_aAutoMounts[iMount] = g_aAutoMounts[--g_cAutoMounts];
    }
}


/*********************************************************************************************************************************
*   Service                                                                                                                      *
*********************************************************************************************************************************/

void VBoxAutoMountInit(void)
{
    InitializeCriticalSection(&g_AutoMountCritSect);
    g_hAutoMountEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
}

/**
 * Cancels the wait for changes and disconnects, so the thread ends.
 */
void VBoxAutoMountStop(void)
{
    UINT32 idClient;

    EnterCriticalSection(&g_AutoMountCritSect);
    idClient = g_idAutoMountClient;
    g_idAutoMountClient = 0;
    LeaveCriticalSection(&g_AutoMountCritSect);
    if (g_hAutoMountEvent)
        SetEvent(g_hAutoMountEvent);
    if (idClient)
    {
        VBGLR3HGCMMSG Msg;
        if (VbglR3HgcmMsgInit(&Msg, idClient, SHFL_FN_CANCEL_MAPPINGS_CHANGES_WAITS,
                              SHFL_FN_CANCEL_MAPPINGS_CHANGES_WAITS_CPARMS, VBOXGUEST98_WAIT_INFINITE, 0))
        {
            VbglR3HgcmMsgCall(g_hAutoMountDevice, &Msg);
            VbglR3HgcmMsgFree(&Msg);
        }
        VbglR3HgcmDisconnect(g_hAutoMountDevice, idClient);
    }
}

/**
 * Auto-mount thread.
 *
 * @returns 0 on normal exit, 1 if the service or VBoxSF could not be used.
 * @param   pvUser      Unused.
 */
unsigned __stdcall VBoxAutoMountThread(void *pvUser)
{
    HANDLE hVBoxSF, hIfsMgr;
    UINT32 idClient;
    UINT32 uVersion = 0;
    int rc;

    (void)pvUser;
    /* Without VBoxSF there is nothing to mount on. */
    hVBoxSF = CreateFile("\\\\.\\VBOXSF", 0, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (hVBoxSF == INVALID_HANDLE_VALUE)
    {
        VBoxTrayLogRel("AutoMount: VBoxSF is not loaded, no shared folders\n");
        return 1;
    }
    CloseHandle(hVBoxSF);
    hIfsMgr = CreateFile("\\\\.\\IFSMGR", 0, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (hIfsMgr == INVALID_HANDLE_VALUE)
    {
        VBoxTrayLogRel("AutoMount: opening IFSMgr failed, error %u\n", GetLastError());
        return 1;
    }

    /* Overlapped: VBoxAutoMountStop cancels the wait on the same client from another thread. */
    g_hAutoMountDevice = VbglR3OpenEx(TRUE);
    if (g_hAutoMountDevice == INVALID_HANDLE_VALUE)
    {
        CloseHandle(hIfsMgr);
        return 1;
    }
    rc = VbglR3HgcmConnect(g_hAutoMountDevice, SHFL_SERVICE_NAME, &idClient);
    if (RT_FAILURE(rc))
    {
        VBoxTrayLogRel("AutoMount: connecting to the shared folders service failed, rc=%d\n", rc);
        VbglR3Close(g_hAutoMountDevice);
        CloseHandle(hIfsMgr);
        return 1;
    }
    EnterCriticalSection(&g_AutoMountCritSect);
    g_idAutoMountClient = g_fVBoxTrayShutdown ? 0 : idClient;
    LeaveCriticalSection(&g_AutoMountCritSect);
    if (!g_idAutoMountClient)
        VbglR3HgcmDisconnect(g_hAutoMountDevice, idClient);

    while (!g_fVBoxTrayShutdown && g_idAutoMountClient)
    {
        vboxAutoMountUpdate(hIfsMgr);
        rc = vboxAutoMountWaitForChanges(&uVersion);
        if (g_fVBoxTrayShutdown)
            break;
        if (RT_FAILURE(rc) && rc != VERR_CANCELLED)
        {
            /* Older hosts can't wait: look again now and then. */
            WaitForSingleObject(g_hAutoMountEvent, VBOXAUTOMOUNT_POLL_MS);
        }
    }

    EnterCriticalSection(&g_AutoMountCritSect);
    idClient = g_idAutoMountClient;
    g_idAutoMountClient = 0;
    LeaveCriticalSection(&g_AutoMountCritSect);
    if (idClient)
        VbglR3HgcmDisconnect(g_hAutoMountDevice, idClient);
    VbglR3Close(g_hAutoMountDevice);
    CloseHandle(hIfsMgr);
    return 0;
}
