/** @file
 * VBoxControl - Guest Additions command line tool for Windows 98/ME.
 *
 * The commands of the official VBoxControl that make sense here: guest
 * properties (get, set, delete, enumerate, wait), the shared folders list,
 * writing to the VM's log and the version.  The exit codes are upstream's:
 * 0 success, 1 failure (including "not set" and timeouts), 2 syntax error.
 *
 * Text: the command line arrives in the ANSI code page (Windows converts the
 * MS-DOS prompt's OEM text) and the host's is UTF-8; the output is in the OEM
 * code page, like any MS-DOS prompt program, also when redirected.
 *
 * Windows 9x only delivers Ctrl+C to a console program when it returns from
 * the driver, so "guestproperty wait" waits in short slices.
 */

#include "VBoxTray.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/

/** Exit codes (RTEXITCODE). */
#define VBOXCTRL_EXIT_SUCCESS       0
#define VBOXCTRL_EXIT_FAILURE       1
#define VBOXCTRL_EXIT_SYNTAX        2

/** Longest single wait in the driver (ms), so Ctrl+C gets through. */
#define VBOXCTRL_WAIT_SLICE_MS      500
/** Size of the shared folder strings asked for (bytes, header included). */
#define VBOXCTRL_CB_SHFL_STRING     (SHFLSTRING_HEADER_SIZE + (SHFL_MAX_LEN + 1) * sizeof(WCHAR))


/*********************************************************************************************************************************
*   Global Variables                                                                                                             *
*********************************************************************************************************************************/

static HANDLE g_hDevice = INVALID_HANDLE_VALUE;


/*********************************************************************************************************************************
*   Helpers                                                                                                                      *
*********************************************************************************************************************************/

static void vboxCtrlUsage(void)
{
    printf("Usage:\n"
           "\n"
           "VBoxControl [-V|--version]           print version number and exit\n"
           "VBoxControl --nologo ...             suppress the logo\n"
           "\n"
           "VBoxControl guestproperty get <property> [--verbose]\n"
           "VBoxControl guestproperty set <property> [<value> [--flags <flags>]]\n"
           "VBoxControl guestproperty delete|unset <property>\n"
           "VBoxControl guestproperty enumerate [--patterns <patterns>]\n"
           "VBoxControl guestproperty wait <patterns> [--timestamp <last timestamp>]\n"
           "                                          [--timeout <timeout in ms>]\n"
           "VBoxControl sharedfolder list [--automount]\n"
           "VBoxControl writelog [-n|--no-newline] [--] <msg>\n"
           "VBoxControl version\n"
           "VBoxControl help [command]\n");
}

static int vboxCtrlSyntaxError(const char *pszMsg, const char *pszArg)
{
    printf("Syntax error: %s%s%s\n\n", pszMsg, pszArg ? ": " : "", pszArg ? pszArg : "");
    vboxCtrlUsage();
    return VBOXCTRL_EXIT_SYNTAX;
}

static int vboxCtrlError(const char *pszWhat, int rc)
{
    printf("Error: %s failed: rc=%d\n", pszWhat, rc);
    return VBOXCTRL_EXIT_FAILURE;
}

/** Matches an option given with one or two dashes, as upstream accepts. */
static BOOL vboxCtrlIsOption(const char *pszArg, const char *pszName)
{
    if (pszArg[0] != '-')
        return FALSE;
    if (pszArg[1] == '-')
        pszArg++;
    return !strcmp(pszArg + 1, pszName);
}

/** Prints UTF-8 text from the host in the OEM code page. */
static void vboxCtrlPrintUtf8(const char *pszFormat, const char *pszUtf8)
{
    char *psz = VBoxTrayUtf8ToAnsi(pszUtf8, (DWORD)strlen(pszUtf8), '?');
    if (psz)
    {
        CharToOemA(psz, psz);
        printf(pszFormat, psz);
        free(psz);
    }
}

/** Converts a command line argument to UTF-8; exits if out of memory. */
static char *vboxCtrlArgToUtf8(const char *pszArg)
{
    char *psz = VBoxTrayAnsiToUtf8(pszArg);
    if (!psz)
    {
        printf("Error: out of memory\n");
        exit(VBOXCTRL_EXIT_FAILURE);
    }
    return psz;
}

/** Opens the driver the first time a command needs it. */
static BOOL vboxCtrlOpenDevice(void)
{
    if (g_hDevice == INVALID_HANDLE_VALUE)
        g_hDevice = VbglR3Open();
    if (g_hDevice == INVALID_HANDLE_VALUE)
    {
        printf("Error: could not open the VBoxGst driver (error %lu); are the Guest Additions installed?\n",
               GetLastError());
        return FALSE;
    }
    return TRUE;
}

/** Connects to an HGCM service, printing the error. */
static BOOL vboxCtrlConnect(const char *pszService, UINT32 *pidClient)
{
    int rc;
    if (!vboxCtrlOpenDevice())
        return FALSE;
    rc = VbglR3HgcmConnect(g_hDevice, pszService, pidClient);
    if (RT_FAILURE(rc))
    {
        printf("Error: could not connect to %s: rc=%d\n", pszService, rc);
        return FALSE;
    }
    return TRUE;
}


/*********************************************************************************************************************************
*   guestproperty                                                                                                                *
*********************************************************************************************************************************/

static int vboxCtrlPropGet(int argc, char **argv)
{
    char szValue[GUEST_PROP_MAX_VALUE_LEN];
    char szFlags[GUEST_PROP_MAX_FLAGS_LEN];
    UINT64 uTimestamp = 0;
    BOOL fVerbose = FALSE;
    char *pszName;
    UINT32 idClient;
    int rc;

    if (argc == 2 && vboxCtrlIsOption(argv[1], "verbose"))
        fVerbose = TRUE;
    else if (argc != 1)
        return vboxCtrlSyntaxError("get takes a property name and --verbose", NULL);
    if (!vboxCtrlConnect(GUEST_PROP_SERVICE_NAME, &idClient))
        return VBOXCTRL_EXIT_FAILURE;

    pszName = vboxCtrlArgToUtf8(argv[0]);
    rc = VBoxGuestPropReadEx(g_hDevice, idClient, pszName, szValue, sizeof(szValue), &uTimestamp,
                             szFlags, sizeof(szFlags));
    free(pszName);
    VbglR3HgcmDisconnect(g_hDevice, idClient);
    if (rc == VERR_NOT_FOUND)
    {
        printf("No value set!\n");
        return VBOXCTRL_EXIT_FAILURE;
    }
    if (RT_FAILURE(rc))
        return vboxCtrlError("reading the property", rc);
    vboxCtrlPrintUtf8("Value: %s\n", szValue);
    if (fVerbose)
    {
        printf("Timestamp: %I64u ns\n", uTimestamp);
        printf("Flags: %s\n", szFlags);
    }
    return VBOXCTRL_EXIT_SUCCESS;
}

static int vboxCtrlPropSet(int argc, char **argv)
{
    const char *pszFlags = NULL;
    char *pszName;
    char *pszValue = NULL;
    UINT32 idClient;
    int rc;

    if (argc == 4 && vboxCtrlIsOption(argv[2], "flags"))
        pszFlags = argv[3];
    else if (argc != 1 && argc != 2)
        return vboxCtrlSyntaxError("set takes a property name, a value and --flags <flags>", NULL);
    if (!vboxCtrlConnect(GUEST_PROP_SERVICE_NAME, &idClient))
        return VBOXCTRL_EXIT_FAILURE;

    pszName = vboxCtrlArgToUtf8(argv[0]);
    if (argc > 1)
        pszValue = vboxCtrlArgToUtf8(argv[1]);
    /* No value deletes the property, as upstream. */
    rc = VBoxGuestPropWrite(g_hDevice, idClient, pszName, pszValue, pszFlags);
    free(pszName);
    free(pszValue);
    VbglR3HgcmDisconnect(g_hDevice, idClient);
    if (RT_FAILURE(rc))
        return vboxCtrlError("setting the property", rc);
    return VBOXCTRL_EXIT_SUCCESS;
}

static int vboxCtrlPropDelete(int argc, char **argv)
{
    char *pszName;
    UINT32 idClient;
    int rc;

    if (argc != 1)
        return vboxCtrlSyntaxError("delete takes a property name", NULL);
    if (!vboxCtrlConnect(GUEST_PROP_SERVICE_NAME, &idClient))
        return VBOXCTRL_EXIT_FAILURE;

    pszName = vboxCtrlArgToUtf8(argv[0]);
    rc = VBoxGuestPropWrite(g_hDevice, idClient, pszName, NULL, NULL);
    free(pszName);
    VbglR3HgcmDisconnect(g_hDevice, idClient);
    if (RT_FAILURE(rc))
        return vboxCtrlError("deleting the property", rc);
    return VBOXCTRL_EXIT_SUCCESS;
}

static int vboxCtrlPropEnum(int argc, char **argv)
{
    char *pszPatterns;
    char *pszList;
    const char *psz;
    UINT32 idClient;
    BOOL fAny = FALSE;
    int rc;

    if (argc == 0)
        pszPatterns = vboxCtrlArgToUtf8("");
    else if (argc == 2 && vboxCtrlIsOption(argv[0], "patterns"))
        pszPatterns = vboxCtrlArgToUtf8(argv[1]);
    else
        return vboxCtrlSyntaxError("enumerate takes --patterns <patterns>", NULL);
    if (!vboxCtrlConnect(GUEST_PROP_SERVICE_NAME, &idClient))
    {
        free(pszPatterns);
        return VBOXCTRL_EXIT_FAILURE;
    }

    rc = VBoxGuestPropEnum(g_hDevice, idClient, pszPatterns, &pszList);
    free(pszPatterns);
    VbglR3HgcmDisconnect(g_hDevice, idClient);
    if (RT_FAILURE(rc))
        return vboxCtrlError("listing the properties", rc);

    for (psz = pszList; *psz; )
    {
        const char *pszName      = psz;
        const char *pszValue     = pszName + strlen(pszName) + 1;
        const char *pszTimestamp = pszValue + strlen(pszValue) + 1;
        const char *pszFlags     = pszTimestamp + strlen(pszTimestamp) + 1;

        vboxCtrlPrintUtf8("Name: %s, ", pszName);
        vboxCtrlPrintUtf8("value: %s, ", pszValue);
        printf("timestamp: %s, flags: %s\n", pszTimestamp, pszFlags);
        fAny = TRUE;
        psz = pszFlags + strlen(pszFlags) + 1;
    }
    if (!fAny)
        printf("No properties found.\n");
    free(pszList);
    return VBOXCTRL_EXIT_SUCCESS;
}

static int vboxCtrlPropWait(int argc, char **argv)
{
    char szName[GUEST_PROP_MAX_NAME_LEN];
    char szValue[GUEST_PROP_MAX_VALUE_LEN];
    char szFlags[GUEST_PROP_MAX_FLAGS_LEN];
    UINT64 uTimestamp = 0;
    DWORD cMsTimeout = INFINITE;
    DWORD msStart;
    char *pszPatterns;
    UINT32 idClient;
    int i;
    int rc;

    if (argc < 1)
        return vboxCtrlSyntaxError("wait takes patterns", NULL);
    for (i = 1; i < argc; i++)
    {
        if (vboxCtrlIsOption(argv[i], "timestamp") && i + 1 < argc)
            uTimestamp = (UINT64)_atoi64(argv[++i]);
        else if (vboxCtrlIsOption(argv[i], "timeout") && i + 1 < argc)
            cMsTimeout = strtoul(argv[++i], NULL, 10);
        else
            return vboxCtrlSyntaxError("unknown option", argv[i]);
    }
    if (!vboxCtrlConnect(GUEST_PROP_SERVICE_NAME, &idClient))
        return VBOXCTRL_EXIT_FAILURE;

    /* A change between two slices is only missed with timestamp 0, and then
     * only in the moment between the calls. */
    pszPatterns = vboxCtrlArgToUtf8(argv[0]);
    msStart = GetTickCount();
    for (;;)
    {
        DWORD cMsSlice = VBOXCTRL_WAIT_SLICE_MS;
        if (cMsTimeout != INFINITE)
        {
            DWORD cMsElapsed = GetTickCount() - msStart;
            if (cMsElapsed >= cMsTimeout)
            {
                rc = VERR_TIMEOUT;
                break;
            }
            if (cMsTimeout - cMsElapsed < cMsSlice)
                cMsSlice = cMsTimeout - cMsElapsed;
        }
        rc = VBoxGuestPropWait(g_hDevice, idClient, pszPatterns, cMsSlice, &uTimestamp, szName, sizeof(szName),
                               szValue, sizeof(szValue), szFlags, sizeof(szFlags));
        if (rc != VERR_TIMEOUT)
            break;
    }
    free(pszPatterns);
    VbglR3HgcmDisconnect(g_hDevice, idClient);

    if (rc == VERR_TIMEOUT)
    {
        printf("Timeout\n");
        return VBOXCTRL_EXIT_FAILURE;
    }
    if (RT_FAILURE(rc))
        return vboxCtrlError("waiting for the property", rc);
    if (rc == VWRN_NOT_FOUND)
        printf("The timestamp was too old, changes were missed.\n");
    vboxCtrlPrintUtf8("Name: %s\n", szName);
    vboxCtrlPrintUtf8("Value: %s\n", szValue);
    printf("Timestamp: %I64u\n", uTimestamp);
    printf("Flags: %s\n", szFlags);
    return VBOXCTRL_EXIT_SUCCESS;
}

static int vboxCtrlGuestProperty(int argc, char **argv)
{
    if (argc < 1)
        return vboxCtrlSyntaxError("guestproperty takes a subcommand", NULL);
    if (!strcmp(argv[0], "get") && argc >= 2)
        return vboxCtrlPropGet(argc - 1, argv + 1);
    if (!strcmp(argv[0], "set") && argc >= 2)
        return vboxCtrlPropSet(argc - 1, argv + 1);
    if ((!strcmp(argv[0], "delete") || !strcmp(argv[0], "unset")) && argc >= 2)
        return vboxCtrlPropDelete(argc - 1, argv + 1);
    if (!strcmp(argv[0], "enumerate"))
        return vboxCtrlPropEnum(argc - 1, argv + 1);
    if (!strcmp(argv[0], "wait") && argc >= 2)
        return vboxCtrlPropWait(argc - 1, argv + 1);
    return vboxCtrlSyntaxError("unknown or incomplete guestproperty subcommand", argv[0]);
}


/*********************************************************************************************************************************
*   sharedfolder                                                                                                                 *
*********************************************************************************************************************************/

/** Prints a SHFLSTRING (UTF-16) in the OEM code page. */
static void vboxCtrlPrintShfl(const char *pszFormat, const SHFLSTRING *pStr)
{
    char sz[SHFL_MAX_LEN * 2 + 1];
    int cwc = pStr->u16Length / sizeof(WCHAR);

    if ((DWORD)pStr->u16Length + SHFLSTRING_HEADER_SIZE > VBOXCTRL_CB_SHFL_STRING)
        cwc = 0;
    cwc = WideCharToMultiByte(CP_OEMCP, 0, pStr->String.utf16, cwc, sz, sizeof(sz) - 1, "?", NULL);
    sz[cwc > 0 ? cwc : 0] = '\0';
    printf(pszFormat, sz);
}

static int vboxCtrlSharedFolderList(int argc, char **argv)
{
    static SHFLMAPPING s_aMappings[SHFL_MAX_MAPPINGS];
    BOOL fAutoMountOnly = FALSE;
    VBGLR3HGCMMSG Msg;
    UINT32 idClient;
    UINT32 cMappings = 0;
    UINT32 i;
    BYTE *pb;
    int rc;

    if (argc == 1 && !strcmp(argv[0], "--automount"))
        fAutoMountOnly = TRUE;
    else if (argc != 0)
        return vboxCtrlSyntaxError("list takes --automount", NULL);
    if (!vboxCtrlConnect("VBoxSharedFolders", &idClient))
        return VBOXCTRL_EXIT_FAILURE;

    rc = VERR_NO_MEMORY;
    if (VbglR3HgcmMsgInit(&Msg, idClient, SHFL_FN_QUERY_MAPPINGS, SHFL_FN_QUERY_MAPPINGS_CPARMS,
                          VBOXGUEST98_WAIT_INFINITE, sizeof(s_aMappings)))
    {
        VbglR3HgcmMsgSetU32(&Msg, 0, SHFL_MF_UCS2 | (fAutoMountOnly ? SHFL_MF_AUTOMOUNT : 0));
        VbglR3HgcmMsgSetU32(&Msg, 1, SHFL_MAX_MAPPINGS);
        pb = VbglR3HgcmMsgSetBuf(&Msg, 2, VBOXGUEST98_HGCM_PARM_BUFFER_OUT, sizeof(s_aMappings), NULL);
        rc = VbglR3HgcmMsgCall(g_hDevice, &Msg);
        if (RT_SUCCESS(rc))
        {
            cMappings = VbglR3HgcmMsgGetU32(&Msg, 1);
            if (cMappings > SHFL_MAX_MAPPINGS)
                cMappings = SHFL_MAX_MAPPINGS;
            memcpy(s_aMappings, pb, cMappings * sizeof(SHFLMAPPING));
        }
        VbglR3HgcmMsgFree(&Msg);
    }
    if (RT_FAILURE(rc))
    {
        VbglR3HgcmDisconnect(g_hDevice, idClient);
        return vboxCtrlError("listing the shared folders", rc);
    }

    if (!cMappings)
        printf("No Shared Folders available.\n");
    else
        printf("Shared Folder mappings (%u):\n\n", cMappings);
    for (i = 0; i < cMappings; i++)
    {
        SHFLSTRING *pName, *pMountPoint;
        UINT64 fFlags;

        if (!VbglR3HgcmMsgInit(&Msg, idClient, SHFL_FN_QUERY_MAP_INFO, SHFL_FN_QUERY_MAP_INFO_CPARMS,
                               VBOXGUEST98_WAIT_INFINITE, 2 * VBOXCTRL_CB_SHFL_STRING))
            break;
        VbglR3HgcmMsgSetU32(&Msg, 0, s_aMappings[i].root);
        pName = (SHFLSTRING *)VbglR3HgcmMsgSetBuf(&Msg, 1, VBOXGUEST98_HGCM_PARM_BUFFER_INOUT,
                                                  VBOXCTRL_CB_SHFL_STRING, NULL);
        pName->u16Size = VBOXCTRL_CB_SHFL_STRING - SHFLSTRING_HEADER_SIZE;
        pMountPoint = (SHFLSTRING *)VbglR3HgcmMsgSetBuf(&Msg, 2, VBOXGUEST98_HGCM_PARM_BUFFER_INOUT,
                                                        VBOXCTRL_CB_SHFL_STRING, NULL);
        pMountPoint->u16Size = VBOXCTRL_CB_SHFL_STRING - SHFLSTRING_HEADER_SIZE;
        VbglR3HgcmMsgSetU64(&Msg, 3, SHFL_MIQF_DRIVE_LETTER);
        VbglR3HgcmMsgSetU32(&Msg, 4, 0);
        rc = VbglR3HgcmMsgCall(g_hDevice, &Msg);
        if (RT_SUCCESS(rc))
        {
            fFlags = VbglR3HgcmMsgGetU64(&Msg, 3);
            printf("%02u - ", i + 1);
            vboxCtrlPrintShfl("%s", pName);
            printf(" [idRoot=%u %s", s_aMappings[i].root, fFlags & SHFL_MIF_WRITABLE ? "writable" : "readonly");
            if (fFlags & SHFL_MIF_AUTO_MOUNT)
                printf(" auto-mount");
            if (pMountPoint->u16Length)
                vboxCtrlPrintShfl(" mnt-pt=%s", pMountPoint);
            printf("]\n");
        }
        else
            printf("%02u - [idRoot=%u] error: rc=%d\n", i + 1, s_aMappings[i].root, rc);
        VbglR3HgcmMsgFree(&Msg);
    }
    VbglR3HgcmDisconnect(g_hDevice, idClient);
    return VBOXCTRL_EXIT_SUCCESS;
}

static int vboxCtrlSharedFolder(int argc, char **argv)
{
    if (argc >= 1 && !strcmp(argv[0], "list"))
        return vboxCtrlSharedFolderList(argc - 1, argv + 1);
    return vboxCtrlSyntaxError("unknown sharedfolder subcommand", argc ? argv[0] : NULL);
}


/*********************************************************************************************************************************
*   writelog, version                                                                                                            *
*********************************************************************************************************************************/

static int vboxCtrlWriteLog(int argc, char **argv)
{
    BOOL fNewline = TRUE;
    int i = 0;
    int rc = VINF_SUCCESS;

    for (; i < argc && argv[i][0] == '-'; i++)
    {
        if (!strcmp(argv[i], "-n") || !strcmp(argv[i], "--no-newline"))
            fNewline = FALSE;
        else if (!strcmp(argv[i], "--"))
        {
            i++;
            break;
        }
        else
            return vboxCtrlSyntaxError("unknown option", argv[i]);
    }
    if (!vboxCtrlOpenDevice())
        return VBOXCTRL_EXIT_FAILURE;

    /* Each argument is a message, as upstream. */
    for (; i < argc && RT_SUCCESS(rc); i++)
    {
        char *psz = vboxCtrlArgToUtf8(argv[i]);
        DWORD cch = (DWORD)strlen(psz);
        DWORD off;

        for (off = 0; off < cch && RT_SUCCESS(rc); off += VBOXGUEST98_MAX_LOG_SIZE)
            rc = VbglR3WriteLog(g_hDevice, psz + off, min(cch - off, VBOXGUEST98_MAX_LOG_SIZE));
        if (fNewline && RT_SUCCESS(rc))
            rc = VbglR3WriteLog(g_hDevice, "\n", 1);
        free(psz);
    }
    if (RT_FAILURE(rc))
        return vboxCtrlError("writing to the log", rc);
    return VBOXCTRL_EXIT_SUCCESS;
}

static int vboxCtrlVersion(void)
{
    printf("%s (VirtualBox %sr%u)\n", VBOXGUEST98_VERSION_STRING, VBOX_VERSION_STRING, VBOX_SVN_REV);
    return VBOXCTRL_EXIT_SUCCESS;
}


int __cdecl main(int argc, char **argv)
{
    BOOL fShowLogo = TRUE;
    const char *pszCmd;
    int iArg;
    int rcExit;

    for (iArg = 1; iArg < argc && argv[iArg][0] == '-'; iArg++)
    {
        if (   !strcmp(argv[iArg], "-V") || !strcmp(argv[iArg], "-v")
            || !strcmp(argv[iArg], "--version") || !strcmp(argv[iArg], "-version"))
            return vboxCtrlVersion();
        if (vboxCtrlIsOption(argv[iArg], "nologo"))
            fShowLogo = FALSE;
        else if (vboxCtrlIsOption(argv[iArg], "help"))
        {
            vboxCtrlUsage();
            return VBOXCTRL_EXIT_SUCCESS;
        }
        else
            return vboxCtrlSyntaxError("unknown option", argv[iArg]);
    }

    if (fShowLogo)
        printf("VBoxGuest98 Guest Additions Command Line Management Interface Version %s\n"
               "(VirtualBox %s)\n\n", VBOXGUEST98_VERSION_STRING, VBOX_VERSION_STRING);
    if (iArg >= argc)
    {
        vboxCtrlUsage();
        return VBOXCTRL_EXIT_SYNTAX;
    }

    pszCmd = argv[iArg++];
    if (!strcmp(pszCmd, "guestproperty"))
        rcExit = vboxCtrlGuestProperty(argc - iArg, argv + iArg);
    else if (!strcmp(pszCmd, "sharedfolder"))
        rcExit = vboxCtrlSharedFolder(argc - iArg, argv + iArg);
    else if (!strcmp(pszCmd, "writelog"))
        rcExit = vboxCtrlWriteLog(argc - iArg, argv + iArg);
    else if (!strcmp(pszCmd, "version") || !strcmp(pszCmd, "getversion"))
        rcExit = argc - iArg ? vboxCtrlSyntaxError("version takes no arguments", NULL) : vboxCtrlVersion();
    else if (!strcmp(pszCmd, "help"))
    {
        vboxCtrlUsage();
        rcExit = VBOXCTRL_EXIT_SUCCESS;
    }
    else
        rcExit = vboxCtrlSyntaxError("unknown command", pszCmd);

    if (g_hDevice != INVALID_HANDLE_VALUE)
        VbglR3Close(g_hDevice);
    return rcExit;
}
