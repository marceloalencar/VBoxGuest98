/** @file
 * VBoxTray - Guest control: programs started from the host
 * (VBoxManage guestcontrol run/start).
 *
 * Follows upstream VBoxServiceControl.cpp and VBoxServiceControlSession.cpp
 * over VBoxGuestControlSvc (protocol 2):
 *  - The root (this service's thread) connects, becomes the master
 *    (GUEST_MSG_MAKE_ME_MASTER), reports its features and waits for host
 *    messages with GUEST_MSG_PEEK_WAIT, which also tells it about VM
 *    restores.  It handles HOST_MSG_SESSION_CREATE and
 *    HOST_MSG_SESSION_CLOSE and skips everything else.
 *  - Upstream spawns a "VBoxService --control-session" process per guest
 *    session; here each session is a thread with its own HGCM client that
 *    accepts the session with the key the root prepared, reports it started
 *    and handles the session's messages (HOST_MSG_EXEC_CMD).
 *  - Windows 98 has no user accounts to log on to: programs run as the
 *    logged on user whatever user name the host gives, and the password is
 *    ignored.
 *  - Programs are started with CreateProcess, with anonymous pipes for
 *    standard input and, when the host wants them, standard output and
 *    error; the host's environment changes are applied to VBoxTray's
 *    environment.  A thread per program reports when it ends, once the
 *    host has read what the program left in its output pipes.  The host
 *    polls the output (HOST_MSG_EXEC_GET_OUTPUT), so the pipes are only
 *    read when it asks, without blocking.
 *  - The file, directory and file system messages are handled by
 *    GstCtrlFile.c (VBOX_GUESTCTRL_GF_0_TOOLBOX_AS_CMDS), so the host
 *    doesn't run upstream's toolbox commands (vbox_ls, vbox_stat, ...).
 */

#include "GstCtrl.h"
#include <process.h>
#include <stdlib.h>
#include <string.h>


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/

/** Size of the affinity buffer of HOST_MSG_EXEC_CMD (upstream: 8 x 64 bits). */
#define VBOXGSTCTRL_AFFINITY_SIZE       64
/** How long the root waits for a closing session thread (ms). */
#define VBOXGSTCTRL_CLOSE_WAIT_MS       10000
/** How long an ended program's output may take to be read by the host (ms). */
#define VBOXGSTCTRL_OUTPUT_WAIT_MS      5000
/** How often to check meanwhile (ms). */
#define VBOXGSTCTRL_OUTPUT_POLL_MS      50
/** Failed waits in a row before the root gives up. */
#define VBOXGSTCTRL_MAX_ERRORS          10
/** The guest features reported to the host. */
#define VBOXGSTCTRL_GUEST_FEATURES      (  VBOX_GUESTCTRL_GF_0_SET_SIZE | VBOX_GUESTCTRL_GF_0_PROCESS_ARGV0 \
                                         | VBOX_GUESTCTRL_GF_0_SHUTDOWN | VBOX_GUESTCTRL_GF_0_TOOLBOX_AS_CMDS \
                                         | VBOX_GUESTCTRL_GF_0_PROCESS_CWD | VBOX_GUESTCTRL_GF_0_MOUNT_POINTS_ENUM)


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/

/** A program started in a session. */
typedef struct VBOXGSTCTRLPROC
{
    struct VBOXGSTCTRLPROC *pNext;
    /** References: the session's list, the program's thread and the session
     *  thread while it handles a message for the program. */
    LONG        cRefs;
    VBOXGSTCTRLSESSION *pSession;
    HANDLE      hProcess;
    UINT32      uPid;
    /** The context ID of its HOST_MSG_EXEC_CMD, for the status messages. */
    UINT32      idContext;
    /** Time limit (ms), 0 for none. */
    UINT32      cMsTimeLimit;
    /** Our ends of the pipes, NULL when not used or closed; only the session
     *  thread uses them. */
    HANDLE      hStdInW;
    HANDLE      hStdOutR;
    HANDLE      hStdErrR;
    /** Set when the host ended the program (HOST_MSG_EXEC_TERMINATE). */
    volatile BOOL fTerminated;
    /** Set when the program has ended (its output pipes get no more data). */
    volatile BOOL fEnded;
    /** The executable, for the log. */
    char        szExe[MAX_PATH];
} VBOXGSTCTRLPROC;


/*********************************************************************************************************************************
*   Global Variables                                                                                                             *
*********************************************************************************************************************************/

static CRITICAL_SECTION g_GstCtrlCritSect;
/** The root's handle and client (0 when not connected), for VBoxGstCtrlStop. */
static HANDLE   g_hGstCtrlDevice = INVALID_HANDLE_VALUE;
static UINT32   g_idGstCtrlClient;
/** The open sessions. */
static VBOXGSTCTRLSESSION *g_pGstCtrlSessions;
/** The host features (VBOX_GUESTCTRL_HF_0_XXX). */
UINT64          g_fVBoxGstCtrlHostFeatures;


/*********************************************************************************************************************************
*   HGCM calls                                                                                                                   *
*********************************************************************************************************************************/

/**
 * Makes a call that has only 32-bit parameters, repeating it when interrupted.
 *
 * @returns VBox status code.
 * @param   cParms      Number of values in pau32.
 */
int VBoxGstCtrlCallU32(HANDLE hDevice, UINT32 idClient, UINT32 uFunction, UINT32 cParms, const UINT32 *pau32)
{
    VBGLR3HGCMMSG Msg;
    UINT32 i;
    int rc;

    do
    {
        if (!VbglR3HgcmMsgInit(&Msg, idClient, uFunction, cParms, VBOXGSTCTRL_CALL_TIMEOUT_MS, 0))
            return VERR_NO_MEMORY;
        for (i = 0; i < cParms; i++)
            VbglR3HgcmMsgSetU32(&Msg, i, pau32[i]);
        rc = VbglR3HgcmMsgCall(hDevice, &Msg);
        VbglR3HgcmMsgFree(&Msg);
    } while (rc == VERR_INTERRUPTED);
    return rc;
}

/**
 * Cancels the pending GUEST_MSG_PEEK_WAIT of a client, then disconnects it.
 * Disconnecting alone doesn't wake the waiting thread up with this service.
 *
 * @param   idClient    The client, 0 if already disconnected.
 */
static void vboxGstCtrlCancelAndDisconnect(HANDLE hDevice, UINT32 idClient)
{
    if (!idClient)
        return;
    VBoxGstCtrlCallU32(hDevice, idClient, GUEST_MSG_CANCEL, 0, NULL);
    VbglR3HgcmDisconnect(hDevice, idClient);
}

/**
 * Reports our features and gets the host's (GUEST_MSG_REPORT_FEATURES).
 */
static int vboxGstCtrlReportFeatures(HANDLE hDevice, UINT32 idClient, UINT64 *pfHostFeatures)
{
    VBGLR3HGCMMSG Msg;
    int rc;

    if (!VbglR3HgcmMsgInit(&Msg, idClient, GUEST_MSG_REPORT_FEATURES, 2, VBOXGSTCTRL_CALL_TIMEOUT_MS, 0))
        return VERR_NO_MEMORY;
    VbglR3HgcmMsgSetU64(&Msg, 0, VBOXGSTCTRL_GUEST_FEATURES);
    VbglR3HgcmMsgSetU64(&Msg, 1, VBOX_GUESTCTRL_GF_1_MUST_BE_ONE);
    rc = VbglR3HgcmMsgCall(hDevice, &Msg);
    if (RT_SUCCESS(rc))
    {
        if (VbglR3HgcmMsgGetU64(&Msg, 1) & VBOX_GUESTCTRL_GF_1_MUST_BE_ONE)
            rc = VERR_NOT_SUPPORTED; /* The host didn't understand the request. */
        else
            *pfHostFeatures = VbglR3HgcmMsgGetU64(&Msg, 0);
    }
    VbglR3HgcmMsgFree(&Msg);
    return rc;
}

/**
 * Waits for the next host message (GUEST_MSG_PEEK_WAIT).
 *
 * @returns VBox status code; VERR_VM_RESTORED when *pidRestore no longer
 *          matches the VM session, which is then updated.
 * @param   pidRestore  The VM session ID to check against, NULL for no check.
 * @param   pidMsg      Where to return the message.
 * @param   pcParms     Where to return its parameter count.
 */
static int vboxGstCtrlPeekWait(HANDLE hDevice, UINT32 idClient, UINT64 *pidRestore, UINT32 *pidMsg, UINT32 *pcParms)
{
    VBGLR3HGCMMSG Msg;
    int rc;

    if (!VbglR3HgcmMsgInit(&Msg, idClient, GUEST_MSG_PEEK_WAIT, 2, VBOXGUEST98_WAIT_INFINITE, 0))
        return VERR_NO_MEMORY;
    VbglR3HgcmMsgSetU64(&Msg, 0, pidRestore ? *pidRestore : 0);
    VbglR3HgcmMsgSetU32(&Msg, 1, 0);
    rc = VbglR3HgcmMsgCall(hDevice, &Msg);
    if (RT_SUCCESS(rc))
    {
        *pidMsg  = (UINT32)VbglR3HgcmMsgGetU64(&Msg, 0);
        *pcParms = VbglR3HgcmMsgGetU32(&Msg, 1);
    }
    else if (rc == VERR_VM_RESTORED && pidRestore)
        *pidRestore = VbglR3HgcmMsgGetU64(&Msg, 0);
    VbglR3HgcmMsgFree(&Msg);
    return rc;
}

/**
 * Drops the host message idMsg, answering it with rcSkip (GUEST_MSG_SKIP).
 */
void VBoxGstCtrlSkip(HANDLE hDevice, UINT32 idClient, int rcSkip, UINT32 idMsg)
{
    UINT32 au32[2];
    au32[0] = (UINT32)rcSkip;
    au32[1] = idMsg;
    VBoxGstCtrlCallU32(hDevice, idClient, GUEST_MSG_SKIP, 2, au32);
}

/**
 * Prepares or accepts a session (GUEST_MSG_SESSION_PREPARE / _ACCEPT).
 */
static int vboxGstCtrlSessionKey(HANDLE hDevice, UINT32 idClient, UINT32 uFunction, UINT32 idSession, const BYTE *pbKey)
{
    VBGLR3HGCMMSG Msg;
    int rc;

    do
    {
        if (!VbglR3HgcmMsgInit(&Msg, idClient, uFunction, 2, VBOXGSTCTRL_CALL_TIMEOUT_MS, VBOXGSTCTRL_KEY_SIZE))
            return VERR_NO_MEMORY;
        VbglR3HgcmMsgSetU32(&Msg, 0, idSession);
        VbglR3HgcmMsgSetBuf(&Msg, 1, VBOXGUEST98_HGCM_PARM_BUFFER_IN, VBOXGSTCTRL_KEY_SIZE, pbKey);
        rc = VbglR3HgcmMsgCall(hDevice, &Msg);
        VbglR3HgcmMsgFree(&Msg);
    } while (rc == VERR_INTERRUPTED);
    return rc;
}

/**
 * Reports a session status (GUEST_MSG_SESSION_NOTIFY).
 */
static int vboxGstCtrlSessionNotify(HANDLE hDevice, UINT32 idClient, UINT32 idContext, UINT32 uType, int iResult)
{
    UINT32 au32[3];
    au32[0] = idContext;
    au32[1] = uType;
    au32[2] = (UINT32)iResult;
    return VBoxGstCtrlCallU32(hDevice, idClient, GUEST_MSG_SESSION_NOTIFY, 3, au32);
}

/**
 * Reports a program status (GUEST_MSG_EXEC_STATUS), without data.
 */
static int vboxGstCtrlExecStatus(HANDLE hDevice, UINT32 idClient, UINT32 idContext, UINT32 uPid, UINT32 uStatus,
                                 UINT32 fFlags)
{
    VBGLR3HGCMMSG Msg;
    int rc;

    do
    {
        if (!VbglR3HgcmMsgInit(&Msg, idClient, GUEST_MSG_EXEC_STATUS, 5, VBOXGSTCTRL_CALL_TIMEOUT_MS, 0))
            return VERR_NO_MEMORY;
        VbglR3HgcmMsgSetU32(&Msg, 0, idContext);
        VbglR3HgcmMsgSetU32(&Msg, 1, uPid);
        VbglR3HgcmMsgSetU32(&Msg, 2, uStatus);
        VbglR3HgcmMsgSetU32(&Msg, 3, fFlags);
        VbglR3HgcmMsgSetBuf(&Msg, 4, VBOXGUEST98_HGCM_PARM_BUFFER_IN, 0, NULL);
        rc = VbglR3HgcmMsgCall(hDevice, &Msg);
        VbglR3HgcmMsgFree(&Msg);
    } while (rc == VERR_INTERRUPTED);
    return rc;
}

/**
 * Makes a call built with VbglR3HgcmMsgInit, repeating it when interrupted.
 */
int VBoxGstCtrlCallMsg(HANDLE hDevice, VBGLR3HGCMMSG *pMsg)
{
    int rc;
    do
        rc = VbglR3HgcmMsgCall(hDevice, pMsg);
    while (rc == VERR_INTERRUPTED);
    return rc;
}

/**
 * Opens NUL for a standard handle the host doesn't want.
 *
 * @returns Inheritable handle, NULL on failure.
 */
static HANDLE vboxGstCtrlOpenNul(void)
{
    SECURITY_ATTRIBUTES Sa;
    HANDLE h;

    Sa.nLength              = sizeof(Sa);
    Sa.lpSecurityDescriptor = NULL;
    Sa.bInheritHandle       = TRUE;
    h = CreateFile("NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &Sa, OPEN_EXISTING, 0, NULL);
    return h != INVALID_HANDLE_VALUE ? h : NULL;
}

/**
 * Fetches HOST_MSG_SESSION_CLOSE.
 *
 * @param   pidContext  Where to return its context ID.
 */
static int vboxGstCtrlGetSessionClose(HANDLE hDevice, UINT32 idClient, UINT32 *pidContext)
{
    VBGLR3HGCMMSG Msg;
    int rc;

    do
    {
        if (!VbglR3HgcmMsgInit(&Msg, idClient, GUEST_MSG_GET, 2, VBOXGSTCTRL_CALL_TIMEOUT_MS, 0))
            return VERR_NO_MEMORY;
        VbglR3HgcmMsgSetU32(&Msg, 0, HOST_MSG_SESSION_CLOSE);
        VbglR3HgcmMsgSetU32(&Msg, 1, 0);
        rc = VbglR3HgcmMsgCall(hDevice, &Msg);
        if (RT_SUCCESS(rc))
            *pidContext = VbglR3HgcmMsgGetU32(&Msg, 0);
        VbglR3HgcmMsgFree(&Msg);
    } while (rc == VERR_INTERRUPTED);
    return rc;
}

/**
 * Copies a string buffer parameter out of a fetched message.
 *
 * @returns The ANSI string (malloc'ed), NULL if out of memory.
 */
char *VBoxGstCtrlGetStr(VBGLR3HGCMMSG *pMsg, UINT32 iParm, const BYTE *pb, char chDefault)
{
    DWORD cb = VbglR3HgcmMsgGetBufSize(pMsg, iParm);
    DWORD cch = 0;

    while (cch < cb && pb[cch])
        cch++;
    return VBoxTrayUtf8ToAnsi((const char *)pb, cch, chDefault);
}


/*********************************************************************************************************************************
*   Sessions                                                                                                                     *
*********************************************************************************************************************************/

static void vboxGstCtrlSessionRelease(VBOXGSTCTRLSESSION *pSession)
{
    if (InterlockedDecrement(&pSession->cRefs) != 0)
        return;
    if (pSession->hThread)
        CloseHandle(pSession->hThread);
    if (pSession->hEvtStop)
        CloseHandle(pSession->hEvtStop);
    if (pSession->hDevice != INVALID_HANDLE_VALUE)
        VbglR3Close(pSession->hDevice);
    free(pSession);
}

/**
 * Disconnects a session's client, which wakes its thread up.
 */
static void vboxGstCtrlSessionDisconnect(VBOXGSTCTRLSESSION *pSession)
{
    UINT32 idClient;

    SetEvent(pSession->hEvtStop);
    EnterCriticalSection(&g_GstCtrlCritSect);
    idClient = pSession->idClient;
    pSession->idClient = 0;
    LeaveCriticalSection(&g_GstCtrlCritSect);
    vboxGstCtrlCancelAndDisconnect(pSession->hDevice, idClient);
}

/**
 * Builds the command line: the executable, then the host's arguments (Bourne
 * shell quoted) quoted the way the C runtime splits them again.
 *
 * @returns The command line (malloc'ed), NULL if out of memory.
 * @param   fSkipArgv0  The arguments start with argv[0], which the
 *                      executable replaces.
 * @param   fUnquoted   Join the arguments with spaces as they are, for
 *                      programs that don't split like the C runtime
 *                      (GUEST_PROC_CREATE_FLAG_UNQUOTED_ARGS, as upstream's
 *                      RTPROC_FLAGS_UNQUOTED_ARGS).
 */
static char *vboxGstCtrlBuildCmdLine(const char *pszExe, const char *pszArgs, BOOL fSkipArgv0, BOOL fUnquoted)
{
    size_t cchArgs = strlen(pszArgs);
    char *pszRet = (char *)malloc(strlen(pszExe) * 2 + 4 + cchArgs * 3 + 16);
    char *pszArg = (char *)malloc(cchArgs + 1);
    char *pszOut;
    const char *pszIn = pszArgs;
    const char *pch;
    BOOL fFirst = TRUE;

    if (!pszRet || !pszArg)
    {
        free(pszRet);
        free(pszArg);
        return NULL;
    }
    pszOut = pszRet;
    *pszOut++ = '"';
    for (pch = pszExe; *pch; pch++)
        *pszOut++ = *pch;
    *pszOut++ = '"';

    for (;;)
    {
        char *pszEnd = pszArg;
        char chQuote = 0;
        BOOL fNeedQuotes = FALSE;
        size_t cBackslashes;

        while (*pszIn == ' ' || *pszIn == '\t' || *pszIn == '\n')
            pszIn++;
        if (!*pszIn)
            break;

        /* One Bourne shell word. */
        while (*pszIn && (chQuote || (*pszIn != ' ' && *pszIn != '\t' && *pszIn != '\n')))
        {
            char ch = *pszIn++;
            if (chQuote == '\'')
            {
                if (ch == '\'')
                    chQuote = 0;
                else
                    *pszEnd++ = ch;
            }
            else if (chQuote == '"')
            {
                if (ch == '"')
                    chQuote = 0;
                else if (ch == '\\' && *pszIn && strchr("$`\"\\\n", *pszIn))
                    *pszEnd++ = *pszIn++;
                else
                    *pszEnd++ = ch;
            }
            else if (ch == '\'' || ch == '"')
                chQuote = ch;
            else if (ch == '\\' && *pszIn)
                *pszEnd++ = *pszIn++;
            else
                *pszEnd++ = ch;
        }
        *pszEnd = '\0';

        if (fFirst && fSkipArgv0)
        {
            fFirst = FALSE;
            continue;
        }
        fFirst = FALSE;

        /* Appended quoted when needed, like the C runtime expects. */
        *pszOut++ = ' ';
        fNeedQuotes = !fUnquoted && (!pszArg[0] || strpbrk(pszArg, " \t\"") != NULL);
        if (!fNeedQuotes)
        {
            for (pch = pszArg; *pch; pch++)
                *pszOut++ = *pch;
            continue;
        }
        *pszOut++ = '"';
        cBackslashes = 0;
        for (pch = pszArg; *pch; pch++)
        {
            if (*pch == '\\')
            {
                cBackslashes++;
                *pszOut++ = '\\';
                continue;
            }
            if (*pch == '"')
            {
                /* Double the backslashes before a quote and escape it. */
                while (cBackslashes--)
                    *pszOut++ = '\\';
                *pszOut++ = '\\';
            }
            cBackslashes = 0;
            *pszOut++ = *pch;
        }
        while (cBackslashes--)
            *pszOut++ = '\\';
        *pszOut++ = '"';
    }

    *pszOut = '\0';
    free(pszArg);
    return pszRet;
}

/**
 * Turns a Win32 error from CreateProcess into a VBox status code.
 */
static int vboxGstCtrlWin32ToRc(DWORD dwErr)
{
    switch (dwErr)
    {
        case ERROR_FILE_NOT_FOUND:      return VERR_FILE_NOT_FOUND;
        case ERROR_PATH_NOT_FOUND:      return VERR_PATH_NOT_FOUND;
        case ERROR_ACCESS_DENIED:       return VERR_ACCESS_DENIED;
        case ERROR_INVALID_NAME:        return VERR_INVALID_NAME;
        case ERROR_NOT_ENOUGH_MEMORY:   return VERR_NO_MEMORY;
        default:                        return VERR_OPEN_FAILED;
    }
}

/**
 * Releases a program; the last reference closes its handles.
 */
static void vboxGstCtrlProcRelease(VBOXGSTCTRLPROC *pProc)
{
    if (InterlockedDecrement(&pProc->cRefs) != 0)
        return;
    if (pProc->hStdInW)
        CloseHandle(pProc->hStdInW);
    if (pProc->hStdOutR)
        CloseHandle(pProc->hStdOutR);
    if (pProc->hStdErrR)
        CloseHandle(pProc->hStdErrR);
    CloseHandle(pProc->hProcess);
    free(pProc);
}

/**
 * Finds a program of the session by its PID.
 *
 * @returns The program with a reference added, NULL if not found.
 */
static VBOXGSTCTRLPROC *vboxGstCtrlProcRetain(VBOXGSTCTRLSESSION *pSession, UINT32 uPid)
{
    VBOXGSTCTRLPROC *pProc;

    EnterCriticalSection(&g_GstCtrlCritSect);
    for (pProc = pSession->pProcs; pProc; pProc = pProc->pNext)
        if (pProc->uPid == uPid)
        {
            InterlockedIncrement(&pProc->cRefs);
            break;
        }
    LeaveCriticalSection(&g_GstCtrlCritSect);
    return pProc;
}

/**
 * Waits until the host has read all a program left in its output pipes.
 *
 * The host stops asking for output once it is told the program ended, and a
 * program's last output may still be in the pipes then (with COMMAND.COM the
 * process is REDIR32.EXE, which can end before the pipes are empty), so the
 * end is only reported once the session thread has met the end of the pipes
 * (it closes them then).  Bounded, as a program the ended one started may
 * still hold the pipes.
 */
static void vboxGstCtrlProcWaitForOutput(VBOXGSTCTRLPROC *pProc, HANDLE hEvtStop)
{
    DWORD msStart = GetTickCount();

    pProc->fEnded = TRUE;
    while (   (*(HANDLE volatile *)&pProc->hStdOutR || *(HANDLE volatile *)&pProc->hStdErrR)
           && GetTickCount() - msStart < VBOXGSTCTRL_OUTPUT_WAIT_MS)
        if (WaitForSingleObject(hEvtStop, VBOXGSTCTRL_OUTPUT_POLL_MS) != WAIT_TIMEOUT)
            break;
}

/**
 * Waits for a program to end and reports it, or ends it at its time limit.
 */
static unsigned __stdcall vboxGstCtrlProcThread(void *pvUser)
{
    VBOXGSTCTRLPROC *pProc = (VBOXGSTCTRLPROC *)pvUser;
    VBOXGSTCTRLSESSION *pSession = pProc->pSession;
    HANDLE ahWait[2];
    DWORD dwWait;
    DWORD dwExit = 0;
    UINT32 uStatus;
    UINT32 idClient;

    ahWait[0] = pProc->hProcess;
    ahWait[1] = pSession->hEvtStop;
    dwWait = WaitForMultipleObjects(2, ahWait, FALSE, pProc->cMsTimeLimit ? pProc->cMsTimeLimit : INFINITE);
    if (dwWait == WAIT_OBJECT_0)
    {
        GetExitCodeProcess(pProc->hProcess, &dwExit);
        vboxGstCtrlProcWaitForOutput(pProc, pSession->hEvtStop);
        if (pProc->fTerminated)
        {
            /* Like upstream, a program the host ended is reported down. */
            uStatus = PROC_STS_DWN;
            VBoxTrayLogRel("GstCtrl: \"%s\" was ended by the host\n", pProc->szExe);
        }
        else
        {
            uStatus = PROC_STS_TEN;
            VBoxTrayLogRel("GstCtrl: \"%s\" ended with %lu\n", pProc->szExe, dwExit);
        }
    }
    else if (dwWait == WAIT_TIMEOUT)
    {
        uStatus = TerminateProcess(pProc->hProcess, 1) ? PROC_STS_TOK : PROC_STS_TOA;
        vboxGstCtrlProcWaitForOutput(pProc, pSession->hEvtStop);
        VBoxTrayLogRel("GstCtrl: \"%s\" reached its time limit of %lu ms, %s\n", pProc->szExe, pProc->cMsTimeLimit,
                       uStatus == PROC_STS_TOK ? "ended" : "couldn't be ended");
    }
    else
    {
        /* The session is closing: like upstream (VGSvcGstCtrlSessionClose), its
           programs end with it.  A program started with "start" keeps running,
           as the host leaves that session open. */
        TerminateProcess(pProc->hProcess, 1);
        VBoxTrayLogRel("GstCtrl: \"%s\" ended with its session\n", pProc->szExe);
        uStatus = 0; /* The session's client is gone, nothing to report to. */
    }

    if (uStatus)
    {
        EnterCriticalSection(&g_GstCtrlCritSect);
        idClient = pSession->idClient;
        LeaveCriticalSection(&g_GstCtrlCritSect);
        if (idClient)
            vboxGstCtrlExecStatus(pSession->hDevice, idClient, pProc->idContext, pProc->uPid, uStatus,
                                  uStatus == PROC_STS_TEN ? dwExit : 0);
    }

    /* The program stays in the session's list: the host may still read what
       it wrote. */
    vboxGstCtrlProcRelease(pProc);
    vboxGstCtrlSessionRelease(pSession);
    return 0;
}

/**
 * Creates a pipe for a child's standard handle.
 *
 * @returns TRUE on success.
 * @param   phOurs      Where to return our end (not inherited).
 * @param   phChild     Where to return the child's end (inherited).
 * @param   fChildReads Whether the child reads from it (standard input).
 */
static BOOL vboxGstCtrlPipe(HANDLE *phOurs, HANDLE *phChild, BOOL fChildReads)
{
    SECURITY_ATTRIBUTES Sa;
    HANDLE hRead;
    HANDLE hWrite;
    HANDLE hOurs;

    Sa.nLength              = sizeof(Sa);
    Sa.lpSecurityDescriptor = NULL;
    Sa.bInheritHandle       = TRUE;
    if (!CreatePipe(&hRead, &hWrite, &Sa, 0))
        return FALSE;
    if (!DuplicateHandle(GetCurrentProcess(), fChildReads ? hWrite : hRead, GetCurrentProcess(), &hOurs, 0, FALSE,
                         DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE))
    {
        CloseHandle(hRead);
        CloseHandle(hWrite);
        return FALSE;
    }
    *phOurs  = hOurs;
    *phChild = fChildReads ? hRead : hWrite;
    return TRUE;
}

/**
 * Applies the host's environment changes to ours, for the new program.
 *
 * @returns The ANSI environment block (malloc'ed), NULL to use ours as is.
 * @param   pbEnv       The changes: zero terminated UTF-8 strings, "NAME=VALUE"
 *                      to set a variable, "NAME" to remove it.
 * @param   cbEnv       Size of the changes.
 */
static char *vboxGstCtrlBuildEnv(const BYTE *pbEnv, DWORD cbEnv)
{
    char *pszOurs = GetEnvironmentStrings();
    char **papszVars = NULL;
    char *pszRet = NULL;
    const char *psz;
    unsigned cVars = 0;
    unsigned cMax = 0;
    unsigned i;
    DWORD off;
    size_t cb = 1;

    /* Ours first ("=C:" style entries included, as they are). */
    for (psz = pszOurs; psz && *psz; psz += strlen(psz) + 1)
        cMax++;
    for (off = 0; off < cbEnv; off++)
        if (!pbEnv[off])
            cMax++;
    papszVars = (char **)calloc(cMax + 1, sizeof(char *));
    if (!papszVars)
        goto done;
    for (psz = pszOurs; psz && *psz; psz += strlen(psz) + 1)
        papszVars[cVars++] = _strdup(psz);

    /* Then the changes. */
    for (off = 0; off < cbEnv; )
    {
        DWORD cch = 0;
        char *pszChange;
        char *pszEq;
        size_t cchName;

        while (off + cch < cbEnv && pbEnv[off + cch])
            cch++;
        pszChange = cch ? VBoxTrayUtf8ToAnsi((const char *)&pbEnv[off], cch, '?') : NULL;
        off += cch + 1;
        if (!pszChange)
            continue;
        pszEq = strchr(pszChange, '=');
        cchName = pszEq ? (size_t)(pszEq - pszChange) : strlen(pszChange);
        for (i = 0; i < cVars; i++)
            if (   papszVars[i]
                && _strnicmp(papszVars[i], pszChange, cchName) == 0
                && papszVars[i][cchName] == '=')
            {
                free(papszVars[i]);
                papszVars[i] = NULL;
            }
        if (pszEq && cchName)
            papszVars[cVars++] = pszChange;
        else
            free(pszChange);
    }

    for (i = 0; i < cVars; i++)
        if (papszVars[i])
            cb += strlen(papszVars[i]) + 1;
    pszRet = (char *)malloc(cb + 1);
    if (pszRet)
    {
        char *pszOut = pszRet;
        for (i = 0; i < cVars; i++)
            if (papszVars[i])
            {
                strcpy(pszOut, papszVars[i]);
                pszOut += strlen(pszOut) + 1;
            }
        *pszOut++ = '\0';
        *pszOut = '\0'; /* An empty block still needs two terminators. */
    }

done:
    if (papszVars)
    {
        for (i = 0; i < cVars; i++)
            free(papszVars[i]);
        free(papszVars);
    }
    if (pszOurs)
        FreeEnvironmentStrings(pszOurs);
    return pszRet;
}

/**
 * Handles HOST_MSG_EXEC_CMD: starts the program and reports whether that
 * worked.
 */
static void vboxGstCtrlSessionExec(VBOXGSTCTRLSESSION *pSession, UINT32 idClient, UINT32 cParms)
{
    VBGLR3HGCMMSG Msg;
    DWORD cbBuf = VBOXGSTCTRL_DEF_BUF_SIZE;
    BYTE *pbCmd;
    BYTE *pbArgs;
    BYTE *pbEnv;
    BYTE *pbCwd = NULL;
    char *pszExe = NULL;
    char *pszArgs = NULL;
    char *pszCwd = NULL;
    char *pszEnv = NULL;
    char *pszCmdLine = NULL;
    UINT32 idContext = 0;
    UINT32 fFlags = 0;
    UINT32 cMsTimeLimit = 0;
    UINT32 i;
    STARTUPINFOA StartupInfo;
    PROCESS_INFORMATION ProcInfo;
    HANDLE hStdInW = NULL;
    HANDLE hStdOutR = NULL;
    HANDLE hStdErrR = NULL;
    HANDLE ahChild[3];
    VBOXGSTCTRLPROC *pProc;
    unsigned idThread;
    HANDLE hThread;
    int rc;

    if (cParms < HOST_MSG_EXEC_CMD_CPARMS)
    {
        VBoxGstCtrlSkip(pSession->hDevice, idClient, VERR_INVALID_PARAMETER, HOST_MSG_EXEC_CMD);
        return;
    }

    /* Fetch it, growing the buffers until everything fits. */
    for (;;)
    {
        if (!VbglR3HgcmMsgInit(&Msg, idClient, GUEST_MSG_GET, cParms, VBOXGSTCTRL_CALL_TIMEOUT_MS,
                               cbBuf * 4 + VBOXGSTCTRL_AFFINITY_SIZE))
        {
            VBoxGstCtrlSkip(pSession->hDevice, idClient, VERR_NO_MEMORY, HOST_MSG_EXEC_CMD);
            return;
        }
        VbglR3HgcmMsgSetU32(&Msg, 0, HOST_MSG_EXEC_CMD);
        pbCmd = VbglR3HgcmMsgSetBuf(&Msg, 1, VBOXGUEST98_HGCM_PARM_BUFFER_OUT, cbBuf, NULL);
        VbglR3HgcmMsgSetU32(&Msg, 2, 0);
        VbglR3HgcmMsgSetU32(&Msg, 3, 0);
        pbArgs = VbglR3HgcmMsgSetBuf(&Msg, 4, VBOXGUEST98_HGCM_PARM_BUFFER_OUT, cbBuf, NULL);
        VbglR3HgcmMsgSetU32(&Msg, 5, 0);
        VbglR3HgcmMsgSetU32(&Msg, 6, 0);
        pbEnv = VbglR3HgcmMsgSetBuf(&Msg, 7, VBOXGUEST98_HGCM_PARM_BUFFER_OUT, cbBuf, NULL);
        VbglR3HgcmMsgSetU32(&Msg, 8, 0);
        VbglR3HgcmMsgSetU32(&Msg, 9, 0);
        VbglR3HgcmMsgSetU32(&Msg, 10, 0);
        VbglR3HgcmMsgSetBuf(&Msg, 11, VBOXGUEST98_HGCM_PARM_BUFFER_OUT, VBOXGSTCTRL_AFFINITY_SIZE, NULL);
        if (cParms > HOST_MSG_EXEC_CMD_CPARMS)
            pbCwd = VbglR3HgcmMsgSetBuf(&Msg, 12, VBOXGUEST98_HGCM_PARM_BUFFER_OUT, cbBuf, NULL);
        for (i = HOST_MSG_EXEC_CMD_CPARMS + 1; i < cParms; i++)
            VbglR3HgcmMsgSetU32(&Msg, i, 0); /* Newer parameters we don't know about. */
        rc = VbglR3HgcmMsgCall(pSession->hDevice, &Msg);
        if (rc != VERR_BUFFER_OVERFLOW || cbBuf >= VBOXGSTCTRL_MAX_BUF_SIZE)
            break;
        VbglR3HgcmMsgFree(&Msg);
        cbBuf *= 2;
    }
    if (RT_FAILURE(rc))
    {
        VBoxTrayLogRel("GstCtrl: fetching a program to start failed, rc=%d\n", rc);
        VbglR3HgcmMsgFree(&Msg);
        VBoxGstCtrlSkip(pSession->hDevice, idClient, rc, HOST_MSG_EXEC_CMD);
        return;
    }

    idContext    = VbglR3HgcmMsgGetU32(&Msg, 0);
    fFlags       = VbglR3HgcmMsgGetU32(&Msg, 2);
    cMsTimeLimit = VbglR3HgcmMsgGetU32(&Msg, 8);
    pszExe  = VBoxGstCtrlGetStr(&Msg, 1, pbCmd, '?');
    pszArgs = VbglR3HgcmMsgGetU32(&Msg, 3) ? VBoxGstCtrlGetStr(&Msg, 4, pbArgs, '?') : VBoxTrayUtf8ToAnsi("", 0, '?');
    if (pbCwd)
        pszCwd = VBoxGstCtrlGetStr(&Msg, 12, pbCwd, '?');
    if (VbglR3HgcmMsgGetU32(&Msg, 5))
        pszEnv = vboxGstCtrlBuildEnv(pbEnv, VbglR3HgcmMsgGetBufSize(&Msg, 7));
    VbglR3HgcmMsgFree(&Msg);

    rc = VERR_NO_MEMORY;
    if (pszExe && pszArgs)
        pszCmdLine = vboxGstCtrlBuildCmdLine(pszExe, pszArgs,
                                             (g_fVBoxGstCtrlHostFeatures & VBOX_GUESTCTRL_HF_0_PROCESS_ARGV0) != 0,
                                             (fFlags & GUEST_PROC_CREATE_FLAG_UNQUOTED_ARGS) != 0);
    if (pszCmdLine)
    {
        /* Standard input always gets a pipe, like upstream; the output only
           when the host wants it, otherwise NUL. */
        ahChild[0] = ahChild[1] = ahChild[2] = NULL;
        memset(&StartupInfo, 0, sizeof(StartupInfo));
        StartupInfo.cb          = sizeof(StartupInfo);
        StartupInfo.dwFlags     = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
        StartupInfo.wShowWindow = (fFlags & GUEST_PROC_CREATE_FLAG_HIDDEN) ? SW_HIDE : SW_SHOWNORMAL;

        /* Serialized: a program started meanwhile would inherit these handles. */
        EnterCriticalSection(&g_GstCtrlCritSect);
        if (   vboxGstCtrlPipe(&hStdInW, &ahChild[0], TRUE)
            && (  (fFlags & GUEST_PROC_CREATE_FLAG_WAIT_STDOUT)
                ? vboxGstCtrlPipe(&hStdOutR, &ahChild[1], FALSE)
                : (ahChild[1] = vboxGstCtrlOpenNul()) != NULL)
            && (  (fFlags & GUEST_PROC_CREATE_FLAG_WAIT_STDERR)
                ? vboxGstCtrlPipe(&hStdErrR, &ahChild[2], FALSE)
                : (ahChild[2] = vboxGstCtrlOpenNul()) != NULL))
        {
            StartupInfo.hStdInput  = ahChild[0];
            StartupInfo.hStdOutput = ahChild[1];
            StartupInfo.hStdError  = ahChild[2];
            if (CreateProcessA(pszExe, pszCmdLine, NULL, NULL, TRUE, 0, pszEnv, pszCwd && *pszCwd ? pszCwd : NULL,
                               &StartupInfo, &ProcInfo))
            {
                rc = VINF_SUCCESS;
                CloseHandle(ProcInfo.hThread);
            }
            else
                rc = vboxGstCtrlWin32ToRc(GetLastError());
        }
        for (i = 0; i < 3; i++)
            if (ahChild[i])
                CloseHandle(ahChild[i]); /* Only the child keeps them, so the pipes end with it. */
        LeaveCriticalSection(&g_GstCtrlCritSect);
    }

    if (RT_FAILURE(rc))
    {
        VBoxTrayLogRel("GstCtrl: starting \"%s\" failed, rc=%d\n", pszExe ? pszExe : "", rc);
        vboxGstCtrlExecStatus(pSession->hDevice, idClient, idContext, 0, PROC_STS_ERROR, (UINT32)rc);
        if (hStdInW)
            CloseHandle(hStdInW);
        if (hStdOutR)
            CloseHandle(hStdOutR);
        if (hStdErrR)
            CloseHandle(hStdErrR);
    }
    else
    {
        VBoxTrayLogRel("GstCtrl: started \"%s\" in session %lu\n", pszExe, pSession->idSession);
        hThread = NULL;
        pProc = (VBOXGSTCTRLPROC *)calloc(1, sizeof(*pProc));
        if (pProc)
        {
            pProc->cRefs        = 2; /* The session's list and the thread. */
            pProc->pSession     = pSession;
            pProc->hProcess     = ProcInfo.hProcess;
            pProc->uPid         = ProcInfo.dwProcessId;
            pProc->idContext    = idContext;
            pProc->cMsTimeLimit = cMsTimeLimit;
            pProc->hStdInW      = hStdInW;
            pProc->hStdOutR     = hStdOutR;
            pProc->hStdErrR     = hStdErrR;
            lstrcpyn(pProc->szExe, pszExe, sizeof(pProc->szExe));
            EnterCriticalSection(&g_GstCtrlCritSect);
            pProc->pNext = pSession->pProcs;
            pSession->pProcs = pProc;
            LeaveCriticalSection(&g_GstCtrlCritSect);
        }
        else
        {
            if (hStdInW)
                CloseHandle(hStdInW);
            if (hStdOutR)
                CloseHandle(hStdOutR);
            if (hStdErrR)
                CloseHandle(hStdErrR);
        }

        /* Started before the thread can report its end. */
        vboxGstCtrlExecStatus(pSession->hDevice, idClient, idContext, ProcInfo.dwProcessId, PROC_STS_STARTED, 0);

        if (pProc)
        {
            InterlockedIncrement(&pSession->cRefs);
            hThread = (HANDLE)_beginthreadex(NULL, 0, vboxGstCtrlProcThread, pProc, 0, &idThread);
            if (hThread)
                CloseHandle(hThread);
            else
            {
                InterlockedDecrement(&pSession->cRefs);
                vboxGstCtrlProcRelease(pProc); /* The thread's reference; the list keeps the program. */
            }
        }
        if (!hThread)
            VBoxTrayLogRel("GstCtrl: can't watch \"%s\", its end won't be reported\n", pszExe);
        if (!pProc)
            CloseHandle(ProcInfo.hProcess);
    }

    free(pszExe);
    free(pszArgs);
    free(pszCwd);
    free(pszEnv);
    free(pszCmdLine);
}

/**
 * Handles HOST_MSG_EXEC_GET_OUTPUT: sends what the program wrote so far,
 * nothing if it wrote nothing (the host asks again).
 */
static void vboxGstCtrlSessionOutput(VBOXGSTCTRLSESSION *pSession, UINT32 idClient)
{
    VBGLR3HGCMMSG Msg;
    VBOXGSTCTRLPROC *pProc;
    HANDLE *phPipe;
    BYTE *pbData;
    UINT32 idContext;
    UINT32 uPid;
    UINT32 uHandle;
    UINT32 fFlags;
    DWORD cbAvail = 0;
    DWORD cbRead = 0;
    int rc;

    if (!VbglR3HgcmMsgInit(&Msg, idClient, GUEST_MSG_GET, 4, VBOXGSTCTRL_CALL_TIMEOUT_MS, 0))
    {
        VBoxGstCtrlSkip(pSession->hDevice, idClient, VERR_NO_MEMORY, HOST_MSG_EXEC_GET_OUTPUT);
        return;
    }
    VbglR3HgcmMsgSetU32(&Msg, 0, HOST_MSG_EXEC_GET_OUTPUT);
    VbglR3HgcmMsgSetU32(&Msg, 1, 0);
    VbglR3HgcmMsgSetU32(&Msg, 2, 0);
    VbglR3HgcmMsgSetU32(&Msg, 3, 0);
    rc = VbglR3HgcmMsgCall(pSession->hDevice, &Msg);
    idContext = VbglR3HgcmMsgGetU32(&Msg, 0);
    uPid      = VbglR3HgcmMsgGetU32(&Msg, 1);
    uHandle   = VbglR3HgcmMsgGetU32(&Msg, 2);
    fFlags    = VbglR3HgcmMsgGetU32(&Msg, 3);
    VbglR3HgcmMsgFree(&Msg);
    if (RT_FAILURE(rc))
    {
        VBoxGstCtrlSkip(pSession->hDevice, idClient, rc, HOST_MSG_EXEC_GET_OUTPUT);
        return;
    }

    /* The answer, with room for the data. */
    if (!VbglR3HgcmMsgInit(&Msg, idClient, GUEST_MSG_EXEC_OUTPUT, 5, VBOXGSTCTRL_CALL_TIMEOUT_MS, VBOXGSTCTRL_OUTPUT_SIZE))
        return;
    VbglR3HgcmMsgSetU32(&Msg, 0, idContext);
    VbglR3HgcmMsgSetU32(&Msg, 1, uPid);
    VbglR3HgcmMsgSetU32(&Msg, 2, uHandle);
    VbglR3HgcmMsgSetU32(&Msg, 3, fFlags);
    pbData = VbglR3HgcmMsgSetBuf(&Msg, 4, VBOXGUEST98_HGCM_PARM_BUFFER_IN, VBOXGSTCTRL_OUTPUT_SIZE, NULL);

    /* Only the session thread reads the pipes, no lock needed for that. */
    pProc = vboxGstCtrlProcRetain(pSession, uPid);
    if (pProc)
    {
        phPipe = uHandle == GUEST_PROC_OUT_H_STDERR ? &pProc->hStdErrR : &pProc->hStdOutR;
        if (*phPipe)
        {
            /* Checked before looking at the pipe: whatever the program wrote
               is in it by then. */
            BOOL fEnded = pProc->fEnded;

            /* Never blocks: only what is there is read.  Windows 9x doesn't
               fail PeekNamedPipe when the writer is gone (ERROR_BROKEN_PIPE,
               as NT does), so an empty pipe of an ended program is its end
               too. */
            if (PeekNamedPipe(*phPipe, NULL, 0, NULL, &cbAvail, NULL) && (cbAvail || !fEnded))
            {
                if (cbAvail && !ReadFile(*phPipe, pbData, cbAvail < VBOXGSTCTRL_OUTPUT_SIZE ? cbAvail
                                                                                          : VBOXGSTCTRL_OUTPUT_SIZE,
                                         &cbRead, NULL))
                    cbRead = 0;
            }
            else
            {
                /* Nothing more to come. */
                CloseHandle(*phPipe);
                *phPipe = NULL;
            }
        }
        vboxGstCtrlProcRelease(pProc);
    }

    /* Always answered, empty if need be, or the host waits for it. */
    VBOXGUEST98_HGCM_CALL_PARMS(Msg.pCall)[4].cb = cbRead;
    VBoxGstCtrlCallMsg(pSession->hDevice, &Msg);
    VbglR3HgcmMsgFree(&Msg);
}

/**
 * Handles HOST_MSG_EXEC_SET_INPUT: writes to the program's standard input.
 */
static void vboxGstCtrlSessionInput(VBOXGSTCTRLSESSION *pSession, UINT32 idClient)
{
    VBGLR3HGCMMSG Msg;
    VBOXGSTCTRLPROC *pProc;
    DWORD cbBuf = VBOXGSTCTRL_OUTPUT_SIZE;
    BYTE *pbData;
    UINT32 au32[5];
    UINT32 idContext;
    UINT32 uPid;
    UINT32 fFlags;
    UINT32 cbData;
    UINT32 uStatus;
    UINT32 fStatus = 0;
    DWORD cbWritten = 0;
    int rc;

    for (;;)
    {
        if (!VbglR3HgcmMsgInit(&Msg, idClient, GUEST_MSG_GET, 5, VBOXGSTCTRL_CALL_TIMEOUT_MS, cbBuf))
        {
            VBoxGstCtrlSkip(pSession->hDevice, idClient, VERR_NO_MEMORY, HOST_MSG_EXEC_SET_INPUT);
            return;
        }
        VbglR3HgcmMsgSetU32(&Msg, 0, HOST_MSG_EXEC_SET_INPUT);
        VbglR3HgcmMsgSetU32(&Msg, 1, 0);
        VbglR3HgcmMsgSetU32(&Msg, 2, 0);
        pbData = VbglR3HgcmMsgSetBuf(&Msg, 3, VBOXGUEST98_HGCM_PARM_BUFFER_OUT, cbBuf, NULL);
        VbglR3HgcmMsgSetU32(&Msg, 4, 0);
        rc = VbglR3HgcmMsgCall(pSession->hDevice, &Msg);
        cbData = VbglR3HgcmMsgGetU32(&Msg, 4);
        if (rc != VERR_BUFFER_OVERFLOW || cbData <= cbBuf || cbData > VBOXGSTCTRL_MAX_BUF_SIZE)
            break;
        VbglR3HgcmMsgFree(&Msg);
        cbBuf = cbData;
    }
    if (RT_FAILURE(rc))
    {
        VbglR3HgcmMsgFree(&Msg);
        VBoxGstCtrlSkip(pSession->hDevice, idClient, rc, HOST_MSG_EXEC_SET_INPUT);
        return;
    }
    idContext = VbglR3HgcmMsgGetU32(&Msg, 0);
    uPid      = VbglR3HgcmMsgGetU32(&Msg, 1);
    fFlags    = VbglR3HgcmMsgGetU32(&Msg, 2);
    if (cbData > cbBuf)
        cbData = cbBuf;

    uStatus = INPUT_STS_TERMINATED;
    pProc = vboxGstCtrlProcRetain(pSession, uPid);
    if (pProc)
    {
        if (pProc->hStdInW)
        {
            if (!cbData || WriteFile(pProc->hStdInW, pbData, cbData, &cbWritten, NULL))
                uStatus = INPUT_STS_WRITTEN;
            else
            {
                uStatus = INPUT_STS_ERROR;
                fStatus = (UINT32)VERR_WRITE_ERROR;
            }
            if ((fFlags & GUEST_PROC_IN_FLAG_EOF) && cbWritten == cbData)
            {
                /* The last of it: the program sees the end of its input. */
                CloseHandle(pProc->hStdInW);
                pProc->hStdInW = NULL;
            }
        }
        vboxGstCtrlProcRelease(pProc);
    }
    VbglR3HgcmMsgFree(&Msg);

    au32[0] = idContext;
    au32[1] = uPid;
    au32[2] = uStatus;
    au32[3] = fStatus;
    au32[4] = cbWritten;
    VBoxGstCtrlCallU32(pSession->hDevice, idClient, GUEST_MSG_EXEC_INPUT_STATUS, 5, au32);
}

/**
 * Handles HOST_MSG_EXEC_TERMINATE: ends the program; its thread reports it.
 */
static void vboxGstCtrlSessionTerminate(VBOXGSTCTRLSESSION *pSession, UINT32 idClient)
{
    VBGLR3HGCMMSG Msg;
    VBOXGSTCTRLPROC *pProc;
    UINT32 uPid;
    int rc;

    if (!VbglR3HgcmMsgInit(&Msg, idClient, GUEST_MSG_GET, 2, VBOXGSTCTRL_CALL_TIMEOUT_MS, 0))
    {
        VBoxGstCtrlSkip(pSession->hDevice, idClient, VERR_NO_MEMORY, HOST_MSG_EXEC_TERMINATE);
        return;
    }
    VbglR3HgcmMsgSetU32(&Msg, 0, HOST_MSG_EXEC_TERMINATE);
    VbglR3HgcmMsgSetU32(&Msg, 1, 0);
    rc = VbglR3HgcmMsgCall(pSession->hDevice, &Msg);
    uPid = VbglR3HgcmMsgGetU32(&Msg, 1);
    VbglR3HgcmMsgFree(&Msg);
    if (RT_FAILURE(rc))
    {
        VBoxGstCtrlSkip(pSession->hDevice, idClient, rc, HOST_MSG_EXEC_TERMINATE);
        return;
    }

    pProc = vboxGstCtrlProcRetain(pSession, uPid);
    if (pProc)
    {
        pProc->fTerminated = TRUE;
        TerminateProcess(pProc->hProcess, 1);
        vboxGstCtrlProcRelease(pProc);
    }
}

/**
 * Handles HOST_MSG_EXEC_WAIT_FOR, which upstream fetches and ignores: the
 * end of a program is reported anyway.
 */
static void vboxGstCtrlSessionWaitFor(VBOXGSTCTRLSESSION *pSession, UINT32 idClient)
{
    UINT32 au32[4];

    au32[0] = HOST_MSG_EXEC_WAIT_FOR;
    au32[1] = au32[2] = au32[3] = 0;
    if (RT_FAILURE(VBoxGstCtrlCallU32(pSession->hDevice, idClient, GUEST_MSG_GET, 4, au32)))
        VBoxGstCtrlSkip(pSession->hDevice, idClient, VERR_NOT_IMPLEMENTED, HOST_MSG_EXEC_WAIT_FOR);
}

/**
 * Handles HOST_MSG_SHUTDOWN: answers first, like upstream, so the host
 * doesn't wait for a guest that is going away, then shuts Windows down.
 */
static void vboxGstCtrlSessionShutdown(VBOXGSTCTRLSESSION *pSession, UINT32 idClient)
{
    VBGLR3HGCMMSG Msg;
    UINT32 idContext;
    UINT32 fAction;
    UINT uFlags;
    int rc;

    if (!VbglR3HgcmMsgInit(&Msg, idClient, GUEST_MSG_GET, 2, VBOXGSTCTRL_CALL_TIMEOUT_MS, 0))
    {
        VBoxGstCtrlSkip(pSession->hDevice, idClient, VERR_NO_MEMORY, HOST_MSG_SHUTDOWN);
        return;
    }
    VbglR3HgcmMsgSetU32(&Msg, 0, HOST_MSG_SHUTDOWN);
    VbglR3HgcmMsgSetU32(&Msg, 1, 0);
    rc = VBoxGstCtrlCallMsg(pSession->hDevice, &Msg);
    idContext = VbglR3HgcmMsgGetU32(&Msg, 0);
    fAction   = VbglR3HgcmMsgGetU32(&Msg, 1);
    VbglR3HgcmMsgFree(&Msg);
    if (RT_FAILURE(rc))
    {
        VBoxGstCtrlSkip(pSession->hDevice, idClient, rc, HOST_MSG_SHUTDOWN);
        return;
    }

    if (!VbglR3HgcmMsgInit(&Msg, idClient, GUEST_MSG_REPLY, 4, VBOXGSTCTRL_CALL_TIMEOUT_MS, 0))
        return;
    VbglR3HgcmMsgSetU32(&Msg, 0, idContext);
    VbglR3HgcmMsgSetU32(&Msg, 1, 0);
    VbglR3HgcmMsgSetU32(&Msg, 2, VINF_SUCCESS);
    VbglR3HgcmMsgSetBuf(&Msg, 3, VBOXGUEST98_HGCM_PARM_BUFFER_IN, 0, NULL);
    rc = VBoxGstCtrlCallMsg(pSession->hDevice, &Msg);
    VbglR3HgcmMsgFree(&Msg);
    if (RT_FAILURE(rc))
        return;

    /* Windows 98 needs no shutdown privilege; EWX_POWEROFF ends with the APM
       power off the host sees. */
    uFlags = (fAction & GUEST_SHUTDOWN_FLAG_REBOOT) ? EWX_REBOOT : EWX_SHUTDOWN | EWX_POWEROFF;
    if (fAction & GUEST_SHUTDOWN_FLAG_FORCE)
        uFlags |= EWX_FORCE;
    VBoxTrayLogRel("GstCtrl: the host asks to %s Windows%s\n", (fAction & GUEST_SHUTDOWN_FLAG_REBOOT) ? "restart" : "shut down",
                   (fAction & GUEST_SHUTDOWN_FLAG_FORCE) ? " (forced)" : "");
    if (!ExitWindowsEx(uFlags, 0))
        VBoxTrayLogRel("GstCtrl: ExitWindowsEx failed, error %lu\n", GetLastError());
}

/**
 * Session thread: accepts the session, reports it started and handles its
 * messages until the host closes it.
 */
static unsigned __stdcall vboxGstCtrlSessionThread(void *pvUser)
{
    VBOXGSTCTRLSESSION *pSession = (VBOXGSTCTRLSESSION *)pvUser;
    VBOXGSTCTRLPROC *pProc;
    UINT32 idClient = 0;
    unsigned cErrors = 0;
    int rc;

    rc = VbglR3HgcmConnect(pSession->hDevice, VBOX_GUESTCTRL_SERVICE_NAME, &idClient);
    if (RT_SUCCESS(rc))
    {
        EnterCriticalSection(&g_GstCtrlCritSect);
        pSession->idClient = idClient;
        LeaveCriticalSection(&g_GstCtrlCritSect);
        rc = vboxGstCtrlSessionKey(pSession->hDevice, idClient, GUEST_MSG_SESSION_ACCEPT, pSession->idSession,
                                   pSession->abKey);
    }
    if (RT_SUCCESS(rc))
        rc = vboxGstCtrlSessionNotify(pSession->hDevice, idClient,
                                      VBOX_GUESTCTRL_CONTEXTID_MAKE_SESSION(pSession->idSession),
                                      GUEST_SESSION_NOTIFYTYPE_STARTED, VINF_SUCCESS);
    if (RT_FAILURE(rc))
        VBoxTrayLogRel("GstCtrl: session %lu couldn't start, rc=%d\n", pSession->idSession, rc);

    while (RT_SUCCESS(rc) && !g_fVBoxTrayShutdown)
    {
        UINT32 idMsg = 0;
        UINT32 cParms = 0;
        UINT32 idContext = 0;

        EnterCriticalSection(&g_GstCtrlCritSect);
        idClient = pSession->idClient;
        LeaveCriticalSection(&g_GstCtrlCritSect);
        if (!idClient)
            break; /* Disconnected by the root or VBoxGstCtrlStop. */

        rc = vboxGstCtrlPeekWait(pSession->hDevice, idClient, NULL, &idMsg, &cParms);
        if (RT_FAILURE(rc))
        {
            if (rc == VERR_INTERRUPTED || ++cErrors <= VBOXGSTCTRL_MAX_ERRORS)
            {
                rc = VINF_SUCCESS;
                Sleep(100);
            }
            continue;
        }
        cErrors = 0;

        switch (idMsg)
        {
            case HOST_MSG_SESSION_CLOSE:
                /* The root gets one too and reports the session closed. */
                vboxGstCtrlGetSessionClose(pSession->hDevice, idClient, &idContext);
                rc = VERR_CANCELLED;
                break;

            case HOST_MSG_EXEC_CMD:
                vboxGstCtrlSessionExec(pSession, idClient, cParms);
                break;

            case HOST_MSG_EXEC_GET_OUTPUT:
                vboxGstCtrlSessionOutput(pSession, idClient);
                break;

            case HOST_MSG_EXEC_SET_INPUT:
                vboxGstCtrlSessionInput(pSession, idClient);
                break;

            case HOST_MSG_EXEC_TERMINATE:
                vboxGstCtrlSessionTerminate(pSession, idClient);
                break;

            case HOST_MSG_EXEC_WAIT_FOR:
                vboxGstCtrlSessionWaitFor(pSession, idClient);
                break;

            case HOST_MSG_SHUTDOWN:
                vboxGstCtrlSessionShutdown(pSession, idClient);
                break;

            case HOST_MSG_CANCEL_PENDING_WAITS:
                break;

            default:
                if (!VBoxGstCtrlFileHandleMsg(pSession, idClient, idMsg, cParms))
                {
                    VBoxTrayLogRel("GstCtrl: host message %lu isn't supported yet, skipped\n", idMsg);
                    VBoxGstCtrlSkip(pSession->hDevice, idClient, VERR_NOT_SUPPORTED, idMsg);
                }
                break;
        }
    }

    /* The program threads end the programs and let go of them. */
    vboxGstCtrlSessionDisconnect(pSession);
    VBoxGstCtrlFileCleanup(pSession);
    for (;;)
    {
        EnterCriticalSection(&g_GstCtrlCritSect);
        pProc = pSession->pProcs;
        if (pProc)
            pSession->pProcs = pProc->pNext;
        LeaveCriticalSection(&g_GstCtrlCritSect);
        if (!pProc)
            break;
        vboxGstCtrlProcRelease(pProc);
    }
    vboxGstCtrlSessionRelease(pSession);
    return 0;
}


/*********************************************************************************************************************************
*   Root                                                                                                                         *
*********************************************************************************************************************************/

/**
 * Handles HOST_MSG_SESSION_CREATE: prepares the session and starts its thread.
 */
static void vboxGstCtrlRootSessionCreate(HANDLE hDevice, UINT32 idClient, UINT32 cParms)
{
    VBGLR3HGCMMSG Msg;
    VBOXGSTCTRLSESSION *pSession = NULL;
    char *pszUser = NULL;
    BYTE *pbUser;
    UINT32 idContext;
    UINT32 uProtocol;
    unsigned idThread;
    unsigned i;
    int rc;

    if (cParms != 6)
    {
        VBoxGstCtrlSkip(hDevice, idClient, VERR_INVALID_PARAMETER, HOST_MSG_SESSION_CREATE);
        return;
    }
    if (!VbglR3HgcmMsgInit(&Msg, idClient, GUEST_MSG_GET, 6, VBOXGSTCTRL_CALL_TIMEOUT_MS, 3 * VBOXGSTCTRL_DEF_BUF_SIZE))
    {
        VBoxGstCtrlSkip(hDevice, idClient, VERR_NO_MEMORY, HOST_MSG_SESSION_CREATE);
        return;
    }
    VbglR3HgcmMsgSetU32(&Msg, 0, HOST_MSG_SESSION_CREATE);
    VbglR3HgcmMsgSetU32(&Msg, 1, 0);
    pbUser = VbglR3HgcmMsgSetBuf(&Msg, 2, VBOXGUEST98_HGCM_PARM_BUFFER_OUT, VBOXGSTCTRL_DEF_BUF_SIZE, NULL);
    VbglR3HgcmMsgSetBuf(&Msg, 3, VBOXGUEST98_HGCM_PARM_BUFFER_OUT, VBOXGSTCTRL_DEF_BUF_SIZE, NULL);
    VbglR3HgcmMsgSetBuf(&Msg, 4, VBOXGUEST98_HGCM_PARM_BUFFER_OUT, VBOXGSTCTRL_DEF_BUF_SIZE, NULL);
    VbglR3HgcmMsgSetU32(&Msg, 5, 0);
    rc = VbglR3HgcmMsgCall(hDevice, &Msg);
    if (RT_FAILURE(rc))
    {
        VBoxTrayLogRel("GstCtrl: fetching a new session failed, rc=%d\n", rc);
        VbglR3HgcmMsgFree(&Msg);
        VBoxGstCtrlSkip(hDevice, idClient, rc, HOST_MSG_SESSION_CREATE);
        return;
    }
    idContext = VbglR3HgcmMsgGetU32(&Msg, 0);
    uProtocol = VbglR3HgcmMsgGetU32(&Msg, 1);
    pszUser = VBoxGstCtrlGetStr(&Msg, 2, pbUser, '?');
    VbglR3HgcmMsgFree(&Msg); /* The password goes with it. */

    if (uProtocol != 2)
        rc = VERR_VERSION_MISMATCH; /* Upstream refuses protocol 1 too. */
    else
    {
        rc = VERR_NO_MEMORY;
        pSession = (VBOXGSTCTRLSESSION *)calloc(1, sizeof(*pSession));
        if (pSession)
        {
            pSession->cRefs     = 2; /* The list and the thread. */
            pSession->idSession = VBOX_GUESTCTRL_CONTEXTID_GET_SESSION(idContext);
            pSession->hDevice   = VbglR3OpenEx(TRUE);
            pSession->hEvtStop  = CreateEvent(NULL, TRUE, FALSE, NULL);
            srand(GetTickCount() ^ (idContext << 8));
            for (i = 0; i < VBOXGSTCTRL_KEY_SIZE; i++)
                pSession->abKey[i] = (BYTE)(rand() >> 4);
            if (pSession->hDevice != INVALID_HANDLE_VALUE && pSession->hEvtStop)
                rc = vboxGstCtrlSessionKey(hDevice, idClient, GUEST_MSG_SESSION_PREPARE, pSession->idSession,
                                           pSession->abKey);
            if (RT_SUCCESS(rc))
            {
                pSession->hThread = (HANDLE)_beginthreadex(NULL, 0, vboxGstCtrlSessionThread, pSession, CREATE_SUSPENDED,
                                                           &idThread);
                if (!pSession->hThread)
                    rc = VERR_NO_MEMORY;
            }
        }
    }

    if (RT_SUCCESS(rc))
    {
        VBoxTrayLogRel("GstCtrl: session %lu opened for user \"%s\" (programs run as the logged on user)\n",
                       pSession->idSession, pszUser ? pszUser : "");
        EnterCriticalSection(&g_GstCtrlCritSect);
        pSession->pNext = g_pGstCtrlSessions;
        g_pGstCtrlSessions = pSession;
        LeaveCriticalSection(&g_GstCtrlCritSect);
        ResumeThread(pSession->hThread);
    }
    else
    {
        VBoxTrayLogRel("GstCtrl: opening a session failed, rc=%d\n", rc);
        vboxGstCtrlSessionNotify(hDevice, idClient, idContext, GUEST_SESSION_NOTIFYTYPE_ERROR, rc);
        if (pSession)
        {
            pSession->cRefs = 1;
            vboxGstCtrlSessionRelease(pSession);
        }
    }
    free(pszUser);
}

/**
 * Takes a session out of the list, stops its thread and reports it closed.
 *
 * @param   fNotify     Whether to tell the host (not after a VM restore).
 */
static void vboxGstCtrlRootSessionDestroy(HANDLE hDevice, UINT32 idClient, VBOXGSTCTRLSESSION *pSession, BOOL fNotify)
{
    VBOXGSTCTRLSESSION **ppCur;

    EnterCriticalSection(&g_GstCtrlCritSect);
    for (ppCur = &g_pGstCtrlSessions; *ppCur; ppCur = &(*ppCur)->pNext)
        if (*ppCur == pSession)
        {
            *ppCur = pSession->pNext;
            break;
        }
    LeaveCriticalSection(&g_GstCtrlCritSect);

    /* The session thread normally ends on its own copy of HOST_MSG_SESSION_CLOSE. */
    if (WaitForSingleObject(pSession->hThread, VBOXGSTCTRL_CLOSE_WAIT_MS) != WAIT_OBJECT_0)
    {
        vboxGstCtrlSessionDisconnect(pSession);
        WaitForSingleObject(pSession->hThread, VBOXGSTCTRL_CLOSE_WAIT_MS);
    }
    SetEvent(pSession->hEvtStop);

    if (fNotify)
        vboxGstCtrlSessionNotify(hDevice, idClient, VBOX_GUESTCTRL_CONTEXTID_MAKE_SESSION(pSession->idSession),
                                 GUEST_SESSION_NOTIFYTYPE_TEN, VINF_SUCCESS);
    VBoxTrayLogRel("GstCtrl: session %lu closed\n", pSession->idSession);
    vboxGstCtrlSessionRelease(pSession);
}

/**
 * Handles HOST_MSG_SESSION_CLOSE on the root.
 */
static void vboxGstCtrlRootSessionClose(HANDLE hDevice, UINT32 idClient)
{
    VBOXGSTCTRLSESSION *pSession;
    UINT32 idContext = 0;
    int rc;

    rc = vboxGstCtrlGetSessionClose(hDevice, idClient, &idContext);
    if (RT_FAILURE(rc))
    {
        VBoxGstCtrlSkip(hDevice, idClient, rc, HOST_MSG_SESSION_CLOSE);
        return;
    }

    EnterCriticalSection(&g_GstCtrlCritSect);
    for (pSession = g_pGstCtrlSessions; pSession; pSession = pSession->pNext)
        if (pSession->idSession == VBOX_GUESTCTRL_CONTEXTID_GET_SESSION(idContext))
            break;
    LeaveCriticalSection(&g_GstCtrlCritSect);
    if (pSession)
        vboxGstCtrlRootSessionDestroy(hDevice, idClient, pSession, TRUE);
    else
        VBoxTrayLogRel("GstCtrl: the host closed unknown session %lu\n", VBOX_GUESTCTRL_CONTEXTID_GET_SESSION(idContext));
}

/**
 * Closes all sessions without telling the host (VM restore or shutdown).
 */
static void vboxGstCtrlRootDestroyAll(HANDLE hDevice, UINT32 idClient)
{
    VBOXGSTCTRLSESSION *pSession;

    for (;;)
    {
        EnterCriticalSection(&g_GstCtrlCritSect);
        pSession = g_pGstCtrlSessions;
        LeaveCriticalSection(&g_GstCtrlCritSect);
        if (!pSession)
            break;
        vboxGstCtrlSessionDisconnect(pSession);
        vboxGstCtrlRootSessionDestroy(hDevice, idClient, pSession, FALSE);
    }
}

/**
 * Connects the root, makes it the master and reports the features.
 *
 * @returns VBox status code; VERR_HGCM_SERVICE_NOT_FOUND if the host has no
 *          guest control.
 */
static int vboxGstCtrlRootConnect(HANDLE hDevice, UINT32 *pidClient)
{
    UINT32 idClient = 0;
    int rc;
    int i;

    rc = VbglR3HgcmConnect(hDevice, VBOX_GUESTCTRL_SERVICE_NAME, &idClient);
    if (RT_FAILURE(rc))
        return rc;
    for (i = 0; i < 3; i++)
    {
        rc = VBoxGstCtrlCallU32(hDevice, idClient, GUEST_MSG_MAKE_ME_MASTER, 0, NULL);
        if (RT_SUCCESS(rc))
            break;
        Sleep(1000);
    }
    if (RT_FAILURE(rc))
    {
        VBoxTrayLogRel("GstCtrl: becoming the master failed, rc=%d\n", rc);
        VbglR3HgcmDisconnect(hDevice, idClient);
        return rc;
    }
    rc = vboxGstCtrlReportFeatures(hDevice, idClient, &g_fVBoxGstCtrlHostFeatures);
    if (RT_FAILURE(rc))
    {
        /* Like upstream, carry on without them. */
        VBoxTrayLogRel("GstCtrl: reporting the features failed, rc=%d\n", rc);
        g_fVBoxGstCtrlHostFeatures = 0;
    }

    EnterCriticalSection(&g_GstCtrlCritSect);
    g_hGstCtrlDevice  = hDevice;
    g_idGstCtrlClient = idClient;
    LeaveCriticalSection(&g_GstCtrlCritSect);
    *pidClient = idClient;
    return VINF_SUCCESS;
}

/**
 * Disconnects the root client, unless VBoxGstCtrlStop already did.
 */
static void vboxGstCtrlRootDisconnect(HANDLE hDevice)
{
    UINT32 idClient;

    EnterCriticalSection(&g_GstCtrlCritSect);
    idClient = g_idGstCtrlClient;
    g_idGstCtrlClient = 0;
    LeaveCriticalSection(&g_GstCtrlCritSect);
    vboxGstCtrlCancelAndDisconnect(hDevice, idClient);
}

/**
 * Prepares the service, before any thread starts.
 */
void VBoxGstCtrlInit(void)
{
    InitializeCriticalSection(&g_GstCtrlCritSect);
}

/**
 * Wakes the root and the session threads up: their pending
 * GUEST_MSG_PEEK_WAIT is cancelled and the clients disconnected.
 */
void VBoxGstCtrlStop(void)
{
    VBOXGSTCTRLSESSION *pSession;

    vboxGstCtrlRootDisconnect(g_hGstCtrlDevice);

    EnterCriticalSection(&g_GstCtrlCritSect);
    for (pSession = g_pGstCtrlSessions; pSession; pSession = pSession->pNext)
    {
        UINT32 idClient = pSession->idClient;
        SetEvent(pSession->hEvtStop);
        pSession->idClient = 0;
        vboxGstCtrlCancelAndDisconnect(pSession->hDevice, idClient);
    }
    LeaveCriticalSection(&g_GstCtrlCritSect);
}

/**
 * Guest control root thread.
 *
 * @returns 0 on normal exit, 1 if the service could not be used.
 * @param   pvUser      Unused.
 */
unsigned __stdcall VBoxGstCtrlThread(void *pvUser)
{
    HANDLE hDevice;
    UINT32 idClient = 0;
    UINT64 idRestore = 0;
    unsigned cErrors = 0;
    int rc;

    /* VBoxGstCtrlStop disconnects while a call waits, hence overlapped. */
    hDevice = VbglR3OpenEx(TRUE);
    if (hDevice == INVALID_HANDLE_VALUE)
        return 1;
    rc = vboxGstCtrlRootConnect(hDevice, &idClient);
    if (RT_FAILURE(rc))
    {
        if (rc == VERR_HGCM_SERVICE_NOT_FOUND)
            VBoxTrayLogRel("GstCtrl: the host has no guest control service\n");
        else
            VBoxTrayLogRel("GstCtrl: connecting to the service failed, rc=%d\n", rc);
        VbglR3Close(hDevice);
        return 1;
    }
    VbglR3QuerySessionId(hDevice, &idRestore);
    VBoxTrayLogRel("GstCtrl: connected, client %lu, host features %#lx\n", idClient, (UINT32)g_fVBoxGstCtrlHostFeatures);

    while (!g_fVBoxTrayShutdown)
    {
        UINT32 idMsg = 0;
        UINT32 cParms = 0;

        rc = vboxGstCtrlPeekWait(hDevice, idClient, &idRestore, &idMsg, &cParms);
        if (RT_SUCCESS(rc))
        {
            cErrors = 0;
            switch (idMsg)
            {
                case HOST_MSG_SESSION_CREATE:
                    vboxGstCtrlRootSessionCreate(hDevice, idClient, cParms);
                    break;
                case HOST_MSG_SESSION_CLOSE:
                    vboxGstCtrlRootSessionClose(hDevice, idClient);
                    break;
                case HOST_MSG_CANCEL_PENDING_WAITS:
                    break;
                default:
                    VBoxTrayLogRel("GstCtrl: host message %lu isn't for the root, skipped\n", idMsg);
                    VBoxGstCtrlSkip(hDevice, idClient, VERR_NOT_SUPPORTED, idMsg);
                    break;
            }
        }
        else if (g_fVBoxTrayShutdown || rc == VERR_INVALID_HANDLE)
            break; /* Disconnected by VBoxGstCtrlStop. */
        else if (rc == VERR_VM_RESTORED)
        {
            /* Like upstream: the sessions of the saved VM are gone, start over. */
            VBoxTrayLogRel("GstCtrl: the VM was restored, closing the sessions and connecting again\n");
            vboxGstCtrlRootDestroyAll(hDevice, idClient);
            vboxGstCtrlRootDisconnect(hDevice);
            rc = vboxGstCtrlRootConnect(hDevice, &idClient);
            if (RT_FAILURE(rc))
            {
                VBoxTrayLogRel("GstCtrl: connecting again failed, rc=%d\n", rc);
                break;
            }
            VbglR3QuerySessionId(hDevice, &idRestore);
        }
        else if (rc != VERR_INTERRUPTED && ++cErrors > VBOXGSTCTRL_MAX_ERRORS)
        {
            VBoxTrayLogRel("GstCtrl: too many failures (last rc=%d), giving up\n", rc);
            break;
        }
        else
            Sleep(1000);
    }

    vboxGstCtrlRootDestroyAll(hDevice, idClient);
    vboxGstCtrlRootDisconnect(hDevice);
    VbglR3Close(hDevice);
    return 0;
}
