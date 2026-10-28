/** @file
 * VBoxTray - Guest control: files, directories and file systems
 * (VBoxManage guestcontrol copyto, copyfrom, mkdir, rm, rmdir, mv, stat,
 * mktemp, fsinfo) and the mount points.
 *
 * Follows upstream VBoxServiceControlSession.cpp with
 * VBOX_GUESTCTRL_GF_0_TOOLBOX_AS_CMDS: the host sends file, directory and
 * file system messages to the session and the guest answers each one with
 * GUEST_MSG_FILE_NOTIFY, GUEST_MSG_DIR_NOTIFY, GUEST_MSG_FS_NOTIFY or
 * GUEST_MSG_REPLY.  Handles are the object IDs of the context IDs the
 * files and directories were opened with, as upstream.
 *
 * Differences from upstream, because of Windows 98:
 *  - Names are converted between UTF-8 and the ANSI code page; characters
 *    the code page doesn't have become '_'.
 *  - There are no owners or groups: they are sent empty, and the Unix
 *    permissions are made up from the read-only attribute.
 *  - Directories are read with FindFirstFile/FindNextFile; the filter of
 *    HOST_MSG_DIR_OPEN is ignored like upstream does on Windows, but a path
 *    with wildcards is used as given.
 *  - The sharing mode of HOST_MSG_FILE_OPEN is ignored, as upstream does.
 */

#include "GstCtrl.h"
#include <stdlib.h>
#include <string.h>


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/

/** Difference between the FILETIME and Unix epochs, in 100 ns units. */
#define VBOXGSTCTRL_FILETIME_UNIX_EPOCH ((UINT64)116444736 * 1000000000)
/** Size of a directory entry up to and including its name's terminator. */
#define VBOXGSTCTRL_DIRENTRY_SIZE(cbName) ((DWORD)((const BYTE *)&((GSTCTLDIRENTRYEX *)0)->szName[0] - (const BYTE *)0) \
                                           + (cbName) + 1)
/** Room for the entries of a HOST_MSG_DIR_LIST answer (upstream: 64K). */
#define VBOXGSTCTRL_DIRLIST_SIZE        (64U * 1024)


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/

/** An open file. */
typedef struct VBOXGSTCTRLFILE
{
    struct VBOXGSTCTRLFILE *pNext;
    UINT32      uHandle;
    HANDLE      hFile;
    /** Opened for appending: every write goes to the end. */
    BOOL        fAppend;
} VBOXGSTCTRLFILE;

/** An open directory. */
typedef struct VBOXGSTCTRLDIR
{
    struct VBOXGSTCTRLDIR *pNext;
    UINT32      uHandle;
    /** The FindFirstFile pattern: the path with "\*.*" unless it has wildcards. */
    char        szPattern[MAX_PATH + 4];
    /** INVALID_HANDLE_VALUE before the first read, after the last and for
     *  an empty directory. */
    HANDLE      hFind;
    /** The next entry is in Data. */
    BOOL        fHaveData;
    /** The directory was read to the end. */
    BOOL        fEnd;
    WIN32_FIND_DATAA Data;
    /** GSTCTLFSOBJATTRADD_XXX asked for. */
    UINT32      enmAttrAdd;
} VBOXGSTCTRLDIR;


/*********************************************************************************************************************************
*   Helpers                                                                                                                      *
*********************************************************************************************************************************/

/**
 * Turns a Win32 error from a file operation into a VBox status code.
 */
static int vboxGstCtrlFileRc(DWORD dwErr)
{
    switch (dwErr)
    {
        case ERROR_FILE_NOT_FOUND:      return VERR_FILE_NOT_FOUND;
        case ERROR_PATH_NOT_FOUND:
        case ERROR_INVALID_DRIVE:
        case ERROR_BAD_NETPATH:         return VERR_PATH_NOT_FOUND;
        case ERROR_ACCESS_DENIED:       return VERR_ACCESS_DENIED;
        case ERROR_FILE_EXISTS:
        case ERROR_ALREADY_EXISTS:      return VERR_ALREADY_EXISTS;
        case ERROR_DIR_NOT_EMPTY:       return VERR_DIR_NOT_EMPTY;
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:      return VERR_SHARING_VIOLATION;
        case ERROR_DISK_FULL:
        case ERROR_HANDLE_DISK_FULL:    return VERR_DISK_FULL;
        case ERROR_WRITE_PROTECT:       return VERR_WRITE_PROTECT;
        case ERROR_TOO_MANY_OPEN_FILES: return VERR_TOO_MANY_OPEN_FILES;
        case ERROR_INVALID_NAME:
        case ERROR_BAD_PATHNAME:
        case ERROR_FILENAME_EXCED_RANGE: return VERR_INVALID_NAME;
        case ERROR_NO_MORE_FILES:       return VERR_NO_MORE_FILES;
        case ERROR_NEGATIVE_SEEK:       return VERR_NEGATIVE_SEEK;
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_OUTOFMEMORY:         return VERR_NO_MEMORY;
        default:                        return VERR_FILE_IO_ERROR;
    }
}

/**
 * Fetches a host message.  Parameter 0 is the context; the others are
 * 32-bit values ('u'), 64-bit values ('q') or buffers ('b', for paths and
 * data).  The buffers grow while the host says they are too small.
 *
 * @returns TRUE if fetched (free pMsg); FALSE if not, the message was then
 *          skipped.
 * @param   pszParms    One letter per parameter, 'u' first.
 * @param   cbBuf       First size of each buffer.
 * @param   papb        Where to return the buffers, by parameter index.
 */
static BOOL vboxGstCtrlFileFetch(VBOXGSTCTRLSESSION *pSession, UINT32 idClient, UINT32 idMsg, UINT32 cParms,
                                 const char *pszParms, DWORD cbBuf, VBGLR3HGCMMSG *pMsg, BYTE **papb)
{
    UINT32 cBufs = 0;
    UINT32 i;
    int rc;

    if (strlen(pszParms) != cParms)
    {
        VBoxGstCtrlSkip(pSession->hDevice, idClient, VERR_INVALID_PARAMETER, idMsg);
        return FALSE;
    }
    for (i = 0; i < cParms; i++)
        if (pszParms[i] == 'b')
            cBufs++;

    for (;;)
    {
        if (!VbglR3HgcmMsgInit(pMsg, idClient, GUEST_MSG_GET, cParms, VBOXGSTCTRL_CALL_TIMEOUT_MS, cbBuf * cBufs))
        {
            VBoxGstCtrlSkip(pSession->hDevice, idClient, VERR_NO_MEMORY, idMsg);
            return FALSE;
        }
        for (i = 0; i < cParms; i++)
            if (pszParms[i] == 'u')
                VbglR3HgcmMsgSetU32(pMsg, i, i == 0 ? idMsg : 0);
            else if (pszParms[i] == 'q')
                VbglR3HgcmMsgSetU64(pMsg, i, 0);
            else
                papb[i] = VbglR3HgcmMsgSetBuf(pMsg, i, VBOXGUEST98_HGCM_PARM_BUFFER_OUT, cbBuf, NULL);
        rc = VBoxGstCtrlCallMsg(pSession->hDevice, pMsg);
        if (rc != VERR_BUFFER_OVERFLOW || cbBuf >= VBOXGSTCTRL_MAX_BUF_SIZE)
            break;
        VbglR3HgcmMsgFree(pMsg);
        cbBuf *= 2;
    }
    if (RT_FAILURE(rc))
    {
        VbglR3HgcmMsgFree(pMsg);
        VBoxGstCtrlSkip(pSession->hDevice, idClient, rc, idMsg);
        return FALSE;
    }
    return TRUE;
}

/**
 * Starts an answer: [0] context, [1] type, [2] status, then cExtra more
 * parameters with cbData bytes of buffers.
 *
 * @returns FALSE if out of memory.
 */
static BOOL vboxGstCtrlFileReplyInit(VBGLR3HGCMMSG *pMsg, UINT32 idClient, UINT32 uFunction, UINT32 idContext,
                                     UINT32 uType, int rc, UINT32 cExtra, DWORD cbData)
{
    if (!VbglR3HgcmMsgInit(pMsg, idClient, uFunction, 3 + cExtra, VBOXGSTCTRL_CALL_TIMEOUT_MS, cbData))
        return FALSE;
    VbglR3HgcmMsgSetU32(pMsg, 0, idContext);
    VbglR3HgcmMsgSetU32(pMsg, 1, uType);
    VbglR3HgcmMsgSetU32(pMsg, 2, (UINT32)rc);
    return TRUE;
}

/**
 * Sends an answer started with vboxGstCtrlFileReplyInit and frees it.
 */
static void vboxGstCtrlFileReplySend(VBOXGSTCTRLSESSION *pSession, VBGLR3HGCMMSG *pMsg)
{
    VBoxGstCtrlCallMsg(pSession->hDevice, pMsg);
    VbglR3HgcmMsgFree(pMsg);
}

/**
 * Sends a GUEST_MSG_REPLY with an optional zero terminated payload.
 */
static void vboxGstCtrlFileReply(VBOXGSTCTRLSESSION *pSession, UINT32 idClient, UINT32 idContext, int rc,
                                 const char *pszPayload)
{
    VBGLR3HGCMMSG Msg;
    DWORD cb = pszPayload ? (DWORD)strlen(pszPayload) + 1 : 0;

    if (!vboxGstCtrlFileReplyInit(&Msg, idClient, GUEST_MSG_REPLY, idContext, 0, rc, 1, cb))
        return;
    VbglR3HgcmMsgSetBuf(&Msg, 3, VBOXGUEST98_HGCM_PARM_BUFFER_IN, cb, pszPayload);
    vboxGstCtrlFileReplySend(pSession, &Msg);
}

/**
 * Gets a path parameter in the ANSI code page, without a trailing
 * backslash (but "C:\" stays as it is).
 *
 * @returns The path (malloc'ed), NULL if out of memory.
 */
static char *vboxGstCtrlFileGetPath(VBGLR3HGCMMSG *pMsg, UINT32 iParm, const BYTE *pb)
{
    char *psz = VBoxGstCtrlGetStr(pMsg, iParm, pb, '_');
    size_t cch;

    if (!psz)
        return NULL;
    cch = strlen(psz);
    while (cch > 1 && (psz[cch - 1] == '\\' || psz[cch - 1] == '/') && !(cch == 3 && psz[1] == ':'))
        psz[--cch] = '\0';
    return psz;
}

/**
 * Converts a FILETIME to nanoseconds since 1970 (RTTIMESPEC), 0 if unset.
 */
static INT64 vboxGstCtrlFileTime(const FILETIME *pFt)
{
    UINT64 u = ((UINT64)pFt->dwHighDateTime << 32) | pFt->dwLowDateTime;
    if (!u)
        return 0;
    return (INT64)(u - VBOXGSTCTRL_FILETIME_UNIX_EPOCH) * 100;
}

/**
 * Fills a GSTCTLFSOBJINFO from what Windows knows about a file or directory.
 *
 * @param   enmAttrAdd  GSTCTLFSOBJATTRADD_XXX the host asked for.
 */
static void vboxGstCtrlFileFillInfo(GSTCTLFSOBJINFO *pInfo, DWORD dwAttr, const FILETIME *pftCreate,
                                    const FILETIME *pftAccess, const FILETIME *pftWrite, DWORD cbHigh, DWORD cbLow,
                                    UINT32 enmAttrAdd)
{
    BOOL fDir = (dwAttr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    UINT64 cb = fDir ? 0 : ((UINT64)cbHigh << 32) | cbLow;

    memset(pInfo, 0, sizeof(*pInfo));
    pInfo->cbObject    = (INT64)cb;
    pInfo->cbAllocated = (INT64)((cb + 4095) & ~(UINT64)4095);
    pInfo->ModificationTime = pftWrite ? vboxGstCtrlFileTime(pftWrite) : 0;
    pInfo->AccessTime  = pftAccess ? vboxGstCtrlFileTime(pftAccess) : 0;
    pInfo->ChangeTime  = pInfo->ModificationTime;
    pInfo->BirthTime   = pftCreate ? vboxGstCtrlFileTime(pftCreate) : 0;
    if (!pInfo->AccessTime)
        pInfo->AccessTime = pInfo->ModificationTime;
    if (!pInfo->BirthTime)
        pInfo->BirthTime = pInfo->ChangeTime;

    /* Like upstream's rtFsModeFromDos: readable by all, writable unless
       read-only, directories searchable. */
    pInfo->Attr.fMode = (fDir ? RTFS_TYPE_DIRECTORY : RTFS_TYPE_FILE)
                      | 0444 | ((dwAttr & FILE_ATTRIBUTE_READONLY) ? 0 : 0222) | (fDir ? 0111 : 0)
                      | ((dwAttr & 0x7fff) << RTFS_DOS_SHIFT);
    if (enmAttrAdd == GSTCTLFSOBJATTRADD_UNIX)
    {
        pInfo->Attr.enmAdditional = GSTCTLFSOBJATTRADD_UNIX;
        pInfo->Attr.u.Unix.uid = ~0U; /* NIL_RTUID: no owners on Windows 98. */
        pInfo->Attr.u.Unix.gid = ~0U;
        pInfo->Attr.u.Unix.cHardlinks = 1;
    }
    else
        pInfo->Attr.enmAdditional = GSTCTLFSOBJATTRADD_NOTHING;
}

/**
 * Gets the information of a file or directory by path.
 *
 * @returns VBox status code.
 */
static int vboxGstCtrlFileStat(const char *pszPath, UINT32 enmAttrAdd, GSTCTLFSOBJINFO *pInfo)
{
    WIN32_FILE_ATTRIBUTE_DATA Data;
    DWORD dwAttr;

    if (GetFileAttributesExA(pszPath, GetFileExInfoStandard, &Data))
    {
        vboxGstCtrlFileFillInfo(pInfo, Data.dwFileAttributes, &Data.ftCreationTime, &Data.ftLastAccessTime,
                                &Data.ftLastWriteTime, Data.nFileSizeHigh, Data.nFileSizeLow, enmAttrAdd);
        return VINF_SUCCESS;
    }

    /* The root of a drive has no times. */
    dwAttr = GetFileAttributesA(pszPath);
    if (dwAttr == 0xffffffff)
        return vboxGstCtrlFileRc(GetLastError());
    vboxGstCtrlFileFillInfo(pInfo, dwAttr, NULL, NULL, NULL, 0, 0, enmAttrAdd);
    return VINF_SUCCESS;
}

/**
 * Finds an open file.
 */
static VBOXGSTCTRLFILE *vboxGstCtrlFileFind(VBOXGSTCTRLSESSION *pSession, UINT32 uHandle)
{
    VBOXGSTCTRLFILE *pFile;
    for (pFile = pSession->pFiles; pFile; pFile = pFile->pNext)
        if (pFile->uHandle == uHandle)
            return pFile;
    return NULL;
}

/**
 * Finds an open directory.
 */
static VBOXGSTCTRLDIR *vboxGstCtrlFileFindDir(VBOXGSTCTRLSESSION *pSession, UINT32 uHandle)
{
    VBOXGSTCTRLDIR *pDir;
    for (pDir = pSession->pDirs; pDir; pDir = pDir->pNext)
        if (pDir->uHandle == uHandle)
            return pDir;
    return NULL;
}

/**
 * Gets the current position of a file.
 */
static UINT64 vboxGstCtrlFileTell(HANDLE hFile)
{
    LONG offHigh = 0;
    DWORD offLow = SetFilePointer(hFile, 0, &offHigh, FILE_CURRENT);
    if (offLow == 0xffffffff && GetLastError() != NO_ERROR)
        return 0;
    return ((UINT64)(DWORD)offHigh << 32) | offLow;
}

/**
 * Moves the position of a file.
 *
 * @returns VBox status code.
 * @param   dwMethod    FILE_BEGIN, FILE_CURRENT or FILE_END.
 */
static int vboxGstCtrlFileSeekTo(HANDLE hFile, INT64 off, DWORD dwMethod)
{
    LONG offHigh = (LONG)(off >> 32);
    DWORD offLow = SetFilePointer(hFile, (LONG)(DWORD)off, &offHigh, dwMethod);
    if (offLow == 0xffffffff && GetLastError() != NO_ERROR)
        return vboxGstCtrlFileRc(GetLastError());
    return VINF_SUCCESS;
}


/*********************************************************************************************************************************
*   Files                                                                                                                        *
*********************************************************************************************************************************/

/**
 * Handles HOST_MSG_FILE_OPEN.
 */
static void vboxGstCtrlFileOpen(VBOXGSTCTRLSESSION *pSession, UINT32 idClient, UINT32 cParms)
{
    VBGLR3HGCMMSG Msg;
    BYTE *apb[7];
    VBOXGSTCTRLFILE *pFile = NULL;
    char *pszName;
    char *pszAccess;
    char *pszDisp;
    const char *pch;
    UINT32 idContext;
    UINT64 offOpen;
    DWORD fAccess = 0;
    DWORD dwDisp = 0;
    BOOL fAppend = FALSE;
    UINT32 uHandle = 0;
    HANDLE hFile;
    int rc = VINF_SUCCESS;

    if (!vboxGstCtrlFileFetch(pSession, idClient, HOST_MSG_FILE_OPEN, cParms, "ubbbbuq", VBOXGSTCTRL_DEF_BUF_SIZE,
                              &Msg, apb))
        return;
    idContext = VbglR3HgcmMsgGetU32(&Msg, 0);
    offOpen   = VbglR3HgcmMsgGetU64(&Msg, 6);
    pszName   = vboxGstCtrlFileGetPath(&Msg, 1, apb[1]);
    pszAccess = VBoxGstCtrlGetStr(&Msg, 2, apb[2], '?');
    pszDisp   = VBoxGstCtrlGetStr(&Msg, 3, apb[3], '?');
    VbglR3HgcmMsgFree(&Msg);

    if (!pszName || !pszAccess || !pszDisp)
        rc = VERR_NO_MEMORY;
    else if (!*pszName)
        rc = VERR_INVALID_NAME;

    /* The access, like upstream's RTFileModeToFlagsEx. */
    if (RT_SUCCESS(rc))
    {
        char chPrev = '\0';
        for (pch = pszAccess; *pch; pch++)
        {
            switch (*pch)
            {
                case 'r': fAccess |= GENERIC_READ; break;
                case 'w': fAccess |= GENERIC_WRITE; break;
                case 'a': fAccess |= GENERIC_WRITE; fAppend = TRUE; break;
                case '+':
                    if (chPrev == 'r')
                        fAccess |= GENERIC_WRITE;
                    else if (chPrev == 'w' || chPrev == 'a')
                        fAccess |= GENERIC_READ;
                    break;
                case 'b':
                case 't':
                    break;
                default:
                    rc = VERR_INVALID_PARAMETER;
                    break;
            }
            chPrev = *pch;
        }
        if (!fAccess)
            rc = VERR_INVALID_PARAMETER;
    }
    if (RT_SUCCESS(rc))
    {
        if (!strcmp(pszDisp, "ca") || !strcmp(pszDisp, "create-replace"))
            dwDisp = CREATE_ALWAYS;
        else if (!strcmp(pszDisp, "ce") || !strcmp(pszDisp, "create"))
            dwDisp = CREATE_NEW;
        else if (!strcmp(pszDisp, "oc") || !strcmp(pszDisp, "open-create"))
            dwDisp = OPEN_ALWAYS;
        else if (!strcmp(pszDisp, "oa") || !strcmp(pszDisp, "open-append"))
        {
            dwDisp = OPEN_ALWAYS;
            fAppend = TRUE;
        }
        else if (!strcmp(pszDisp, "oe") || !strcmp(pszDisp, "open"))
            dwDisp = OPEN_EXISTING;
        else if (!strcmp(pszDisp, "ot") || !strcmp(pszDisp, "open-truncate"))
            /* Upstream drops the truncation when reading. */
            dwDisp = (fAccess & GENERIC_WRITE) && !(fAccess & GENERIC_READ) ? TRUNCATE_EXISTING : OPEN_EXISTING;
        else
            rc = VERR_INVALID_PARAMETER;
    }

    if (RT_SUCCESS(rc))
    {
        hFile = CreateFileA(pszName, fAccess, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, dwDisp, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE)
            rc = vboxGstCtrlFileRc(GetLastError());
        else
        {
            if (offOpen)
                rc = vboxGstCtrlFileSeekTo(hFile, (INT64)offOpen, FILE_BEGIN);
            pFile = RT_SUCCESS(rc) ? (VBOXGSTCTRLFILE *)calloc(1, sizeof(*pFile)) : NULL;
            if (pFile)
            {
                uHandle = (idContext >> 16) & 0x7ff; /* VBOX_GUESTCTRL_CONTEXTID_GET_OBJECT */
                pFile->uHandle = uHandle;
                pFile->hFile   = hFile;
                pFile->fAppend = fAppend;
                pFile->pNext   = pSession->pFiles;
                pSession->pFiles = pFile;
            }
            else
            {
                if (RT_SUCCESS(rc))
                    rc = VERR_NO_MEMORY;
                CloseHandle(hFile);
            }
        }
    }
    if (RT_FAILURE(rc))
        VBoxTrayLogRel("GstCtrl: opening \"%s\" (%s, %s) failed, rc=%d\n", pszName ? pszName : "",
                       pszAccess ? pszAccess : "", pszDisp ? pszDisp : "", rc);

    if (vboxGstCtrlFileReplyInit(&Msg, idClient, GUEST_MSG_FILE_NOTIFY, idContext, GUEST_FILE_NOTIFYTYPE_OPEN, rc, 1, 0))
    {
        VbglR3HgcmMsgSetU32(&Msg, 3, uHandle);
        vboxGstCtrlFileReplySend(pSession, &Msg);
    }
    free(pszName);
    free(pszAccess);
    free(pszDisp);
}

/**
 * Handles HOST_MSG_FILE_CLOSE.
 */
static void vboxGstCtrlFileClose(VBOXGSTCTRLSESSION *pSession, UINT32 idClient, UINT32 cParms)
{
    VBGLR3HGCMMSG Msg;
    VBOXGSTCTRLFILE **ppCur;
    VBOXGSTCTRLFILE *pFile = NULL;
    UINT32 idContext;
    UINT32 uHandle;
    int rc = VERR_NOT_FOUND;

    if (!vboxGstCtrlFileFetch(pSession, idClient, HOST_MSG_FILE_CLOSE, cParms, "uu", 0, &Msg, NULL))
        return;
    idContext = VbglR3HgcmMsgGetU32(&Msg, 0);
    uHandle   = VbglR3HgcmMsgGetU32(&Msg, 1);
    VbglR3HgcmMsgFree(&Msg);

    for (ppCur = &pSession->pFiles; *ppCur; ppCur = &(*ppCur)->pNext)
        if ((*ppCur)->uHandle == uHandle)
        {
            pFile = *ppCur;
            *ppCur = pFile->pNext;
            break;
        }
    if (pFile)
    {
        rc = CloseHandle(pFile->hFile) ? VINF_SUCCESS : vboxGstCtrlFileRc(GetLastError());
        free(pFile);
    }

    if (vboxGstCtrlFileReplyInit(&Msg, idClient, GUEST_MSG_FILE_NOTIFY, idContext, GUEST_FILE_NOTIFYTYPE_CLOSE, rc, 0, 0))
        vboxGstCtrlFileReplySend(pSession, &Msg);
}

/**
 * Handles HOST_MSG_FILE_READ and HOST_MSG_FILE_READ_AT.
 */
static void vboxGstCtrlFileRead(VBOXGSTCTRLSESSION *pSession, UINT32 idClient, UINT32 idMsg, UINT32 cParms)
{
    VBGLR3HGCMMSG Msg;
    VBOXGSTCTRLFILE *pFile;
    BOOL fAt = idMsg == HOST_MSG_FILE_READ_AT;
    BOOL fOffset = (g_fVBoxGstCtrlHostFeatures & VBOX_GUESTCTRL_HF_0_NOTIFY_RDWR_OFFSET) != 0;
    BYTE *pbData;
    UINT32 idContext;
    UINT32 uHandle;
    UINT32 cbToRead;
    UINT64 offAt = 0;
    UINT64 offNew = 0;
    DWORD cbRead = 0;
    int rc = VERR_NOT_FOUND;

    if (!vboxGstCtrlFileFetch(pSession, idClient, idMsg, cParms, fAt ? "uuqu" : "uuu", 0, &Msg, NULL))
        return;
    idContext = VbglR3HgcmMsgGetU32(&Msg, 0);
    uHandle   = VbglR3HgcmMsgGetU32(&Msg, 1);
    if (fAt)
    {
        offAt    = VbglR3HgcmMsgGetU64(&Msg, 2);
        cbToRead = VbglR3HgcmMsgGetU32(&Msg, 3);
    }
    else
        cbToRead = VbglR3HgcmMsgGetU32(&Msg, 2);
    VbglR3HgcmMsgFree(&Msg);
    if (cbToRead > VBOXGSTCTRL_OUTPUT_SIZE)
        cbToRead = VBOXGSTCTRL_OUTPUT_SIZE; /* Upstream too reads what fits in its buffer. */

    if (!vboxGstCtrlFileReplyInit(&Msg, idClient, GUEST_MSG_FILE_NOTIFY, idContext,
                                  fOffset ? GUEST_FILE_NOTIFYTYPE_READ_OFFSET : GUEST_FILE_NOTIFYTYPE_READ, 0,
                                  fOffset ? 2 : 1, cbToRead))
        return;
    pbData = VbglR3HgcmMsgSetBuf(&Msg, 3, VBOXGUEST98_HGCM_PARM_BUFFER_IN, cbToRead, NULL);

    pFile = vboxGstCtrlFileFind(pSession, uHandle);
    if (pFile)
    {
        rc = fAt ? vboxGstCtrlFileSeekTo(pFile->hFile, (INT64)offAt, FILE_BEGIN) : VINF_SUCCESS;
        if (RT_SUCCESS(rc) && !ReadFile(pFile->hFile, pbData, cbToRead, &cbRead, NULL))
        {
            rc = vboxGstCtrlFileRc(GetLastError());
            cbRead = 0;
        }
        offNew = vboxGstCtrlFileTell(pFile->hFile);
    }

    VbglR3HgcmMsgSetU32(&Msg, 2, (UINT32)rc);
    VBOXGUEST98_HGCM_CALL_PARMS(Msg.pCall)[3].cb = cbRead;
    if (fOffset)
        VbglR3HgcmMsgSetU64(&Msg, 4, offNew);
    vboxGstCtrlFileReplySend(pSession, &Msg);
}

/**
 * Handles HOST_MSG_FILE_WRITE and HOST_MSG_FILE_WRITE_AT.
 */
static void vboxGstCtrlFileWrite(VBOXGSTCTRLSESSION *pSession, UINT32 idClient, UINT32 idMsg, UINT32 cParms)
{
    VBGLR3HGCMMSG Msg;
    VBOXGSTCTRLFILE *pFile;
    BOOL fAt = idMsg == HOST_MSG_FILE_WRITE_AT;
    BOOL fOffset = (g_fVBoxGstCtrlHostFeatures & VBOX_GUESTCTRL_HF_0_NOTIFY_RDWR_OFFSET) != 0;
    BYTE *apb[5];
    UINT32 idContext;
    UINT32 uHandle;
    UINT32 cbData;
    UINT64 offAt = 0;
    UINT64 offNew = 0;
    DWORD cbWritten = 0;
    UINT32 iData;
    int rc = VERR_NOT_FOUND;

    /* The order of HGCMMsgFileWrite(At): the size comes before the data. */
    if (!vboxGstCtrlFileFetch(pSession, idClient, idMsg, cParms, fAt ? "uuqub" : "uuub", VBOXGSTCTRL_OUTPUT_SIZE,
                              &Msg, apb))
        return;
    idContext = VbglR3HgcmMsgGetU32(&Msg, 0);
    uHandle   = VbglR3HgcmMsgGetU32(&Msg, 1);
    if (fAt)
        offAt = VbglR3HgcmMsgGetU64(&Msg, 2);
    iData     = fAt ? 4 : 3;
    cbData    = VbglR3HgcmMsgGetU32(&Msg, iData - 1);
    if (cbData > VbglR3HgcmMsgGetBufSize(&Msg, iData))
        cbData = VbglR3HgcmMsgGetBufSize(&Msg, iData);

    pFile = vboxGstCtrlFileFind(pSession, uHandle);
    if (pFile)
    {
        if (fAt)
            rc = vboxGstCtrlFileSeekTo(pFile->hFile, (INT64)offAt, FILE_BEGIN);
        else if (pFile->fAppend)
            rc = vboxGstCtrlFileSeekTo(pFile->hFile, 0, FILE_END);
        else
            rc = VINF_SUCCESS;
        if (RT_SUCCESS(rc) && cbData && !WriteFile(pFile->hFile, apb[iData], cbData, &cbWritten, NULL))
            rc = vboxGstCtrlFileRc(GetLastError());
        offNew = vboxGstCtrlFileTell(pFile->hFile);
    }
    VbglR3HgcmMsgFree(&Msg);

    if (vboxGstCtrlFileReplyInit(&Msg, idClient, GUEST_MSG_FILE_NOTIFY, idContext,
                                 fOffset ? GUEST_FILE_NOTIFYTYPE_WRITE_OFFSET : GUEST_FILE_NOTIFYTYPE_WRITE, rc,
                                 fOffset ? 2 : 1, 0))
    {
        VbglR3HgcmMsgSetU32(&Msg, 3, cbWritten);
        if (fOffset)
            VbglR3HgcmMsgSetU64(&Msg, 4, offNew);
        vboxGstCtrlFileReplySend(pSession, &Msg);
    }
}

/**
 * Handles HOST_MSG_FILE_SEEK, HOST_MSG_FILE_TELL and HOST_MSG_FILE_SET_SIZE,
 * which all answer with an offset or size.
 */
static void vboxGstCtrlFilePosition(VBOXGSTCTRLSESSION *pSession, UINT32 idClient, UINT32 idMsg, UINT32 cParms)
{
    VBGLR3HGCMMSG Msg;
    VBOXGSTCTRLFILE *pFile;
    UINT32 idContext;
    UINT32 uHandle;
    UINT32 uMethod = 0;
    UINT32 uType;
    UINT64 u64 = 0;
    UINT64 u64Answer = 0;
    int rc = VERR_NOT_FOUND;

    if (!vboxGstCtrlFileFetch(pSession, idClient, idMsg, cParms,
                              idMsg == HOST_MSG_FILE_SEEK ? "uuuq" : idMsg == HOST_MSG_FILE_TELL ? "uu" : "uuq",
                              0, &Msg, NULL))
        return;
    idContext = VbglR3HgcmMsgGetU32(&Msg, 0);
    uHandle   = VbglR3HgcmMsgGetU32(&Msg, 1);
    if (idMsg == HOST_MSG_FILE_SEEK)
    {
        uMethod = VbglR3HgcmMsgGetU32(&Msg, 2);
        u64     = VbglR3HgcmMsgGetU64(&Msg, 3);
    }
    else if (idMsg == HOST_MSG_FILE_SET_SIZE)
        u64 = VbglR3HgcmMsgGetU64(&Msg, 2);
    VbglR3HgcmMsgFree(&Msg);

    pFile = vboxGstCtrlFileFind(pSession, uHandle);
    if (pFile)
    {
        if (idMsg == HOST_MSG_FILE_SEEK)
        {
            uType = GUEST_FILE_NOTIFYTYPE_SEEK;
            rc = vboxGstCtrlFileSeekTo(pFile->hFile, (INT64)u64,
                                         uMethod == GUEST_FILE_SEEKTYPE_CURRENT ? FILE_CURRENT
                                       : uMethod == GUEST_FILE_SEEKTYPE_END ? FILE_END : FILE_BEGIN);
            u64Answer = vboxGstCtrlFileTell(pFile->hFile);
        }
        else if (idMsg == HOST_MSG_FILE_TELL)
        {
            uType = GUEST_FILE_NOTIFYTYPE_TELL;
            u64Answer = vboxGstCtrlFileTell(pFile->hFile);
            rc = VINF_SUCCESS;
        }
        else
        {
            /* Like RTFileSetSize, the position stays where it was. */
            UINT64 offOld = vboxGstCtrlFileTell(pFile->hFile);
            uType = GUEST_FILE_NOTIFYTYPE_SET_SIZE;
            rc = vboxGstCtrlFileSeekTo(pFile->hFile, (INT64)u64, FILE_BEGIN);
            if (RT_SUCCESS(rc) && !SetEndOfFile(pFile->hFile))
                rc = vboxGstCtrlFileRc(GetLastError());
            vboxGstCtrlFileSeekTo(pFile->hFile, (INT64)offOld, FILE_BEGIN);
            u64Answer = u64;
        }
    }
    else
        uType = idMsg == HOST_MSG_FILE_SEEK ? GUEST_FILE_NOTIFYTYPE_SEEK
              : idMsg == HOST_MSG_FILE_TELL ? GUEST_FILE_NOTIFYTYPE_TELL : GUEST_FILE_NOTIFYTYPE_SET_SIZE;

    if (vboxGstCtrlFileReplyInit(&Msg, idClient, GUEST_MSG_FILE_NOTIFY, idContext, uType, rc, 1, 0))
    {
        VbglR3HgcmMsgSetU64(&Msg, 3, u64Answer);
        vboxGstCtrlFileReplySend(pSession, &Msg);
    }
}

/**
 * Handles HOST_MSG_FILE_REMOVE.
 */
static void vboxGstCtrlFileRemove(VBOXGSTCTRLSESSION *pSession, UINT32 idClient, UINT32 cParms)
{
    VBGLR3HGCMMSG Msg;
    BYTE *apb[2];
    UINT32 idContext;
    char *pszName;
    int rc = VERR_NO_MEMORY;

    if (!vboxGstCtrlFileFetch(pSession, idClient, HOST_MSG_FILE_REMOVE, cParms, "ub", VBOXGSTCTRL_DEF_BUF_SIZE, &Msg, apb))
        return;
    idContext = VbglR3HgcmMsgGetU32(&Msg, 0);
    pszName   = vboxGstCtrlFileGetPath(&Msg, 1, apb[1]);
    VbglR3HgcmMsgFree(&Msg);

    if (pszName)
        rc = DeleteFileA(pszName) ? VINF_SUCCESS : vboxGstCtrlFileRc(GetLastError());
    vboxGstCtrlFileReply(pSession, idClient, idContext, rc, NULL);
    free(pszName);
}


/*********************************************************************************************************************************
*   Directories                                                                                                                  *
*********************************************************************************************************************************/

/**
 * Gets the next entry of a directory.
 *
 * @returns TRUE if pDir->Data has it, FALSE at the end.
 */
static BOOL vboxGstCtrlDirNext(VBOXGSTCTRLDIR *pDir)
{
    if (pDir->fHaveData)
    {
        pDir->fHaveData = FALSE;
        return TRUE;
    }
    if (pDir->fEnd)
        return FALSE;
    if (pDir->hFind == INVALID_HANDLE_VALUE)
    {
        pDir->hFind = FindFirstFileA(pDir->szPattern, &pDir->Data);
        if (pDir->hFind != INVALID_HANDLE_VALUE)
            return TRUE;
    }
    else if (FindNextFileA(pDir->hFind, &pDir->Data))
        return TRUE;

    if (pDir->hFind != INVALID_HANDLE_VALUE)
        FindClose(pDir->hFind);
    pDir->hFind = INVALID_HANDLE_VALUE;
    pDir->fEnd = TRUE;
    return FALSE;
}

/**
 * Builds the GSTCTLDIRENTRYEX of the current entry of a directory.
 *
 * @returns Its size up to the name's terminator.
 */
static DWORD vboxGstCtrlDirEntry(VBOXGSTCTRLDIR *pDir, GSTCTLDIRENTRYEX *pEntry)
{
    char *pszName = VBoxTrayAnsiToUtf8(pDir->Data.cFileName);
    size_t cbName = pszName ? strlen(pszName) : 0;
    int cwc;

    memset(pEntry, 0, sizeof(*pEntry));
    vboxGstCtrlFileFillInfo(&pEntry->Info, pDir->Data.dwFileAttributes, &pDir->Data.ftCreationTime,
                            &pDir->Data.ftLastAccessTime, &pDir->Data.ftLastWriteTime, pDir->Data.nFileSizeHigh,
                            pDir->Data.nFileSizeLow, pDir->enmAttrAdd);
    if (pDir->Data.cAlternateFileName[0])
    {
        cwc = MultiByteToWideChar(CP_ACP, 0, pDir->Data.cAlternateFileName, -1, pEntry->wszShortName,
                                  sizeof(pEntry->wszShortName) / sizeof(WCHAR));
        pEntry->cwcShortName = (USHORT)(cwc > 0 ? cwc - 1 : 0);
    }
    if (cbName >= sizeof(pEntry->szName))
        cbName = sizeof(pEntry->szName) - 1;
    if (pszName)
        memcpy(pEntry->szName, pszName, cbName);
    pEntry->cbName = (USHORT)cbName;
    free(pszName);
    return VBOXGSTCTRL_DIRENTRY_SIZE(cbName);
}

/**
 * Handles HOST_MSG_DIR_OPEN.
 */
static void vboxGstCtrlDirOpen(VBOXGSTCTRLSESSION *pSession, UINT32 idClient, UINT32 cParms)
{
    VBGLR3HGCMMSG Msg;
    BYTE *apb[6];
    VBOXGSTCTRLDIR *pDir;
    UINT32 idContext;
    UINT32 enmAttrAdd;
    UINT32 uHandle = 0;
    char *pszPath;
    DWORD dwAttr;
    int rc = VINF_SUCCESS;

    if (!vboxGstCtrlFileFetch(pSession, idClient, HOST_MSG_DIR_OPEN, cParms, "ubuuuu", VBOXGSTCTRL_DEF_BUF_SIZE,
                              &Msg, apb))
        return;
    idContext  = VbglR3HgcmMsgGetU32(&Msg, 0);
    enmAttrAdd = VbglR3HgcmMsgGetU32(&Msg, 4);
    pszPath    = vboxGstCtrlFileGetPath(&Msg, 1, apb[1]);
    VbglR3HgcmMsgFree(&Msg);

    pDir = pszPath ? (VBOXGSTCTRLDIR *)calloc(1, sizeof(*pDir)) : NULL;
    if (!pDir)
        rc = VERR_NO_MEMORY;
    else if (strlen(pszPath) + 4 >= sizeof(pDir->szPattern))
        rc = VERR_FILENAME_TOO_LONG;
    else if (strpbrk(pszPath, "*?"))
        lstrcpy(pDir->szPattern, pszPath);
    else
    {
        dwAttr = GetFileAttributesA(pszPath);
        if (dwAttr == 0xffffffff)
            rc = vboxGstCtrlFileRc(GetLastError());
        else if (!(dwAttr & FILE_ATTRIBUTE_DIRECTORY))
            rc = VERR_NOT_A_DIRECTORY;
        else
            wsprintf(pDir->szPattern, pszPath[strlen(pszPath) - 1] == '\\' ? "%s*.*" : "%s\\*.*", pszPath);
    }

    if (RT_SUCCESS(rc))
    {
        uHandle = (idContext >> 16) & 0x7ff; /* VBOX_GUESTCTRL_CONTEXTID_GET_OBJECT */
        pDir->uHandle    = uHandle;
        pDir->hFind      = INVALID_HANDLE_VALUE;
        pDir->enmAttrAdd = enmAttrAdd;
        pDir->pNext      = pSession->pDirs;
        pSession->pDirs  = pDir;
    }
    else
    {
        VBoxTrayLogRel("GstCtrl: opening the directory \"%s\" failed, rc=%d\n", pszPath ? pszPath : "", rc);
        free(pDir);
    }

    if (vboxGstCtrlFileReplyInit(&Msg, idClient, GUEST_MSG_DIR_NOTIFY, idContext, GUEST_DIR_NOTIFYTYPE_OPEN, rc, 1, 0))
    {
        VbglR3HgcmMsgSetU32(&Msg, 3, uHandle);
        vboxGstCtrlFileReplySend(pSession, &Msg);
    }
    free(pszPath);
}

/**
 * Handles HOST_MSG_DIR_CLOSE and HOST_MSG_DIR_REWIND.
 */
static void vboxGstCtrlDirCloseRewind(VBOXGSTCTRLSESSION *pSession, UINT32 idClient, UINT32 idMsg, UINT32 cParms)
{
    VBGLR3HGCMMSG Msg;
    VBOXGSTCTRLDIR **ppCur;
    VBOXGSTCTRLDIR *pDir = NULL;
    UINT32 idContext;
    UINT32 uHandle;
    int rc = VERR_NOT_FOUND;

    if (!vboxGstCtrlFileFetch(pSession, idClient, idMsg, cParms, "uu", 0, &Msg, NULL))
        return;
    idContext = VbglR3HgcmMsgGetU32(&Msg, 0);
    uHandle   = VbglR3HgcmMsgGetU32(&Msg, 1);
    VbglR3HgcmMsgFree(&Msg);

    for (ppCur = &pSession->pDirs; *ppCur; ppCur = &(*ppCur)->pNext)
        if ((*ppCur)->uHandle == uHandle)
        {
            pDir = *ppCur;
            if (idMsg == HOST_MSG_DIR_CLOSE)
                *ppCur = pDir->pNext;
            break;
        }
    if (pDir)
    {
        if (pDir->hFind != INVALID_HANDLE_VALUE)
            FindClose(pDir->hFind);
        pDir->hFind     = INVALID_HANDLE_VALUE;
        pDir->fHaveData = FALSE;
        pDir->fEnd      = FALSE;
        if (idMsg == HOST_MSG_DIR_CLOSE)
            free(pDir);
        rc = VINF_SUCCESS;
    }

    if (vboxGstCtrlFileReplyInit(&Msg, idClient, GUEST_MSG_DIR_NOTIFY, idContext,
                                 idMsg == HOST_MSG_DIR_CLOSE ? GUEST_DIR_NOTIFYTYPE_CLOSE : GUEST_DIR_NOTIFYTYPE_REWIND,
                                 rc, 0, 0))
        vboxGstCtrlFileReplySend(pSession, &Msg);
}

/**
 * Handles HOST_MSG_DIR_READ: one entry, VERR_NO_MORE_FILES at the end.
 */
static void vboxGstCtrlDirRead(VBOXGSTCTRLSESSION *pSession, UINT32 idClient, UINT32 cParms)
{
    VBGLR3HGCMMSG Msg;
    VBOXGSTCTRLDIR *pDir;
    GSTCTLDIRENTRYEX Entry;
    UINT32 idContext;
    DWORD cbEntry = 0;
    int rc = VERR_NOT_FOUND;

    if (!vboxGstCtrlFileFetch(pSession, idClient, HOST_MSG_DIR_READ, cParms, "uu", 0, &Msg, NULL))
        return;
    idContext = VbglR3HgcmMsgGetU32(&Msg, 0);
    pDir = vboxGstCtrlFileFindDir(pSession, VbglR3HgcmMsgGetU32(&Msg, 1));
    VbglR3HgcmMsgFree(&Msg);

    if (pDir)
    {
        if (vboxGstCtrlDirNext(pDir))
        {
            cbEntry = vboxGstCtrlDirEntry(pDir, &Entry);
            rc = VINF_SUCCESS;
        }
        else
            rc = VERR_NO_MORE_FILES;
    }

    /* No owners on Windows 98: empty user and groups. */
    if (vboxGstCtrlFileReplyInit(&Msg, idClient, GUEST_MSG_DIR_NOTIFY, idContext, GUEST_DIR_NOTIFYTYPE_READ, rc, 3,
                                 cbEntry + 2))
    {
        VbglR3HgcmMsgSetBuf(&Msg, 3, VBOXGUEST98_HGCM_PARM_BUFFER_IN, cbEntry, &Entry);
        VbglR3HgcmMsgSetBuf(&Msg, 4, VBOXGUEST98_HGCM_PARM_BUFFER_IN, 1, "");
        VbglR3HgcmMsgSetBuf(&Msg, 5, VBOXGUEST98_HGCM_PARM_BUFFER_IN, 1, "");
        vboxGstCtrlFileReplySend(pSession, &Msg);
    }
}

/**
 * Handles HOST_MSG_DIR_LIST: up to the wanted number of entries, each
 * packed as GSTCTLDIRENTRYLISTHDR, entry, user and groups.
 */
static void vboxGstCtrlDirList(VBOXGSTCTRLSESSION *pSession, UINT32 idClient, UINT32 cParms)
{
    VBGLR3HGCMMSG Msg;
    VBOXGSTCTRLDIR *pDir;
    GSTCTLDIRENTRYEX Entry;
    GSTCTLDIRENTRYLISTHDR Hdr;
    BYTE *pbBuf;
    UINT32 idContext;
    UINT32 cWanted;
    UINT32 cEntries = 0;
    DWORD cbUsed = 0;
    int rc = VERR_NOT_FOUND;

    if (!vboxGstCtrlFileFetch(pSession, idClient, HOST_MSG_DIR_LIST, cParms, "uuuu", 0, &Msg, NULL))
        return;
    idContext = VbglR3HgcmMsgGetU32(&Msg, 0);
    pDir      = vboxGstCtrlFileFindDir(pSession, VbglR3HgcmMsgGetU32(&Msg, 1));
    cWanted   = VbglR3HgcmMsgGetU32(&Msg, 2);
    VbglR3HgcmMsgFree(&Msg);

    if (!vboxGstCtrlFileReplyInit(&Msg, idClient, GUEST_MSG_DIR_NOTIFY, idContext, GUEST_DIR_NOTIFYTYPE_LIST, 0, 2,
                                  VBOXGSTCTRL_DIRLIST_SIZE))
        return;
    pbBuf = VbglR3HgcmMsgSetBuf(&Msg, 4, VBOXGUEST98_HGCM_PARM_BUFFER_IN, VBOXGSTCTRL_DIRLIST_SIZE, NULL);

    if (pDir)
    {
        rc = VINF_SUCCESS;
        while (cEntries < cWanted)
        {
            DWORD cbEntry;
            if (VBOXGSTCTRL_DIRLIST_SIZE - cbUsed < sizeof(Hdr) + sizeof(Entry) + 2)
                break; /* Full; the host asks for more. */
            if (!vboxGstCtrlDirNext(pDir))
            {
                if (!cEntries)
                    rc = VERR_NO_MORE_FILES;
                break;
            }
            cbEntry = vboxGstCtrlDirEntry(pDir, &Entry);
            Hdr.cbDirEntryEx = cbEntry;
            Hdr.cbUser       = 1;
            Hdr.cbGroups     = 1;
            memcpy(pbBuf + cbUsed, &Hdr, sizeof(Hdr));
            cbUsed += sizeof(Hdr);
            memcpy(pbBuf + cbUsed, &Entry, cbEntry);
            cbUsed += cbEntry;
            pbBuf[cbUsed++] = '\0'; /* No user, */
            pbBuf[cbUsed++] = '\0'; /* no groups. */
            cEntries++;
        }
    }

    VbglR3HgcmMsgSetU32(&Msg, 2, (UINT32)rc);
    VbglR3HgcmMsgSetU32(&Msg, 3, cEntries);
    VBOXGUEST98_HGCM_CALL_PARMS(Msg.pCall)[4].cb = cbUsed;
    vboxGstCtrlFileReplySend(pSession, &Msg);
}

/**
 * Creates a directory, and its parents if asked to.
 *
 * @returns VBox status code; VINF_SUCCESS if it already exists and
 *          fParents is set, like RTDirCreateFullPath.
 */
static int vboxGstCtrlDirCreatePath(char *pszPath, BOOL fParents)
{
    DWORD dwAttr;
    DWORD dwErr;
    char *pch;

    if (CreateDirectoryA(pszPath, NULL))
        return VINF_SUCCESS;
    dwErr = GetLastError();
    dwAttr = GetFileAttributesA(pszPath);
    if (dwAttr != 0xffffffff)
    {
        /* Windows 9x may say access denied for an existing directory. */
        if (!(dwAttr & FILE_ATTRIBUTE_DIRECTORY))
            return VERR_ALREADY_EXISTS;
        return fParents ? VINF_SUCCESS : VERR_ALREADY_EXISTS;
    }
    if (!fParents || dwErr != ERROR_PATH_NOT_FOUND)
        return vboxGstCtrlFileRc(dwErr);

    /* Create the parent first. */
    pch = strrchr(pszPath, '\\');
    if (!pch || pch == pszPath || (pch == pszPath + 2 && pszPath[1] == ':'))
        return VERR_PATH_NOT_FOUND;
    *pch = '\0';
    dwErr = (DWORD)vboxGstCtrlDirCreatePath(pszPath, TRUE);
    *pch = '\\';
    if (RT_FAILURE((int)dwErr))
        return (int)dwErr;
    return CreateDirectoryA(pszPath, NULL) ? VINF_SUCCESS : vboxGstCtrlFileRc(GetLastError());
}

/**
 * Handles HOST_MSG_DIR_CREATE.
 */
static void vboxGstCtrlDirCreate(VBOXGSTCTRLSESSION *pSession, UINT32 idClient, UINT32 cParms)
{
    VBGLR3HGCMMSG Msg;
    BYTE *apb[4];
    UINT32 idContext;
    UINT32 fFlags;
    char *pszPath;
    int rc = VERR_NO_MEMORY;

    if (!vboxGstCtrlFileFetch(pSession, idClient, HOST_MSG_DIR_CREATE, cParms, "ubuu", VBOXGSTCTRL_DEF_BUF_SIZE,
                              &Msg, apb))
        return;
    idContext = VbglR3HgcmMsgGetU32(&Msg, 0);
    fFlags    = VbglR3HgcmMsgGetU32(&Msg, 3);
    pszPath   = vboxGstCtrlFileGetPath(&Msg, 1, apb[1]);
    VbglR3HgcmMsgFree(&Msg);

    if (pszPath)
        rc = vboxGstCtrlDirCreatePath(pszPath, (fFlags & GSTCTL_CREATEDIRECTORY_F_PARENTS) != 0);
    vboxGstCtrlFileReply(pSession, idClient, idContext, rc, NULL);
    free(pszPath);
}

/**
 * Removes what a directory contains, and the directory itself if asked to.
 *
 * @returns VBox status code.
 */
static int vboxGstCtrlDirRemoveTree(const char *pszPath, BOOL fDirToo)
{
    WIN32_FIND_DATAA Data;
    char szPath[MAX_PATH + 4];
    HANDLE hFind;
    int rc = VINF_SUCCESS;

    if (strlen(pszPath) + 4 >= sizeof(szPath))
        return VERR_FILENAME_TOO_LONG;
    wsprintf(szPath, "%s\\*.*", pszPath);
    hFind = FindFirstFileA(szPath, &Data);
    if (hFind != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (!strcmp(Data.cFileName, ".") || !strcmp(Data.cFileName, ".."))
                continue;
            if (strlen(pszPath) + strlen(Data.cFileName) + 2 > MAX_PATH)
            {
                rc = VERR_FILENAME_TOO_LONG;
                break;
            }
            wsprintf(szPath, "%s\\%s", pszPath, Data.cFileName);
            if (Data.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
                SetFileAttributesA(szPath, Data.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY);
            if (Data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                rc = vboxGstCtrlDirRemoveTree(szPath, TRUE);
            else if (!DeleteFileA(szPath))
                rc = vboxGstCtrlFileRc(GetLastError());
        } while (RT_SUCCESS(rc) && FindNextFileA(hFind, &Data));
        FindClose(hFind);
    }
    if (RT_SUCCESS(rc) && fDirToo && !RemoveDirectoryA(pszPath))
        rc = vboxGstCtrlFileRc(GetLastError());
    return rc;
}

/**
 * Handles HOST_MSG_DIR_REMOVE.
 */
static void vboxGstCtrlDirRemove(VBOXGSTCTRLSESSION *pSession, UINT32 idClient, UINT32 cParms)
{
    VBGLR3HGCMMSG Msg;
    BYTE *apb[3];
    UINT32 idContext;
    UINT32 fFlags;
    char *pszPath;
    DWORD dwAttr;
    int rc = VERR_NO_MEMORY;

    if (!vboxGstCtrlFileFetch(pSession, idClient, HOST_MSG_DIR_REMOVE, cParms, "ubu", VBOXGSTCTRL_DEF_BUF_SIZE,
                              &Msg, apb))
        return;
    idContext = VbglR3HgcmMsgGetU32(&Msg, 0);
    fFlags    = VbglR3HgcmMsgGetU32(&Msg, 2);
    pszPath   = vboxGstCtrlFileGetPath(&Msg, 1, apb[1]);
    VbglR3HgcmMsgFree(&Msg);

    if (pszPath)
    {
        dwAttr = GetFileAttributesA(pszPath);
        if (dwAttr == 0xffffffff)
            rc = vboxGstCtrlFileRc(GetLastError());
        else if (!(dwAttr & FILE_ATTRIBUTE_DIRECTORY))
            rc = VERR_NOT_A_DIRECTORY;
        else if (fFlags & DIRREMOVEREC_FLAG_RECURSIVE)
            rc = vboxGstCtrlDirRemoveTree(pszPath, !(fFlags & DIRREMOVEREC_FLAG_CONTENT_ONLY));
        else if (RemoveDirectoryA(pszPath))
            rc = VINF_SUCCESS;
        else
        {
            /* Windows 9x says access denied for a directory that isn't empty. */
            WIN32_FIND_DATAA Data;
            char szPattern[MAX_PATH + 4];
            HANDLE hFind;
            rc = vboxGstCtrlFileRc(GetLastError());
            if (rc == VERR_ACCESS_DENIED && strlen(pszPath) + 4 < sizeof(szPattern))
            {
                wsprintf(szPattern, "%s\\*.*", pszPath);
                hFind = FindFirstFileA(szPattern, &Data);
                if (hFind != INVALID_HANDLE_VALUE)
                {
                    do
                        if (strcmp(Data.cFileName, ".") && strcmp(Data.cFileName, ".."))
                        {
                            rc = VERR_DIR_NOT_EMPTY;
                            break;
                        }
                    while (FindNextFileA(hFind, &Data));
                    FindClose(hFind);
                }
            }
        }
    }
    vboxGstCtrlFileReply(pSession, idClient, idContext, rc, NULL);
    free(pszPath);
}


/*********************************************************************************************************************************
*   Paths and file systems                                                                                                       *
*********************************************************************************************************************************/

/**
 * Handles HOST_MSG_PATH_RENAME.
 */
static void vboxGstCtrlPathRename(VBOXGSTCTRLSESSION *pSession, UINT32 idClient, UINT32 cParms)
{
    VBGLR3HGCMMSG Msg;
    BYTE *apb[4];
    UINT32 idContext;
    UINT32 fFlags;
    char *pszSrc;
    char *pszDst;
    DWORD dwAttr;
    int rc = VERR_NO_MEMORY;

    if (!vboxGstCtrlFileFetch(pSession, idClient, HOST_MSG_PATH_RENAME, cParms, "ubbu", VBOXGSTCTRL_DEF_BUF_SIZE,
                              &Msg, apb))
        return;
    idContext = VbglR3HgcmMsgGetU32(&Msg, 0);
    fFlags    = VbglR3HgcmMsgGetU32(&Msg, 3);
    pszSrc    = vboxGstCtrlFileGetPath(&Msg, 1, apb[1]);
    pszDst    = vboxGstCtrlFileGetPath(&Msg, 2, apb[2]);
    VbglR3HgcmMsgFree(&Msg);

    if (pszSrc && pszDst)
    {
        /* Windows 9x has no MoveFileEx: a file in the way is removed first. */
        dwAttr = GetFileAttributesA(pszDst);
        if (dwAttr != 0xffffffff && lstrcmpi(pszSrc, pszDst) != 0)
        {
            if (!(fFlags & PATHRENAME_FLAG_REPLACE))
                rc = VERR_ALREADY_EXISTS;
            else if (dwAttr & FILE_ATTRIBUTE_DIRECTORY)
                rc = VERR_IS_A_DIRECTORY;
            else
                rc = DeleteFileA(pszDst) ? VINF_SUCCESS : vboxGstCtrlFileRc(GetLastError());
        }
        else
            rc = VINF_SUCCESS;
        if (RT_SUCCESS(rc) && !MoveFileA(pszSrc, pszDst))
            rc = vboxGstCtrlFileRc(GetLastError());
    }
    vboxGstCtrlFileReply(pSession, idClient, idContext, rc, NULL);
    free(pszSrc);
    free(pszDst);
}

/**
 * Handles HOST_MSG_PATH_USER_DOCUMENTS and HOST_MSG_PATH_USER_HOME.
 */
static void vboxGstCtrlPathUser(VBOXGSTCTRLSESSION *pSession, UINT32 idClient, UINT32 idMsg, UINT32 cParms)
{
    VBGLR3HGCMMSG Msg;
    char szPath[MAX_PATH];
    char *pszUtf8 = NULL;
    UINT32 idContext;
    DWORD cb = sizeof(szPath);
    HKEY hKey;
    int rc = VERR_PATH_NOT_FOUND;

    if (!vboxGstCtrlFileFetch(pSession, idClient, idMsg, cParms, "u", 0, &Msg, NULL))
        return;
    idContext = VbglR3HgcmMsgGetU32(&Msg, 0);
    VbglR3HgcmMsgFree(&Msg);

    szPath[0] = '\0';
    if (idMsg == HOST_MSG_PATH_USER_DOCUMENTS)
    {
        if (RegOpenKeyEx(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders", 0,
                         KEY_READ, &hKey) == ERROR_SUCCESS)
        {
            if (RegQueryValueEx(hKey, "Personal", NULL, NULL, (LPBYTE)szPath, &cb) != ERROR_SUCCESS)
                szPath[0] = '\0';
            RegCloseKey(hKey);
        }
    }
    else if (!GetEnvironmentVariable("USERPROFILE", szPath, sizeof(szPath)) || !szPath[0])
        GetWindowsDirectory(szPath, sizeof(szPath)); /* No profiles: the Windows directory is home. */
    szPath[sizeof(szPath) - 1] = '\0';
    if (szPath[0])
    {
        pszUtf8 = VBoxTrayAnsiToUtf8(szPath);
        rc = pszUtf8 ? VINF_SUCCESS : VERR_NO_MEMORY;
    }
    vboxGstCtrlFileReply(pSession, idClient, idContext, rc, pszUtf8);
    free(pszUtf8);
}

/**
 * Handles HOST_MSG_FS_OBJ_QUERY_INFO.
 */
static void vboxGstCtrlFsObjQueryInfo(VBOXGSTCTRLSESSION *pSession, UINT32 idClient, UINT32 cParms)
{
    VBGLR3HGCMMSG Msg;
    GSTCTLFSOBJINFO Info;
    BYTE *apb[4];
    UINT32 idContext;
    UINT32 enmAttrAdd;
    char *pszPath;
    int rc = VERR_NO_MEMORY;

    if (!vboxGstCtrlFileFetch(pSession, idClient, HOST_MSG_FS_OBJ_QUERY_INFO, cParms, "ubuu", VBOXGSTCTRL_DEF_BUF_SIZE,
                              &Msg, apb))
        return;
    idContext  = VbglR3HgcmMsgGetU32(&Msg, 0);
    enmAttrAdd = VbglR3HgcmMsgGetU32(&Msg, 2);
    pszPath    = vboxGstCtrlFileGetPath(&Msg, 1, apb[1]);
    VbglR3HgcmMsgFree(&Msg);

    memset(&Info, 0, sizeof(Info));
    if (pszPath)
        rc = vboxGstCtrlFileStat(pszPath, enmAttrAdd, &Info);

    if (vboxGstCtrlFileReplyInit(&Msg, idClient, GUEST_MSG_FS_NOTIFY, idContext, GUEST_FS_NOTIFYTYPE_QUERY_OBJ_INFO, rc,
                                 3, sizeof(Info) + 2))
    {
        VbglR3HgcmMsgSetBuf(&Msg, 3, VBOXGUEST98_HGCM_PARM_BUFFER_IN, sizeof(Info), &Info);
        VbglR3HgcmMsgSetBuf(&Msg, 4, VBOXGUEST98_HGCM_PARM_BUFFER_IN, 1, "");
        VbglR3HgcmMsgSetBuf(&Msg, 5, VBOXGUEST98_HGCM_PARM_BUFFER_IN, 1, "");
        vboxGstCtrlFileReplySend(pSession, &Msg);
    }
    free(pszPath);
}

/**
 * Handles HOST_MSG_FS_CREATE_TEMP: replaces the trailing X's of the
 * template with random characters until the name is new.
 */
static void vboxGstCtrlFsCreateTemp(VBOXGSTCTRLSESSION *pSession, UINT32 idClient, UINT32 cParms)
{
    static const char s_szChars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    VBGLR3HGCMMSG Msg;
    BYTE *apb[5];
    UINT32 idContext;
    UINT32 fFlags;
    char *pszTemplate;
    char *pszDir;
    char *pszUtf8 = NULL;
    char szPath[MAX_PATH];
    size_t cchDir;
    size_t cX = 0;
    size_t off;
    unsigned iTry;
    int rc = VERR_NO_MEMORY;

    if (!vboxGstCtrlFileFetch(pSession, idClient, HOST_MSG_FS_CREATE_TEMP, cParms, "ubbuu", VBOXGSTCTRL_DEF_BUF_SIZE,
                              &Msg, apb))
        return;
    idContext   = VbglR3HgcmMsgGetU32(&Msg, 0);
    fFlags      = VbglR3HgcmMsgGetU32(&Msg, 3);
    pszTemplate = VBoxGstCtrlGetStr(&Msg, 1, apb[1], '_');
    pszDir      = vboxGstCtrlFileGetPath(&Msg, 2, apb[2]);
    VbglR3HgcmMsgFree(&Msg);

    if (pszTemplate && pszDir)
    {
        /* The template, in the given directory or the temporary one. */
        szPath[0] = '\0';
        if (pszDir[0])
            lstrcpyn(szPath, pszDir, sizeof(szPath));
        else
            GetTempPath(sizeof(szPath), szPath);
        cchDir = strlen(szPath);
        if (cchDir && szPath[cchDir - 1] != '\\')
            szPath[cchDir++] = '\\';
        szPath[cchDir] = '\0';
        if (cchDir + strlen(pszTemplate) >= sizeof(szPath))
            rc = VERR_FILENAME_TOO_LONG;
        else
        {
            lstrcpy(&szPath[cchDir], pszTemplate);
            off = strlen(szPath);
            while (off > cchDir && szPath[off - 1] == 'X')
            {
                off--;
                cX++;
            }
            rc = cX >= 3 ? VERR_ALREADY_EXISTS : VERR_INVALID_PARAMETER; /* RTDirCreateTemp wants at least 3. */
        }

        srand(GetTickCount());
        for (iTry = 0; iTry < 100 && rc == VERR_ALREADY_EXISTS; iTry++)
        {
            size_t i;
            HANDLE hFile;
            for (i = 0; i < cX; i++)
                szPath[off + i] = s_szChars[rand() % (sizeof(s_szChars) - 1)];
            if (fFlags & GSTCTL_CREATETEMP_F_DIRECTORY)
                rc = CreateDirectoryA(szPath, NULL) ? VINF_SUCCESS
                   : GetFileAttributesA(szPath) != 0xffffffff ? VERR_ALREADY_EXISTS : vboxGstCtrlFileRc(GetLastError());
            else
            {
                hFile = CreateFileA(szPath, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
                if (hFile != INVALID_HANDLE_VALUE)
                {
                    CloseHandle(hFile);
                    rc = VINF_SUCCESS;
                }
                else
                    rc = GetFileAttributesA(szPath) != 0xffffffff ? VERR_ALREADY_EXISTS : vboxGstCtrlFileRc(GetLastError());
            }
        }
        if (RT_SUCCESS(rc))
        {
            pszUtf8 = VBoxTrayAnsiToUtf8(szPath);
            if (!pszUtf8)
                rc = VERR_NO_MEMORY;
        }
    }

    if (vboxGstCtrlFileReplyInit(&Msg, idClient, GUEST_MSG_FS_NOTIFY, idContext, GUEST_FS_NOTIFYTYPE_CREATE_TEMP, rc, 1,
                                 pszUtf8 ? (DWORD)strlen(pszUtf8) + 1 : 1))
    {
        VbglR3HgcmMsgSetBuf(&Msg, 3, VBOXGUEST98_HGCM_PARM_BUFFER_IN, pszUtf8 ? (DWORD)strlen(pszUtf8) + 1 : 1,
                            pszUtf8 ? pszUtf8 : "");
        vboxGstCtrlFileReplySend(pSession, &Msg);
    }
    free(pszTemplate);
    free(pszDir);
    free(pszUtf8);
}

/**
 * Handles HOST_MSG_FS_QUERY_INFO: the file system of a drive.
 */
static void vboxGstCtrlFsQueryInfo(VBOXGSTCTRLSESSION *pSession, UINT32 idClient, UINT32 cParms)
{
    VBGLR3HGCMMSG Msg;
    GSTCTLFSINFO *pInfo;
    BYTE *apb[2];
    UINT32 idContext;
    char *pszPath;
    char szRoot[4];
    char szLabel[MAX_PATH];
    char szFsName[MAX_PATH];
    char *pszLabel;
    DWORD dwSerial = 0;
    DWORD cchMaxComponent = 0;
    DWORD fFsFlags = 0;
    DWORD cSectorsPerCluster = 0;
    DWORD cbSector = 0;
    DWORD cFreeClusters = 0;
    DWORD cClusters = 0;
    ULARGE_INTEGER cbAvail;
    ULARGE_INTEGER cbTotal;
    ULARGE_INTEGER cbFree;
    DWORD cbInfo;
    int rc = VERR_NO_MEMORY;

    if (!vboxGstCtrlFileFetch(pSession, idClient, HOST_MSG_FS_QUERY_INFO, cParms, "ub", VBOXGSTCTRL_DEF_BUF_SIZE,
                              &Msg, apb))
        return;
    idContext = VbglR3HgcmMsgGetU32(&Msg, 0);
    pszPath   = vboxGstCtrlFileGetPath(&Msg, 1, apb[1]);
    VbglR3HgcmMsgFree(&Msg);

    /* The drive of the path (no UNC names). */
    cbInfo = sizeof(GSTCTLFSINFO); /* Exactly: the host (guestCtrlHgcmGetBufExact) takes nothing else. */
    pInfo = (GSTCTLFSINFO *)calloc(1, cbInfo);
    if (pszPath && pInfo)
    {
        if (pszPath[0] && pszPath[1] == ':')
        {
            szRoot[0] = pszPath[0];
            szRoot[1] = ':';
            szRoot[2] = '\\';
            szRoot[3] = '\0';
            rc = VINF_SUCCESS;
        }
        else
            rc = VERR_NOT_SUPPORTED;
    }
    if (RT_SUCCESS(rc))
    {
        if (!GetVolumeInformationA(szRoot, szLabel, sizeof(szLabel), &dwSerial, &cchMaxComponent, &fFsFlags,
                                   szFsName, sizeof(szFsName)))
            rc = vboxGstCtrlFileRc(GetLastError());
        else
        {
            if (GetDiskFreeSpaceA(szRoot, &cSectorsPerCluster, &cbSector, &cFreeClusters, &cClusters))
            {
                pInfo->cbSectorSize = cbSector;
                pInfo->cbBlockSize  = cSectorsPerCluster * cbSector;
                pInfo->cbFree       = (UINT64)cFreeClusters * cSectorsPerCluster * cbSector;
                pInfo->cbTotalSize  = (UINT64)cClusters * cSectorsPerCluster * cbSector;
            }
            /* GetDiskFreeSpaceA caps at 2 GB; the Ex version (OSR2 and later) doesn't. */
            if (GetDiskFreeSpaceExA(szRoot, &cbAvail, &cbTotal, &cbFree))
            {
                pInfo->cbFree      = cbAvail.QuadPart;
                pInfo->cbTotalSize = cbTotal.QuadPart;
            }
            pInfo->uSerialNumber = dwSerial;
            pInfo->cMaxComponent = cchMaxComponent;
            pInfo->fFlags        = (fFsFlags & FILE_CASE_SENSITIVE_SEARCH) ? 2 /* GSTCTLFSINFO_F_IS_CASE_SENSITIVE */ : 0;
            if (GetDriveTypeA(szRoot) == DRIVE_REMOTE)
                pInfo->fFlags |= 1; /* GSTCTLFSINFO_F_IS_REMOTE */
            lstrcpyn(pInfo->szName, szFsName, sizeof(pInfo->szName));
            /* The host checks the label is UTF-8.  There is no room for the
               mount point (upstream leaves it out too). */
            pszLabel = VBoxTrayAnsiToUtf8(szLabel);
            if (pszLabel)
            {
                size_t cch = strlen(pszLabel);
                while (cch >= sizeof(pInfo->szLabel)) /* Don't cut a UTF-8 sequence. */
                    while (cch && (pszLabel[--cch] & 0xc0) == 0x80)
                        ;
                memcpy(pInfo->szLabel, pszLabel, cch);
                pInfo->szLabel[cch] = '\0';
                free(pszLabel);
            }
            pInfo->cbMountpoint = 0;
        }
    }

    if (vboxGstCtrlFileReplyInit(&Msg, idClient, GUEST_MSG_FS_NOTIFY, idContext, GUEST_FS_NOTIFYTYPE_QUERY_INFO, rc, 1,
                                 pInfo ? cbInfo : 0))
    {
        /* Full size even on failure: the host checks the size before the status. */
        VbglR3HgcmMsgSetBuf(&Msg, 3, VBOXGUEST98_HGCM_PARM_BUFFER_IN, pInfo ? cbInfo : 0, pInfo);
        vboxGstCtrlFileReplySend(pSession, &Msg);
    }
    free(pInfo);
    free(pszPath);
}


/**
 * Handles HOST_MSG_MOUNT_POINTS: the drive roots, each zero terminated,
 * like upstream's RTFsMountpointsEnum.
 */
static void vboxGstCtrlMountPoints(VBOXGSTCTRLSESSION *pSession, UINT32 idClient, UINT32 cParms)
{
    VBGLR3HGCMMSG Msg;
    char szDrives[26 * 4 + 1];
    UINT32 idContext;
    DWORD cch;
    int rc = VINF_SUCCESS;

    if (!vboxGstCtrlFileFetch(pSession, idClient, HOST_MSG_MOUNT_POINTS, cParms, "uu", 0, &Msg, NULL))
        return;
    idContext = VbglR3HgcmMsgGetU32(&Msg, 0);
    VbglR3HgcmMsgFree(&Msg);

    /* "A:\<NUL>C:\<NUL><NUL>": the last terminator isn't sent. */
    cch = GetLogicalDriveStringsA(sizeof(szDrives) - 1, szDrives);
    if (!cch || cch >= sizeof(szDrives))
    {
        rc = cch ? VERR_BUFFER_OVERFLOW : vboxGstCtrlFileRc(GetLastError());
        cch = 0;
    }

    if (vboxGstCtrlFileReplyInit(&Msg, idClient, GUEST_MSG_REPLY, idContext, 0, rc, 1, cch))
    {
        VbglR3HgcmMsgSetBuf(&Msg, 3, VBOXGUEST98_HGCM_PARM_BUFFER_IN, cch, szDrives);
        vboxGstCtrlFileReplySend(pSession, &Msg);
    }
}


/*********************************************************************************************************************************
*   Entry points                                                                                                                 *
*********************************************************************************************************************************/

/**
 * Handles a file, directory or file system message of a session.
 *
 * @returns TRUE if handled, FALSE if it isn't one of ours.
 */
BOOL VBoxGstCtrlFileHandleMsg(VBOXGSTCTRLSESSION *pSession, UINT32 idClient, UINT32 idMsg, UINT32 cParms)
{
    switch (idMsg)
    {
        case HOST_MSG_FILE_OPEN:            vboxGstCtrlFileOpen(pSession, idClient, cParms); return TRUE;
        case HOST_MSG_FILE_CLOSE:           vboxGstCtrlFileClose(pSession, idClient, cParms); return TRUE;
        case HOST_MSG_FILE_READ:
        case HOST_MSG_FILE_READ_AT:         vboxGstCtrlFileRead(pSession, idClient, idMsg, cParms); return TRUE;
        case HOST_MSG_FILE_WRITE:
        case HOST_MSG_FILE_WRITE_AT:        vboxGstCtrlFileWrite(pSession, idClient, idMsg, cParms); return TRUE;
        case HOST_MSG_FILE_SEEK:
        case HOST_MSG_FILE_TELL:
        case HOST_MSG_FILE_SET_SIZE:        vboxGstCtrlFilePosition(pSession, idClient, idMsg, cParms); return TRUE;
        case HOST_MSG_FILE_REMOVE:          vboxGstCtrlFileRemove(pSession, idClient, cParms); return TRUE;
        case HOST_MSG_DIR_OPEN:             vboxGstCtrlDirOpen(pSession, idClient, cParms); return TRUE;
        case HOST_MSG_DIR_CLOSE:
        case HOST_MSG_DIR_REWIND:           vboxGstCtrlDirCloseRewind(pSession, idClient, idMsg, cParms); return TRUE;
        case HOST_MSG_DIR_READ:             vboxGstCtrlDirRead(pSession, idClient, cParms); return TRUE;
        case HOST_MSG_DIR_LIST:             vboxGstCtrlDirList(pSession, idClient, cParms); return TRUE;
        case HOST_MSG_DIR_CREATE:           vboxGstCtrlDirCreate(pSession, idClient, cParms); return TRUE;
        case HOST_MSG_DIR_REMOVE:           vboxGstCtrlDirRemove(pSession, idClient, cParms); return TRUE;
        case HOST_MSG_PATH_RENAME:          vboxGstCtrlPathRename(pSession, idClient, cParms); return TRUE;
        case HOST_MSG_PATH_USER_DOCUMENTS:
        case HOST_MSG_PATH_USER_HOME:       vboxGstCtrlPathUser(pSession, idClient, idMsg, cParms); return TRUE;
        case HOST_MSG_FS_OBJ_QUERY_INFO:    vboxGstCtrlFsObjQueryInfo(pSession, idClient, cParms); return TRUE;
        case HOST_MSG_FS_CREATE_TEMP:       vboxGstCtrlFsCreateTemp(pSession, idClient, cParms); return TRUE;
        case HOST_MSG_FS_QUERY_INFO:        vboxGstCtrlFsQueryInfo(pSession, idClient, cParms); return TRUE;
        case HOST_MSG_MOUNT_POINTS:         vboxGstCtrlMountPoints(pSession, idClient, cParms); return TRUE;
        default:                            return FALSE;
    }
}

/**
 * Closes the files and directories a closing session left open.
 */
void VBoxGstCtrlFileCleanup(VBOXGSTCTRLSESSION *pSession)
{
    while (pSession->pFiles)
    {
        VBOXGSTCTRLFILE *pFile = pSession->pFiles;
        pSession->pFiles = pFile->pNext;
        CloseHandle(pFile->hFile);
        free(pFile);
    }
    while (pSession->pDirs)
    {
        VBOXGSTCTRLDIR *pDir = pSession->pDirs;
        pSession->pDirs = pDir->pNext;
        if (pDir->hFind != INVALID_HANDLE_VALUE)
            FindClose(pDir->hFind);
        free(pDir);
    }
}
