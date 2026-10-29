/** @file
 * VBoxNP - Network provider for the shared folders on Windows 98 and ME.
 *
 * MPR loads this DLL at startup and asks it about the names of the
 * "VirtualBox Shared Folders" network, like VBoxMRXNP.dll of the official
 * Guest Additions for Windows NT: the network holds one server, \\VBOXSVR,
 * and the server holds the host's shared folders.  With it Explorer shows the
 * network under Entire Network, opens \\VBOXSVR, and connects drive letters
 * from "Map Network Drive" (WNetAddConnection).
 *
 * The files themselves go through VBoxSF.vxd, the IFSMgr redirector, which
 * also gives this DLL the names of the shared folders (VBOXSF_IOCTL_QUERY_NAMES).
 * Drive letters are redirected with INT 21h 5F02h/5F03h/5F04h through IFSMgr's
 * Win32 interface, as VBoxTray's auto-mounting does; those calls take OEM
 * names, MPR gives and wants ANSI ones.
 *
 * The SPI is the one of Windows 95 (NETSPI.H of the Windows 98 DDK): ANSI
 * strings, buffer sizes in bytes, the variable parts of the returned
 * NETRESOURCE entries at the end of the caller's buffer, and NPEnumResource
 * reports the free space in between.  NPGetCaps is exported by name, the
 * rest by ordinal (VBoxNP.def).
 */
#include <windows.h>
#include <winnetwk.h>
#include <string.h>


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/

/** Name of the network, as Explorer shows it. */
#define VBOXNP_PROVIDER_NAME        "VirtualBox Shared Folders"
/** The server, with the alternative name VBoxSF accepts too. */
#define VBOXNP_SERVER_NAME          "VBOXSVR"
#define VBOXNP_SERVER_NAME_ALT      "VBOXSRV"
#define VBOXNP_SERVER_CCH           7

/** Network type (WNNC_NET_RDR2SAMPLE, the one of VBoxMRXNP.dll); VBoxSF
 *  registers with IFSMgr under it too. */
#define VBOXNP_NET_TYPE             WNNC_NET_RDR2SAMPLE

/** VBoxSF's Win32 DeviceIoControl code for the names of the shared folders. */
#define VBOXNP_IOCTL_QUERY_NAMES    2
/** IFSMgr's Win32 DeviceIoControl code for an INT 21h function (IFS.H). */
#define VBOXNP_IFS_IOCTL_21         100
/** Most shared folders (SHFL_MAX_MAPPINGS) and longest name (SHFL_MAX_LEN). */
#define VBOXNP_MAX_SHARES           64
#define VBOXNP_MAX_NAME             256
/** Longest remote name: \\VBOXSVR\ plus a name in a DBCS code page, and the
 *  empty password INT 21h 5F03h wants after it. */
#define VBOXNP_MAX_REMOTE           (2 + VBOXNP_SERVER_CCH + 1 + VBOXNP_MAX_NAME * 2 + 2)
/** Most redirections INT 21h 5F02h is asked for. */
#define VBOXNP_MAX_REDIRECTIONS     64

/*
 * NPGetCaps queries and answers (NETSPI.H).
 */
#define WNNC_SPEC_VERSION                   0x01
#define     WNNC_SPEC_VERSION51             0x00050001
#define WNNC_NET_TYPE                       0x02
#define WNNC_DRIVER_VERSION                 0x03
#define WNNC_CONNECTION                     0x06
#define     WNNC_CON_ADDCONNECTION          0x01
#define     WNNC_CON_CANCELCONNECTION       0x02
#define     WNNC_CON_GETCONNECTIONS         0x04
#define WNNC_DIALOG                         0x08
#define     WNNC_DLG_FORMATNETWORKNAME      0x080
#define     WNNC_DLG_GETRESOURCEPARENT      0x100
#define     WNNC_DLG_GETRESOURCEINFORMATION 0x800
#define WNNC_ENUMERATION                    0x0B
#define     WNNC_ENUM_GLOBAL                0x01
#define     WNNC_ENUM_LOCAL                 0x02
#define WNNC_START                          0x0C
#define     WNNC_START_DONE                 0x01
#define WNNC_DRIVER(major, minor)           ((major) * 0x00010000 + (minor))

/** NPFormatNetworkName flag: the name is shown inside its container. */
#define WNFMT_INENUM                        0x10

#define SPIENTRY DWORD WINAPI


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/

/** IFS_IOCTL_21 input and output (struct win32apireq of IFS.H). */
typedef struct VBOXNPINT21
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
} VBOXNPINT21;

/** A resource to return, before it is packed into a NETRESOURCE. */
typedef struct VBOXNPRES
{
    DWORD dwScope;
    DWORD dwType;
    DWORD dwDisplayType;
    DWORD dwUsage;
    char  szLocal[3];
    char  szRemote[VBOXNP_MAX_REMOTE];
} VBOXNPRES;

/** An enumeration (the handle NPOpenEnum gives). */
typedef struct VBOXNPENUM
{
    DWORD     iNext;
    DWORD     cRes;
    VBOXNPRES aRes[VBOXNP_MAX_SHARES];
} VBOXNPENUM;


/*********************************************************************************************************************************
*   Name helpers                                                                                                                 *
*********************************************************************************************************************************/

/**
 * Compares the start of a name with an upper case ASCII string.
 */
static BOOL vboxNpStartsWith(const char *psz, const char *pszUpper, int cch)
{
    while (cch-- > 0)
    {
        char ch = *psz++;
        if (ch >= 'a' && ch <= 'z')
            ch = (char)(ch - 'a' + 'A');
        if (ch != *pszUpper++)
            return FALSE;
    }
    return TRUE;
}

/**
 * Checks that a remote name is on \\VBOXSVR (or \\VBOXSRV).
 *
 * @returns What follows the server name ("" or "\..."), NULL if the name
 *          is not ours.
 */
static const char *vboxNpSkipServer(const char *pszRemote)
{
    if (!pszRemote || pszRemote[0] != '\\' || pszRemote[1] != '\\')
        return NULL;
    pszRemote += 2;
    if (   !vboxNpStartsWith(pszRemote, VBOXNP_SERVER_NAME, VBOXNP_SERVER_CCH)
        && !vboxNpStartsWith(pszRemote, VBOXNP_SERVER_NAME_ALT, VBOXNP_SERVER_CCH))
        return NULL;
    pszRemote += VBOXNP_SERVER_CCH;
    return *pszRemote == '\\' || *pszRemote == '\0' ? pszRemote : NULL;
}

/**
 * Checks a local device name ("X:", either case).
 *
 * @returns The upper case drive letter, 0 if it isn't one.
 */
static char vboxNpDriveLetter(const char *pszLocal)
{
    char ch;
    if (!pszLocal || pszLocal[1] != ':' || pszLocal[2] != '\0')
        return 0;
    ch = pszLocal[0];
    if (ch >= 'a' && ch <= 'z')
        ch = (char)(ch - 'a' + 'A');
    return ch >= 'A' && ch <= 'Z' ? ch : 0;
}

/**
 * Copies a string to a caller's buffer (the size in bytes).
 *
 * @returns WN_SUCCESS, or WN_MORE_DATA with the size needed in *pcbBuffer.
 */
static DWORD vboxNpCopyOut(const char *psz, char *pszBuffer, LPDWORD pcbBuffer)
{
    DWORD cb = lstrlen(psz) + 1;
    if (!pszBuffer || *pcbBuffer < cb)
    {
        *pcbBuffer = cb;
        return WN_MORE_DATA;
    }
    memcpy(pszBuffer, psz, cb);
    return WN_SUCCESS;
}


/*********************************************************************************************************************************
*   Drive redirection through IFSMgr                                                                                             *
*********************************************************************************************************************************/

/**
 * Runs an INT 21h redirection function in IFSMgr.
 *
 * @returns 0 or the DOS error code.
 */
static unsigned vboxNpInt21(VBOXNPINT21 *pReq)
{
    HANDLE hIfsMgr;
    DWORD cb = 0;
    unsigned uErr;

    hIfsMgr = CreateFile("\\\\.\\IFSMGR", 0, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (hIfsMgr == INVALID_HANDLE_VALUE)
        return ERROR_NO_NETWORK;
    pReq->dwProvider = 0xffffffff;  /* ANYPROID */
    if (DeviceIoControl(hIfsMgr, VBOXNP_IFS_IOCTL_21, pReq, sizeof(*pReq), pReq, sizeof(*pReq), &cb, NULL))
        uErr = pReq->wError;
    else
        uErr = GetLastError() ? GetLastError() : ERROR_GEN_FAILURE;
    CloseHandle(hIfsMgr);
    return uErr;
}

/**
 * Gets a drive redirection by index (INT 21h 5F02h) if it is ours.
 *
 * @returns WN_SUCCESS, WN_NOT_CONNECTED if the entry belongs to another
 *          network, WN_NO_MORE_ENTRIES past the last one.
 * @param   iEntry      The index.
 * @param   pchDrive    Where to return the drive letter.
 * @param   pszRemote   Where to return the remote name (ANSI), at least
 *                      VBOXNP_MAX_REMOTE bytes.
 */
static DWORD vboxNpGetRedirection(DWORD iEntry, char *pchDrive, char *pszRemote)
{
    char szLocal[16];
    char szRemote[128];
    VBOXNPINT21 Req;

    memset(&Req, 0, sizeof(Req));
    szLocal[0] = szRemote[0] = '\0';
    Req.dwEax = 0x5f02;
    Req.dwEbx = iEntry;
    Req.dwEsi = (DWORD)szLocal;
    Req.dwEdi = (DWORD)szRemote;
    if (vboxNpInt21(&Req))
        return WN_NO_MORE_ENTRIES;
    szLocal[sizeof(szLocal) - 1] = szRemote[sizeof(szRemote) - 1] = '\0';
    *pchDrive = vboxNpDriveLetter(szLocal);
    if (!*pchDrive || !vboxNpSkipServer(szRemote))
        return WN_NOT_CONNECTED;
    OemToChar(szRemote, pszRemote);
    return WN_SUCCESS;
}

/**
 * Finds the remote name of one of our drive letters.
 *
 * @returns WN_SUCCESS or WN_NOT_CONNECTED.
 */
static DWORD vboxNpFindRedirection(char chDrive, char *pszRemote)
{
    DWORD iEntry;
    for (iEntry = 0; iEntry < VBOXNP_MAX_REDIRECTIONS; iEntry++)
    {
        char chEntry;
        DWORD dwStatus = vboxNpGetRedirection(iEntry, &chEntry, pszRemote);
        if (dwStatus == WN_NO_MORE_ENTRIES)
            break;
        if (dwStatus == WN_SUCCESS && chEntry == chDrive)
            return WN_SUCCESS;
    }
    return WN_NOT_CONNECTED;
}


/*********************************************************************************************************************************
*   Shared folder names (VBoxSF.vxd)                                                                                             *
*********************************************************************************************************************************/

/**
 * Gets the names of the host's shared folders.
 *
 * @returns WN_SUCCESS or WN_NO_NETWORK.
 * @param   ppwszNames  Where to return them (each one terminated, the list
 *                      ended by an empty one), to free with HeapFree.
 */
static DWORD vboxNpQueryNames(WCHAR **ppwszNames)
{
    DWORD cb = (VBOXNP_MAX_SHARES * (VBOXNP_MAX_NAME + 1) + 1) * sizeof(WCHAR);
    WCHAR *pwszNames;
    HANDLE hVBoxSF;
    DWORD cbRet = 0;
    BOOL fOk;

    *ppwszNames = NULL;
    hVBoxSF = CreateFile("\\\\.\\VBOXSF", 0, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (hVBoxSF == INVALID_HANDLE_VALUE)
        return WN_NO_NETWORK;
    pwszNames = (WCHAR *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, cb);
    fOk = pwszNames && DeviceIoControl(hVBoxSF, VBOXNP_IOCTL_QUERY_NAMES, NULL, 0, pwszNames, cb, &cbRet, NULL);
    CloseHandle(hVBoxSF);
    if (!fOk)
    {
        if (pwszNames)
            HeapFree(GetProcessHeap(), 0, pwszNames);
        return WN_NO_NETWORK;
    }
    *ppwszNames = pwszNames;
    return WN_SUCCESS;
}

/**
 * Builds the remote name of a shared folder (\\VBOXSVR\name, ANSI).
 *
 * @returns FALSE if the name doesn't convert.
 */
static BOOL vboxNpShareRemoteName(const WCHAR *pwszName, char *pszRemote)
{
    int cch;

    lstrcpy(pszRemote, "\\\\" VBOXNP_SERVER_NAME "\\");
    cch = lstrlen(pszRemote);
    cch = WideCharToMultiByte(CP_ACP, 0, pwszName, -1, pszRemote + cch, VBOXNP_MAX_REMOTE - cch, NULL, NULL);
    return cch > 1;
}


/*********************************************************************************************************************************
*   NETRESOURCE packing                                                                                                          *
*********************************************************************************************************************************/

/**
 * Bytes a resource takes in a buffer: the structure and its strings.
 */
static DWORD vboxNpResSize(const VBOXNPRES *pRes)
{
    DWORD cb = sizeof(NETRESOURCE) + lstrlen(pRes->szRemote) + 1 + sizeof(VBOXNP_PROVIDER_NAME);
    if (pRes->szLocal[0])
        cb += lstrlen(pRes->szLocal) + 1;
    return cb;
}

/**
 * Puts a string below *ppchEnd and moves *ppchEnd down.
 */
static char *vboxNpPutString(char **ppchEnd, const char *psz)
{
    DWORD cb = lstrlen(psz) + 1;
    *ppchEnd -= cb;
    memcpy(*ppchEnd, psz, cb);
    return *ppchEnd;
}

/**
 * Fills a NETRESOURCE, its strings going below *ppchEnd (the caller checked
 * the space with vboxNpResSize).
 */
static void vboxNpResPut(NETRESOURCE *pNr, char **ppchEnd, const VBOXNPRES *pRes)
{
    memset(pNr, 0, sizeof(*pNr));
    pNr->dwScope       = pRes->dwScope;
    pNr->dwType        = pRes->dwType;
    pNr->dwDisplayType = pRes->dwDisplayType;
    pNr->dwUsage       = pRes->dwUsage;
    pNr->lpProvider    = vboxNpPutString(ppchEnd, VBOXNP_PROVIDER_NAME);
    pNr->lpRemoteName  = vboxNpPutString(ppchEnd, pRes->szRemote);
    if (pRes->szLocal[0])
        pNr->lpLocalName = vboxNpPutString(ppchEnd, pRes->szLocal);
}

/**
 * Returns a single resource in a caller's buffer (NPGetResourceInformation
 * and NPGetResourceParent), with an optional string after it.
 *
 * @returns WN_SUCCESS, or WN_MORE_DATA with the size needed in *pcbBuffer.
 */
static DWORD vboxNpReturnRes(const VBOXNPRES *pRes, const char *pszExtra, char **ppszExtra,
                             LPVOID pvBuffer, LPDWORD pcbBuffer)
{
    DWORD cb = vboxNpResSize(pRes) + (pszExtra ? lstrlen(pszExtra) + 1 : 0);
    char *pchEnd;

    if (!pvBuffer || *pcbBuffer < cb)
    {
        *pcbBuffer = cb;
        return WN_MORE_DATA;
    }
    pchEnd = (char *)pvBuffer + *pcbBuffer;
    if (pszExtra)
        *ppszExtra = vboxNpPutString(&pchEnd, pszExtra);
    vboxNpResPut((NETRESOURCE *)pvBuffer, &pchEnd, pRes);
    return WN_SUCCESS;
}

/**
 * Fills a resource for \\VBOXSVR.
 */
static void vboxNpServerRes(VBOXNPRES *pRes, DWORD dwScope)
{
    memset(pRes, 0, sizeof(*pRes));
    pRes->dwScope       = dwScope;
    pRes->dwType        = RESOURCETYPE_ANY;
    pRes->dwDisplayType = RESOURCEDISPLAYTYPE_SERVER;
    pRes->dwUsage       = RESOURCEUSAGE_CONTAINER;
    lstrcpy(pRes->szRemote, "\\\\" VBOXNP_SERVER_NAME);
}

/**
 * Fills a resource for a shared folder (pszRemote is \\VBOXSVR\name).
 */
static void vboxNpShareRes(VBOXNPRES *pRes, DWORD dwScope, const char *pszRemote, char chDrive)
{
    memset(pRes, 0, sizeof(*pRes));
    pRes->dwScope       = dwScope;
    pRes->dwType        = RESOURCETYPE_DISK;
    pRes->dwDisplayType = RESOURCEDISPLAYTYPE_SHARE;
    pRes->dwUsage       = RESOURCEUSAGE_CONNECTABLE;
    lstrcpyn(pRes->szRemote, pszRemote, sizeof(pRes->szRemote));
    if (chDrive)
    {
        pRes->szLocal[0] = chDrive;
        pRes->szLocal[1] = ':';
    }
}


/*********************************************************************************************************************************
*   The network provider SPI                                                                                                     *
*********************************************************************************************************************************/

/**
 * Tells MPR what this provider does.
 */
SPIENTRY NPGetCaps(DWORD nIndex)
{
    switch (nIndex)
    {
        case WNNC_SPEC_VERSION:
            return WNNC_SPEC_VERSION51;
        case WNNC_NET_TYPE:
            return VBOXNP_NET_TYPE;
        case WNNC_DRIVER_VERSION:
            return WNNC_DRIVER(1, 0);
        case WNNC_CONNECTION:
            return WNNC_CON_ADDCONNECTION | WNNC_CON_CANCELCONNECTION | WNNC_CON_GETCONNECTIONS;
        case WNNC_DIALOG:
            return WNNC_DLG_FORMATNETWORKNAME | WNNC_DLG_GETRESOURCEPARENT | WNNC_DLG_GETRESOURCEINFORMATION;
        case WNNC_ENUMERATION:
            return WNNC_ENUM_GLOBAL | WNNC_ENUM_LOCAL;
        case WNNC_START:
            return WNNC_START_DONE;
        default:
            return 0;
    }
}

/**
 * Connects a shared folder, to a drive letter or deviceless.
 *
 * Deviceless connections need nothing: VBoxSF connects UNC names on use.
 */
SPIENTRY NPAddConnection(HWND hwndOwner, LPNETRESOURCE pNetResource, LPSTR pszPassword, LPSTR pszUserID,
                         DWORD dwFlags, LPSTR pszAccessName, LPDWORD pcbAccessName, LPDWORD pfResult)
{
    const char *pszAfter;
    char szRemote[VBOXNP_MAX_REMOTE];
    char szLocal[3];
    char chDrive;
    WCHAR *pwszNames;
    const WCHAR *pwsz;
    BOOL fFound = FALSE;
    VBOXNPINT21 Req;
    unsigned uErr;
    DWORD dwStatus;

    (void)hwndOwner; (void)pszPassword; (void)pszUserID; (void)dwFlags; (void)pszAccessName; (void)pcbAccessName;
    (void)pfResult;

    if (!pNetResource || pNetResource->dwType == RESOURCETYPE_PRINT)
        return WN_BAD_NETNAME;
    pszAfter = vboxNpSkipServer(pNetResource->lpRemoteName);
    if (!pszAfter || pszAfter[0] != '\\' || !pszAfter[1] || strchr(pszAfter + 1, '\\'))
        return WN_BAD_NETNAME;

    /* The shared folder must exist (the names compared as the host has them). */
    dwStatus = vboxNpQueryNames(&pwszNames);
    if (dwStatus != WN_SUCCESS)
        return dwStatus;
    for (pwsz = pwszNames; *pwsz && !fFound; pwsz += lstrlenW(pwsz) + 1)
        fFound = vboxNpShareRemoteName(pwsz, szRemote) && !lstrcmpi(szRemote + 2 + VBOXNP_SERVER_CCH, pszAfter);
    HeapFree(GetProcessHeap(), 0, pwszNames);
    if (!fFound)
        return WN_BAD_NETNAME;

    if (!pNetResource->lpLocalName || !pNetResource->lpLocalName[0])
        return WN_SUCCESS;
    chDrive = vboxNpDriveLetter(pNetResource->lpLocalName);
    if (!chDrive)
        return WN_BAD_LOCALNAME;

    /* INT 21h 5F03h: OEM names, the password after the remote name (none). */
    szLocal[0] = chDrive;
    szLocal[1] = ':';
    szLocal[2] = '\0';
    lstrcpy(szRemote, "\\\\" VBOXNP_SERVER_NAME);
    CharToOemBuff(pszAfter, szRemote + 2 + VBOXNP_SERVER_CCH, lstrlen(pszAfter) + 1);
    szRemote[lstrlen(szRemote) + 1] = '\0';
    memset(&Req, 0, sizeof(Req));
    Req.dwEax = 0x5f03;
    Req.dwEbx = 4;                          /* Disk. */
    Req.dwEsi = (DWORD)szLocal;
    Req.dwEdi = (DWORD)szRemote;
    uErr = vboxNpInt21(&Req);
    switch (uErr)
    {
        case 0:
            return WN_SUCCESS;
        case ERROR_ALREADY_ASSIGNED:
            return WN_ALREADY_CONNECTED;
        case ERROR_INVALID_DRIVE:
            return WN_BAD_LOCALNAME;
        case ERROR_BAD_NETPATH:
        case ERROR_BAD_NET_NAME:
            return WN_BAD_NETNAME;
        default:
            return uErr;
    }
}

/**
 * Disconnects one of our drive letters.  Deviceless connections have
 * nothing to cancel.
 */
SPIENTRY NPCancelConnection(LPSTR pszName, BOOL fForce, DWORD dwFlags)
{
    char szRemote[VBOXNP_MAX_REMOTE];
    char szLocal[3];
    char chDrive;
    VBOXNPINT21 Req;

    (void)fForce; (void)dwFlags;

    if (vboxNpSkipServer(pszName))
        return WN_SUCCESS;
    chDrive = vboxNpDriveLetter(pszName);
    if (!chDrive || vboxNpFindRedirection(chDrive, szRemote) != WN_SUCCESS)
        return WN_NOT_CONNECTED;
    szLocal[0] = chDrive;
    szLocal[1] = ':';
    szLocal[2] = '\0';
    memset(&Req, 0, sizeof(Req));
    Req.dwEax = 0x5f04;
    Req.dwEsi = (DWORD)szLocal;
    return vboxNpInt21(&Req) ? WN_NOT_CONNECTED : WN_SUCCESS;
}

/**
 * Gets the remote name of one of our drive letters.
 */
SPIENTRY NPGetConnection(LPSTR pszLocalName, LPSTR pszRemoteName, LPDWORD pcbRemoteName)
{
    char szRemote[VBOXNP_MAX_REMOTE];
    char chDrive = vboxNpDriveLetter(pszLocalName);

    if (!chDrive)
        return WN_BAD_LOCALNAME;
    if (vboxNpFindRedirection(chDrive, szRemote) != WN_SUCCESS)
        return WN_NOT_CONNECTED;
    return vboxNpCopyOut(szRemote, pszRemoteName, pcbRemoteName);
}

/**
 * Opens an enumeration: our drive letters (RESOURCE_CONNECTED), \\VBOXSVR
 * (the top of the network) or the shared folders (\\VBOXSVR).  The list is
 * taken now and handed out by NPEnumResource.
 */
SPIENTRY NPOpenEnum(DWORD dwScope, DWORD dwType, DWORD dwUsage, LPNETRESOURCE pNetResource, LPHANDLE phEnum)
{
    VBOXNPENUM *pEnum;
    BOOL fDisk = !dwType || (dwType & RESOURCETYPE_DISK);

    *phEnum = NULL;
    if (!dwUsage)
        dwUsage = RESOURCEUSAGE_CONNECTABLE | RESOURCEUSAGE_CONTAINER;
    pEnum = (VBOXNPENUM *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*pEnum));
    if (!pEnum)
        return WN_OUT_OF_MEMORY;

    if (dwScope == RESOURCE_CONNECTED)
    {
        DWORD iEntry;
        for (iEntry = 0; iEntry < VBOXNP_MAX_REDIRECTIONS && pEnum->cRes < VBOXNP_MAX_SHARES && fDisk; iEntry++)
        {
            char szRemote[VBOXNP_MAX_REMOTE];
            char chDrive;
            DWORD dwStatus = vboxNpGetRedirection(iEntry, &chDrive, szRemote);
            if (dwStatus == WN_NO_MORE_ENTRIES)
                break;
            if (dwStatus == WN_SUCCESS)
                vboxNpShareRes(&pEnum->aRes[pEnum->cRes++], RESOURCE_CONNECTED, szRemote, chDrive);
        }
    }
    else if (dwScope == RESOURCE_GLOBALNET)
    {
        const char *pszAfter = NULL;
        if (pNetResource && pNetResource->lpRemoteName)
        {
            pszAfter = vboxNpSkipServer(pNetResource->lpRemoteName);
            if (!pszAfter || (pszAfter[0] && (pszAfter[0] != '\\' || pszAfter[1])))
            {
                HeapFree(GetProcessHeap(), 0, pEnum);
                return WN_NOT_CONTAINER;
            }
        }
        if (!pszAfter)
        {
            /* The top of the network: the server. */
            if (dwUsage & RESOURCEUSAGE_CONTAINER)
                vboxNpServerRes(&pEnum->aRes[pEnum->cRes++], RESOURCE_GLOBALNET);
        }
        else if (fDisk && (dwUsage & RESOURCEUSAGE_CONNECTABLE))
        {
            WCHAR *pwszNames;
            const WCHAR *pwsz;
            DWORD dwStatus = vboxNpQueryNames(&pwszNames);
            if (dwStatus != WN_SUCCESS)
            {
                HeapFree(GetProcessHeap(), 0, pEnum);
                return dwStatus;
            }
            for (pwsz = pwszNames; *pwsz && pEnum->cRes < VBOXNP_MAX_SHARES; pwsz += lstrlenW(pwsz) + 1)
            {
                char szRemote[VBOXNP_MAX_REMOTE];
                if (vboxNpShareRemoteName(pwsz, szRemote))
                    vboxNpShareRes(&pEnum->aRes[pEnum->cRes++], RESOURCE_GLOBALNET, szRemote, 0);
            }
            HeapFree(GetProcessHeap(), 0, pwszNames);
        }
    }
    else
    {
        HeapFree(GetProcessHeap(), 0, pEnum);
        return WN_NOT_SUPPORTED;
    }

    *phEnum = (HANDLE)pEnum;
    return WN_SUCCESS;
}

/**
 * Returns the next entries of an enumeration: the structures at the start
 * of the buffer, their strings at the end, the space left in between.
 */
SPIENTRY NPEnumResource(HANDLE hEnum, LPDWORD pcCount, LPVOID pvBuffer, DWORD cbBuffer, LPDWORD pcbFree)
{
    VBOXNPENUM *pEnum = (VBOXNPENUM *)hEnum;
    NETRESOURCE *pNr = (NETRESOURCE *)pvBuffer;
    char *pchEnd = (char *)pvBuffer + cbBuffer;
    DWORD cbFree = cbBuffer;
    DWORD cWanted, cDone = 0;

    if (!pEnum)
        return WN_BAD_HANDLE;
    cWanted = *pcCount;
    while (cDone < cWanted && pEnum->iNext < pEnum->cRes)
    {
        const VBOXNPRES *pRes = &pEnum->aRes[pEnum->iNext];
        DWORD cb = vboxNpResSize(pRes);
        if (cb > cbFree)
        {
            if (!cDone)
            {
                *pcbFree = cb;
                return WN_MORE_DATA;
            }
            break;
        }
        vboxNpResPut(pNr++, &pchEnd, pRes);
        cbFree -= cb;
        cDone++;
        pEnum->iNext++;
    }
    *pcCount = cDone;
    *pcbFree = cbFree;
    return cDone ? WN_SUCCESS : WN_NO_MORE_ENTRIES;
}

/**
 * Closes an enumeration.
 */
SPIENTRY NPCloseEnum(HANDLE hEnum)
{
    if (!hEnum)
        return WN_BAD_HANDLE;
    HeapFree(GetProcessHeap(), 0, hEnum);
    return WN_SUCCESS;
}

/**
 * Describes \\VBOXSVR, \\VBOXSVR\name or a path below it (lplpSystem gets
 * the path).
 */
SPIENTRY NPGetResourceInformation(LPNETRESOURCE pNetResource, LPVOID pvBuffer, LPDWORD pcbBuffer, LPSTR *ppszSystem)
{
    const char *pszAfter;
    const char *pszPath;
    VBOXNPRES Res;
    char szRemote[VBOXNP_MAX_REMOTE];
    int cchShare;

    if (ppszSystem)
        *ppszSystem = NULL;
    if (!pNetResource || !pcbBuffer)
        return WN_BAD_VALUE;
    pszAfter = vboxNpSkipServer(pNetResource->lpRemoteName);
    if (!pszAfter)
        return WN_BAD_NETNAME;
    if (pNetResource->dwType && pNetResource->dwType != RESOURCETYPE_DISK && pNetResource->dwType != RESOURCETYPE_ANY)
        return WN_BAD_DEV_TYPE;

    if (!pszAfter[0] || !pszAfter[1])
    {
        vboxNpServerRes(&Res, 0);
        return vboxNpReturnRes(&Res, NULL, NULL, pvBuffer, pcbBuffer);
    }

    /* \\VBOXSVR\name, maybe with a path. */
    pszPath = strchr(pszAfter + 1, '\\');
    cchShare = pszPath ? (int)(pszPath - pszAfter) : lstrlen(pszAfter);
    if (cchShare > 1 + VBOXNP_MAX_NAME * 2)
        return WN_BAD_NETNAME;
    lstrcpy(szRemote, "\\\\" VBOXNP_SERVER_NAME);
    memcpy(szRemote + 2 + VBOXNP_SERVER_CCH, pszAfter, cchShare);
    szRemote[2 + VBOXNP_SERVER_CCH + cchShare] = '\0';
    vboxNpShareRes(&Res, 0, szRemote, 0);
    return vboxNpReturnRes(&Res, pszPath, ppszSystem, pvBuffer, pcbBuffer);
}

/**
 * Gives the browse parent: the shared folder for a path in it, the server
 * for a shared folder, the network for the server.
 */
SPIENTRY NPGetResourceParent(LPNETRESOURCE pNetResource, LPVOID pvBuffer, LPDWORD pcbBuffer)
{
    const char *pszAfter;
    const char *pszPath;
    VBOXNPRES Res;
    char *pszSystem;

    if (!pNetResource || !pcbBuffer)
        return WN_BAD_VALUE;
    pszAfter = vboxNpSkipServer(pNetResource->lpRemoteName);
    if (!pszAfter)
        return WN_BAD_NETNAME;
    pszPath = pszAfter[0] && pszAfter[1] ? strchr(pszAfter + 1, '\\') : NULL;
    if (pszPath && pszPath[1])
    {
        NETRESOURCE Share = *pNetResource;
        Share.dwType = 0;
        return NPGetResourceInformation(&Share, pvBuffer, pcbBuffer, &pszSystem);
    }
    if (pszAfter[0] && pszAfter[1])
        vboxNpServerRes(&Res, 0);
    else
    {
        /* The network itself, named after the provider as MPR names it. */
        memset(&Res, 0, sizeof(Res));
        Res.dwType        = RESOURCETYPE_ANY;
        Res.dwDisplayType = RESOURCEDISPLAYTYPE_NETWORK;
        Res.dwUsage       = RESOURCEUSAGE_CONTAINER;
        lstrcpy(Res.szRemote, VBOXNP_PROVIDER_NAME);
    }
    return vboxNpReturnRes(&Res, NULL, NULL, pvBuffer, pcbBuffer);
}

/**
 * Shortens a name shown inside its container to its last element
 * ("VBOXSVR" in the network, "name" in \\VBOXSVR).
 */
SPIENTRY NPFormatNetworkName(LPSTR pszRemoteName, LPSTR pszFormattedName, LPDWORD pcbFormattedName, DWORD dwFlags,
                             DWORD dwAveCharPerLine)
{
    const char *psz = pszRemoteName;

    (void)dwAveCharPerLine;
    if (!psz || !pcbFormattedName)
        return WN_BAD_VALUE;
    if (dwFlags & WNFMT_INENUM)
    {
        const char *pszLast = strrchr(psz, '\\');
        if (pszLast && pszLast[1])
            psz = pszLast + 1;
    }
    return vboxNpCopyOut(psz, pszFormattedName, pcbFormattedName);
}
