/** @file
 * VBoxTray - Guest properties with the OS, user and network information.
 *
 * Follows upstream VBoxServiceVMInfo.cpp: the host shows these in the
 * session information and VBoxManage guestproperty enumerate.
 *  - Once at start, without flags (they survive a VM reset):
 *    /VirtualBox/GuestInfo/OS/Product, Release, Version, ServicePack and
 *    /VirtualBox/GuestAdd/Version, VersionExt, Revision.  The VirtualBox
 *    version is reported, as in ReportGuestInfo2 (see VBoxGuest98.h).
 *  - Every interval (VBOXVMINFO_INTERVAL_MS, or the host options
 *    --vminfo-interval in ms or the global --interval in seconds, see
 *    HostOpts.c), with the TRANSRESET flag (TRANSIENT on
 *    hosts refusing it) and only when the value changed, except for the
 *    OS/LoggedInUsers and Net/Count beacons which are always written:
 *    OS/LoggedInUsersList, LoggedInUsers, NoLoggedInUsers and
 *    Net/Count, Net/<n>/V4/IP, V4/Broadcast, V4/Netmask, Status, MAC.
 *  - On exit the runtime properties are deleted, like upstream's property
 *    cache does with its temporary entries.
 *
 * Windows 98 has a single interactive user, the one GetUserName returns
 * (none if the logon was cancelled).  The network adapters come from
 * GetAdaptersInfo of the IP Helper API (iphlpapi.dll, loaded dynamically);
 * without it no network information is reported.  An adapter is "Up" when it
 * has an address, and dial-up adapters are only reported while connected.
 */

#include "VBoxTray.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/

/** How often to refresh the runtime properties by default (ms), as upstream. */
#define VBOXVMINFO_INTERVAL_MS      10000
/** Maximum number of runtime properties cached. */
#define VBOXVMINFO_MAX_PROPS        64
/** Maximum number of network adapters reported (5 properties each). */
#define VBOXVMINFO_MAX_NETS         10
/** Upper limit on a Net/Count left by an earlier run. */
#define VBOXVMINFO_MAX_OLD_NETS     64
/** The host options for this service. */
#define VBOXVMINFO_OPT_PATTERNS     VBOXHOSTOPTS_PREFIX "--vminfo-interval|" VBOXHOSTOPTS_PREFIX "--interval"
/** Prefix of the guest information properties. */
#define VBOXVMINFO_PREFIX           "/VirtualBox/GuestInfo/"

/** @name IP Helper adapter types (MIB_IF_TYPE_XXX).
 * @{ */
#define VBOXVMINFO_IF_TYPE_PPP      23
#define VBOXVMINFO_IF_TYPE_LOOPBACK 24
#define VBOXVMINFO_IF_TYPE_SLIP     28
/** @} */


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/

/** IP_ADDR_STRING of the IP Helper API (iptypes.h, not in the VC6 headers). */
typedef struct VBOXIPADDRSTRING
{
    struct VBOXIPADDRSTRING *Next;
    char  IpAddress[16];
    char  IpMask[16];
    DWORD Context;
} VBOXIPADDRSTRING;

/** IP_ADAPTER_INFO of the IP Helper API. */
typedef struct VBOXIPADAPTERINFO
{
    struct VBOXIPADAPTERINFO *Next;
    DWORD ComboIndex;
    char  AdapterName[256 + 4];
    char  Description[128 + 4];
    UINT  AddressLength;
    BYTE  Address[8];
    DWORD Index;
    UINT  Type;
    UINT  DhcpEnabled;
    VBOXIPADDRSTRING *CurrentIpAddress;
    VBOXIPADDRSTRING IpAddressList;
    VBOXIPADDRSTRING GatewayList;
    VBOXIPADDRSTRING DhcpServer;
    BOOL  HaveWins;
    VBOXIPADDRSTRING PrimaryWinsServer;
    VBOXIPADDRSTRING SecondaryWinsServer;
    long  LeaseObtained;
    long  LeaseExpires;
} VBOXIPADAPTERINFO;

/** Settings from the host options. */
typedef struct VBOXVMINFOOPTS
{
    /** --vminfo-interval (ms); 0 if not set. */
    DWORD cMsInterval;
    /** --interval (s); 0 if not set. */
    DWORD cSecInterval;
} VBOXVMINFOOPTS;

typedef DWORD (WINAPI *PFNGETADAPTERSINFO)(VBOXIPADAPTERINFO *pAdapterInfo, ULONG *pcbOutBufLen);

/** A runtime property written to the host. */
typedef struct VBOXVMINFOPROP
{
    char  szName[GUEST_PROP_MAX_NAME_LEN];
    /** The value last written (malloc'ed). */
    char *pszValue;
} VBOXVMINFOPROP;


/*********************************************************************************************************************************
*   Global Variables                                                                                                             *
*********************************************************************************************************************************/

/** Signaled by VBoxVMInfoStop to wake the thread up. */
static HANDLE g_hVMInfoEvent;
static HANDLE g_hVMInfoDevice = INVALID_HANDLE_VALUE;
static UINT32 g_idVMInfoClient;
/** The flags for the runtime properties, "TRANSIENT" on hosts without TRANSRESET. */
static const char *g_pszVMInfoFlags = "TRANSRESET";
/** The runtime properties written. */
static VBOXVMINFOPROP g_aVMInfoProps[VBOXVMINFO_MAX_PROPS];
/** The host options known. */
static const VBOXHOSTOPT g_aVMInfoOpts[] =
{
    { "--vminfo-interval",  offsetof(VBOXVMINFOOPTS, cMsInterval),  TRUE, 50, 0xfffffffe },
    { "--interval",         offsetof(VBOXVMINFOOPTS, cSecInterval), TRUE, 1,  0xffffffff / 1000 - 1 }
};
/** The option list last read. */
static VBOXHOSTOPTSCACHE g_VMInfoOptsCache;
static unsigned g_cVMInfoProps;
/** Number of network adapters reported last (or by an earlier run). */
static unsigned g_cVMInfoNets;
/** Status of the last failed write, so failures are only logged once. */
static int g_rcVMInfoLast = VINF_SUCCESS;


/**
 * Writes or deletes a runtime property, skipping writes that don't change it.
 *
 * @param   pszName     The property name.
 * @param   pszValue    The value (UTF-8), NULL to delete the property.
 * @param   fAlways     Whether to write it even if unchanged (the beacons
 *                      the host watches).
 */
static void vboxVMInfoUpdate(const char *pszName, const char *pszValue, BOOL fAlways)
{
    VBOXVMINFOPROP *pProp = NULL;
    unsigned i;
    int rc;

    for (i = 0; i < g_cVMInfoProps; i++)
        if (!strcmp(g_aVMInfoProps[i].szName, pszName))
        {
            pProp = &g_aVMInfoProps[i];
            break;
        }

    if (!pszValue)
    {
        VBoxGuestPropWrite(g_hVMInfoDevice, g_idVMInfoClient, pszName, NULL, NULL);
        if (pProp)
        {
            free(pProp->pszValue);
            *pProp = g_aVMInfoProps[--g_cVMInfoProps];
        }
        return;
    }
    if (pProp && !fAlways && !strcmp(pProp->pszValue, pszValue))
        return;

    rc = VBoxGuestPropWrite(g_hVMInfoDevice, g_idVMInfoClient, pszName, pszValue, g_pszVMInfoFlags);
    if (RT_FAILURE(rc) && !strcmp(g_pszVMInfoFlags, "TRANSRESET"))
    {
        /* Host without the TRANSRESET flag: TRANSIENT is better than nothing (as upstream). */
        rc = VBoxGuestPropWrite(g_hVMInfoDevice, g_idVMInfoClient, pszName, pszValue, "TRANSIENT");
        if (RT_SUCCESS(rc))
        {
            VBoxTrayLogRel("VMInfo: the host refused TRANSRESET, using TRANSIENT\n");
            g_pszVMInfoFlags = "TRANSIENT";
        }
    }
    if (RT_FAILURE(rc))
    {
        if (rc != g_rcVMInfoLast)
            VBoxTrayLogRel("VMInfo: writing %s failed, rc=%d\n", pszName, rc);
        g_rcVMInfoLast = rc;
        return; /* Not cached, retried next time. */
    }
    g_rcVMInfoLast = VINF_SUCCESS;

    if (!pProp)
    {
        if (g_cVMInfoProps >= VBOXVMINFO_MAX_PROPS)
            return;
        pProp = &g_aVMInfoProps[g_cVMInfoProps++];
        lstrcpyn(pProp->szName, pszName, sizeof(pProp->szName));
        pProp->pszValue = NULL;
    }
    free(pProp->pszValue);
    pProp->pszValue = _strdup(pszValue);
    if (!pProp->pszValue)
        *pProp = g_aVMInfoProps[--g_cVMInfoProps];
}

/**
 * Writes the properties that don't change while Windows runs.
 */
static void vboxVMInfoWriteFixed(void)
{
    OSVERSIONINFO Ver;
    const char *pszProduct;
    const char *pszCsd;
    char szTmp[64];
    UINT32 uBuild;
    int cch;

    memset(&Ver, 0, sizeof(Ver));
    Ver.dwOSVersionInfoSize = sizeof(Ver);
    GetVersionEx(&Ver);
    uBuild = LOWORD(Ver.dwBuildNumber); /* The high word has the version on Windows 9x. */
    if (Ver.dwMinorVersion >= 90)
        pszProduct = "Windows Me";
    else if (uBuild >= 2222)
        pszProduct = "Windows 98 SE";
    else
        pszProduct = "Windows 98";
    VBoxGuestPropWrite(g_hVMInfoDevice, g_idVMInfoClient, VBOXVMINFO_PREFIX "OS/Product", pszProduct, NULL);

    wsprintf(szTmp, "%lu.%lu.%u", Ver.dwMajorVersion, Ver.dwMinorVersion, uBuild);
    VBoxGuestPropWrite(g_hVMInfoDevice, g_idVMInfoClient, VBOXVMINFO_PREFIX "OS/Release", szTmp, NULL);
    wsprintf(szTmp, "Build %u", uBuild);
    VBoxGuestPropWrite(g_hVMInfoDevice, g_idVMInfoClient, VBOXVMINFO_PREFIX "OS/Version", szTmp, NULL);

    /* szCSDVersion is " A " on Windows 98 SE, blank otherwise. */
    for (pszCsd = Ver.szCSDVersion; *pszCsd == ' '; pszCsd++)
        ;
    lstrcpyn(szTmp, pszCsd, sizeof(szTmp));
    for (cch = lstrlen(szTmp); cch > 0 && szTmp[cch - 1] == ' '; cch--)
        szTmp[cch - 1] = '\0';
    VBoxGuestPropWrite(g_hVMInfoDevice, g_idVMInfoClient, VBOXVMINFO_PREFIX "OS/ServicePack", szTmp, NULL);

    VBoxGuestPropWrite(g_hVMInfoDevice, g_idVMInfoClient, "/VirtualBox/GuestAdd/Version", VBOX_VERSION_STRING, NULL);
    VBoxGuestPropWrite(g_hVMInfoDevice, g_idVMInfoClient, "/VirtualBox/GuestAdd/VersionExt", VBOX_VERSION_STRING, NULL);
    wsprintf(szTmp, "%u", VBOX_SVN_REV);
    VBoxGuestPropWrite(g_hVMInfoDevice, g_idVMInfoClient, "/VirtualBox/GuestAdd/Revision", szTmp, NULL);
}

/**
 * Updates the logged in user properties.
 */
static void vboxVMInfoUpdateUsers(void)
{
    char szUser[256];
    DWORD cchUser = sizeof(szUser);
    char *pszUser = NULL;

    if (GetUserName(szUser, &cchUser) && szUser[0])
        pszUser = VBoxTrayAnsiToUtf8(szUser);

    vboxVMInfoUpdate(VBOXVMINFO_PREFIX "OS/LoggedInUsersList", pszUser, FALSE);
    vboxVMInfoUpdate(VBOXVMINFO_PREFIX "OS/LoggedInUsers", pszUser ? "1" : "0", TRUE);
    vboxVMInfoUpdate(VBOXVMINFO_PREFIX "OS/NoLoggedInUsers", pszUser ? "false" : "true", FALSE);
    free(pszUser);
}

/**
 * Parses a dotted IPv4 address.
 *
 * @returns The address in host byte order, 0 if invalid.
 */
static DWORD vboxVMInfoParseIp(const char *psz)
{
    DWORD uAddr = 0;
    unsigned i;

    for (i = 0; i < 4; i++)
    {
        DWORD uPart = 0;
        if (*psz < '0' || *psz > '9')
            return 0;
        while (*psz >= '0' && *psz <= '9')
            uPart = uPart * 10 + (*psz++ - '0');
        if (uPart > 255 || (i < 3 ? *psz++ != '.' : *psz != '\0'))
            return 0;
        uAddr = (uAddr << 8) | uPart;
    }
    return uAddr;
}

/**
 * Writes a Net/<n>/<pszKey> property with an IPv4 address.
 */
static void vboxVMInfoUpdateIp(unsigned iNet, const char *pszKey, DWORD uAddr)
{
    char szName[GUEST_PROP_MAX_NAME_LEN];
    char szValue[16];

    wsprintf(szName, VBOXVMINFO_PREFIX "Net/%u/%s", iNet, pszKey);
    wsprintf(szValue, "%u.%u.%u.%u", uAddr >> 24, (uAddr >> 16) & 0xff, (uAddr >> 8) & 0xff, uAddr & 0xff);
    vboxVMInfoUpdate(szName, szValue, FALSE);
}

/**
 * Deletes the properties of network adapter iNet.
 */
static void vboxVMInfoDeleteNet(unsigned iNet)
{
    static const char * const s_apszKeys[] = { "V4/IP", "V4/Broadcast", "V4/Netmask", "Status", "MAC" };
    char szName[GUEST_PROP_MAX_NAME_LEN];
    unsigned i;

    for (i = 0; i < sizeof(s_apszKeys) / sizeof(s_apszKeys[0]); i++)
    {
        wsprintf(szName, VBOXVMINFO_PREFIX "Net/%u/%s", iNet, s_apszKeys[i]);
        vboxVMInfoUpdate(szName, NULL, FALSE);
    }
}

/**
 * Updates the network adapter properties.
 *
 * @param   pfnGetAdaptersInfo  GetAdaptersInfo, NULL if not available.
 */
static void vboxVMInfoUpdateNetwork(PFNGETADAPTERSINFO pfnGetAdaptersInfo)
{
    VBOXIPADAPTERINFO *paAdapters = NULL;
    VBOXIPADAPTERINFO *pAdapter;
    ULONG cbAdapters = 0;
    unsigned cNets = 0;
    unsigned i;
    char szName[GUEST_PROP_MAX_NAME_LEN];
    char szValue[16];

    if (!pfnGetAdaptersInfo)
        return;
    if (   pfnGetAdaptersInfo(NULL, &cbAdapters) == ERROR_BUFFER_OVERFLOW
        && cbAdapters
        && (paAdapters = (VBOXIPADAPTERINFO *)malloc(cbAdapters)) != NULL
        && pfnGetAdaptersInfo(paAdapters, &cbAdapters) != ERROR_SUCCESS)
    {
        free(paAdapters);
        paAdapters = NULL;
    }

    for (pAdapter = paAdapters; pAdapter && cNets < VBOXVMINFO_MAX_NETS; pAdapter = pAdapter->Next)
    {
        DWORD uIp   = vboxVMInfoParseIp(pAdapter->IpAddressList.IpAddress);
        DWORD uMask = vboxVMInfoParseIp(pAdapter->IpAddressList.IpMask);

        if (pAdapter->Type == VBOXVMINFO_IF_TYPE_LOOPBACK)
            continue;
        if (!uIp && (pAdapter->Type == VBOXVMINFO_IF_TYPE_PPP || pAdapter->Type == VBOXVMINFO_IF_TYPE_SLIP))
            continue; /* A dial-up adapter that isn't connected. */

        vboxVMInfoUpdateIp(cNets, "V4/IP", uIp);
        vboxVMInfoUpdateIp(cNets, "V4/Broadcast", uIp ? uIp | ~uMask : 0);
        vboxVMInfoUpdateIp(cNets, "V4/Netmask", uMask);

        wsprintf(szName, VBOXVMINFO_PREFIX "Net/%u/Status", cNets);
        vboxVMInfoUpdate(szName, uIp ? "Up" : "Down", FALSE);

        wsprintf(szName, VBOXVMINFO_PREFIX "Net/%u/MAC", cNets);
        if (pAdapter->AddressLength >= 6)
        {
            wsprintf(szValue, "%02X%02X%02X%02X%02X%02X", pAdapter->Address[0], pAdapter->Address[1],
                     pAdapter->Address[2], pAdapter->Address[3], pAdapter->Address[4], pAdapter->Address[5]);
            vboxVMInfoUpdate(szName, szValue, FALSE);
        }
        else
            vboxVMInfoUpdate(szName, NULL, FALSE);
        cNets++;
    }
    free(paAdapters);

    for (i = cNets; i < g_cVMInfoNets; i++)
        vboxVMInfoDeleteNet(i);
    g_cVMInfoNets = cNets;

    wsprintf(szValue, "%u", cNets);
    vboxVMInfoUpdate(VBOXVMINFO_PREFIX "Net/Count", szValue, TRUE);
}

/**
 * Creates the wake-up event.
 */
void VBoxVMInfoInit(void)
{
    g_hVMInfoEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
}

/**
 * Wakes up the thread so it sees g_fVBoxTrayShutdown.
 */
void VBoxVMInfoStop(void)
{
    if (g_hVMInfoEvent)
        SetEvent(g_hVMInfoEvent);
}

/**
 * Reads the host options.
 *
 * @param   pcMsInterval    The refresh interval, updated.
 */
static void vboxVMInfoReadOpts(DWORD *pcMsInterval)
{
    VBOXVMINFOOPTS Opts = { 0, 0 };
    DWORD cMsInterval;

    if (!VBoxHostOptsRead(g_hVMInfoDevice, g_idVMInfoClient, "VMInfo", VBOXVMINFO_OPT_PATTERNS, g_aVMInfoOpts,
                          sizeof(g_aVMInfoOpts) / sizeof(g_aVMInfoOpts[0]), &Opts, &g_VMInfoOptsCache))
        return;
    cMsInterval = Opts.cMsInterval  ? Opts.cMsInterval
                : Opts.cSecInterval ? Opts.cSecInterval * 1000
                : VBOXVMINFO_INTERVAL_MS;
    if (cMsInterval != *pcMsInterval)
    {
        VBoxTrayLogRel("VMInfo: interval %lu ms\n", cMsInterval);
        *pcMsInterval = cMsInterval;
    }
}

/**
 * Guest information thread.
 *
 * @returns 0 on normal exit, 1 if the driver or the service could not be used.
 * @param   pvUser      Unused.
 */
unsigned __stdcall VBoxVMInfoThread(void *pvUser)
{
    PFNGETADAPTERSINFO pfnGetAdaptersInfo = NULL;
    HMODULE hIpHlpApi;
    DWORD cMsInterval = VBOXVMINFO_INTERVAL_MS;
    DWORD msLastOpts;
    char szCount[16];
    int rc;

    if (!g_hVMInfoEvent)
        return 1;
    g_hVMInfoDevice = VbglR3Open();
    if (g_hVMInfoDevice == INVALID_HANDLE_VALUE)
        return 1;
    rc = VbglR3HgcmConnect(g_hVMInfoDevice, GUEST_PROP_SERVICE_NAME, &g_idVMInfoClient);
    if (RT_FAILURE(rc))
    {
        VBoxTrayLogRel("VMInfo: connecting to the guest property service failed, rc=%d\n", rc);
        VbglR3Close(g_hVMInfoDevice);
        return 1;
    }

    hIpHlpApi = LoadLibrary("iphlpapi.dll");
    if (hIpHlpApi)
        pfnGetAdaptersInfo = (PFNGETADAPTERSINFO)GetProcAddress(hIpHlpApi, "GetAdaptersInfo");
    if (!pfnGetAdaptersInfo)
        VBoxTrayLogRel("VMInfo: no IP Helper API (iphlpapi.dll), the network is not reported\n");

    vboxVMInfoReadOpts(&cMsInterval);
    msLastOpts = GetTickCount();
    vboxVMInfoWriteFixed();

    /* Adapters left by an earlier run (VBoxTray restarted without a VM reset) are deleted on the first update. */
    if (RT_SUCCESS(VBoxGuestPropRead(g_hVMInfoDevice, g_idVMInfoClient, VBOXVMINFO_PREFIX "Net/Count",
                                     szCount, sizeof(szCount))))
    {
        g_cVMInfoNets = (unsigned)strtoul(szCount, NULL, 10);
        if (g_cVMInfoNets > VBOXVMINFO_MAX_OLD_NETS)
            g_cVMInfoNets = VBOXVMINFO_MAX_OLD_NETS;
    }

    while (!g_fVBoxTrayShutdown)
    {
        DWORD msLastUpdate;

        vboxVMInfoUpdateUsers();
        vboxVMInfoUpdateNetwork(pfnGetAdaptersInfo);
        msLastUpdate = GetTickCount();

        /* Wait for the interval, reading the options again meanwhile (a new
           interval counts from the last update). */
        while (!g_fVBoxTrayShutdown)
        {
            DWORD cMsElapsed;
            DWORD cMsWait;

            if (GetTickCount() - msLastOpts >= VBOXHOSTOPTS_INTERVAL_MS)
            {
                vboxVMInfoReadOpts(&cMsInterval);
                msLastOpts = GetTickCount();
            }
            cMsElapsed = GetTickCount() - msLastUpdate;
            if (cMsElapsed >= cMsInterval)
                break;
            cMsWait = cMsInterval - cMsElapsed;
            if (cMsWait > VBOXHOSTOPTS_INTERVAL_MS - (GetTickCount() - msLastOpts))
                cMsWait = VBOXHOSTOPTS_INTERVAL_MS - (GetTickCount() - msLastOpts);
            WaitForSingleObject(g_hVMInfoEvent, cMsWait);
        }
    }

    /* Delete the runtime properties, newest first so the cache shrinks from the end. */
    while (g_cVMInfoProps)
    {
        char szName[GUEST_PROP_MAX_NAME_LEN];
        lstrcpyn(szName, g_aVMInfoProps[g_cVMInfoProps - 1].szName, sizeof(szName));
        vboxVMInfoUpdate(szName, NULL, FALSE);
    }

    if (hIpHlpApi)
        FreeLibrary(hIpHlpApi);
    VBoxHostOptsFree(&g_VMInfoOptsCache);
    VbglR3HgcmDisconnect(g_hVMInfoDevice, g_idVMInfoClient);
    VbglR3Close(g_hVMInfoDevice);
    return 0;
}
