/** @file
 * VBoxTray - Shared clipboard file transfers, guest to host: answering the
 * host's requests for the files copied in the guest (worker thread).  See
 * ClipFile.c for the protocol.
 */

#include "ClipFile.h"
#include <stdlib.h>
#include <string.h>


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/

/** Room for a path the host sends (SHCL_TRANSFER_PATH_MAX, IPRT's
 *  RTPATH_MAX): the host sends its whole buffer. */
#define VBOXCLIPFILE_MAX_PATH       (4096 + 4)
/** Directories and files the host may have open at once (guest to host). */
#define VBOXCLIPFILE_MAX_OPEN       16


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/

/** A host request of a transfer of guest files (VBoxClipFileOnHostMsg). */
typedef struct VBOXCLIPFILEREQ
{
    /** The context ID, for the answer. */
    UINT64               idContext;
    /** The list or object handle, or the root index. */
    UINT64               u64;
    /** Bytes to read (OBJ_READ). */
    UINT32               u32;
    /** The path (LIST_OPEN, OBJ_OPEN), UTF-8. */
    char                 szPath[VBOXCLIPFILE_MAX_PATH];
} VBOXCLIPFILEREQ;

/** A guest directory or file the host has open. */
typedef struct VBOXCLIPFILEOPEN
{
    /** The handle given to the host, 0 if the slot is free. */
    UINT64               uHandle;
    BOOL                 fList;
    /** The file, or the directory search (INVALID_HANDLE_VALUE once it ended). */
    HANDLE               h;
    /** List: the search pattern ("dir\*.*") and the next entry. */
    char                 szPattern[MAX_PATH];
    WIN32_FIND_DATA      Next;
} VBOXCLIPFILEOPEN;

/** The transfer of guest files to the host (worker thread only). */
typedef struct VBOXCLIPFILESEND
{
    /** The context ID of the transfer, 0 if there is none. */
    UINT64               idContext;
    /** The directory the roots are in, with a trailing backslash. */
    char                 szDir[MAX_PATH];
    /** The paths of the roots (the files copied, all in szDir), each zero
     *  terminated, then an empty one. */
    char                *pszRoots;
    UINT32               cRoots;
    /** The last handle given to the host. */
    UINT64               uLastHandle;
    VBOXCLIPFILEOPEN     aOpen[VBOXCLIPFILE_MAX_OPEN];
    /** For the log. */
    UINT32               cFilesSent;
    UINT64               cbSent;
} VBOXCLIPFILESEND;


/*********************************************************************************************************************************
*   Global Variables                                                                                                             *
*********************************************************************************************************************************/

/** The transfer of guest files to the host. */
static VBOXCLIPFILESEND g_ClipFileSend;


/*********************************************************************************************************************************
*   Guest to host                                                                                                                *
*********************************************************************************************************************************/

/**
 * Fetches a host request (MSG_GET).  Parameter 0 is the context; the others
 * are 32-bit values ('d'), 64-bit values ('q') or buffers ('b'), as the
 * request has them.  The first 64-bit and 32-bit values after the context
 * are returned (the handle or root index, the size to read), and the last
 * buffer (the path).
 */
static int vboxClipFileFetchReq(HANDLE hDevice, UINT32 idClient, UINT32 uMsg, VBOXCLIPFILEREQ *pReq)
{
    const char *pszParms;
    VBGLR3HGCMMSG Msg;
    BYTE *pbPath = NULL;
    UINT32 iPath = 0;
    UINT32 iU64 = 0;
    UINT32 iU32 = 0;
    UINT32 i;
    DWORD cch;
    int rc;

    switch (uMsg)
    {
        case VBOX_SHCL_HOST_MSG_TRANSFER_ROOT_LIST_HDR_READ:   pszParms = "qd";    break; /* Flags. */
        case VBOX_SHCL_HOST_MSG_TRANSFER_ROOT_LIST_ENTRY_READ: pszParms = "qdq";   break; /* Info flags, index. */
        case VBOX_SHCL_HOST_MSG_TRANSFER_LIST_OPEN:            pszParms = "qdbbq"; break; /* Flags, filter, path, handle. */
        case VBOX_SHCL_HOST_MSG_TRANSFER_LIST_HDR_READ:
        case VBOX_SHCL_HOST_MSG_TRANSFER_LIST_ENTRY_READ:      pszParms = "qqd";   break; /* Handle, flags. */
        case VBOX_SHCL_HOST_MSG_TRANSFER_OBJ_OPEN:             pszParms = "qqbd";  break; /* Handle, path, create flags. */
        case VBOX_SHCL_HOST_MSG_TRANSFER_OBJ_READ:             pszParms = "qqdd";  break; /* Handle, size, flags. */
        default: /* LIST_CLOSE, OBJ_CLOSE */                   pszParms = "qq";    break; /* Handle. */
    }

    if (!VbglR3HgcmMsgInit(&Msg, idClient, VBOX_SHCL_GUEST_FN_MSG_GET, (UINT32)strlen(pszParms),
                           VBOXCLIPFILE_CALL_TIMEOUT_MS, 2 * VBOXCLIPFILE_MAX_PATH))
        return VERR_NO_MEMORY;
    VbglR3HgcmMsgSetU64(&Msg, 0, uMsg);
    for (i = 1; pszParms[i]; i++)
    {
        if (pszParms[i] == 'd')
        {
            VbglR3HgcmMsgSetU32(&Msg, i, 0);
            if (!iU32)
                iU32 = i;
        }
        else if (pszParms[i] == 'q')
        {
            VbglR3HgcmMsgSetU64(&Msg, i, 0);
            if (!iU64)
                iU64 = i;
        }
        else
        {
            pbPath = VbglR3HgcmMsgSetBuf(&Msg, i, VBOXGUEST98_HGCM_PARM_BUFFER_OUT, VBOXCLIPFILE_MAX_PATH, NULL);
            iPath  = i;
        }
    }
    rc = VBoxClipFileCall(hDevice, &Msg);
    pReq->idContext = VbglR3HgcmMsgGetU64(&Msg, 0);
    pReq->u64       = iU64 ? VbglR3HgcmMsgGetU64(&Msg, iU64) : 0;
    pReq->u32       = iU32 ? VbglR3HgcmMsgGetU32(&Msg, iU32) : 0;
    pReq->szPath[0] = '\0';
    if (RT_SUCCESS(rc) && pbPath)
    {
        cch = VBoxTrayStrNLen(pbPath, VbglR3HgcmMsgGetBufSize(&Msg, iPath));
        if (cch >= VBOXCLIPFILE_MAX_PATH)
            cch = VBOXCLIPFILE_MAX_PATH - 1;
        memcpy(pReq->szPath, pbPath, cch);
        pReq->szPath[cch] = '\0';
    }
    VbglR3HgcmMsgFree(&Msg);
    return rc;
}

/**
 * Answers a list or object open or close (VBOX_SHCL_GUEST_FN_REPLY).
 *
 * @param   uType       VBOX_SHCL_TX_REPLYMSGTYPE_XXX.
 */
static int vboxClipFileReply(HANDLE hDevice, UINT32 idClient, UINT64 idContext, UINT32 uType, int rcReply,
                             UINT64 uHandle)
{
    VBGLR3HGCMMSG Msg;
    int rc;

    if (!VbglR3HgcmMsgInit(&Msg, idClient, VBOX_SHCL_GUEST_FN_REPLY, 5, VBOXCLIPFILE_CALL_TIMEOUT_MS, 0))
        return VERR_NO_MEMORY;
    VbglR3HgcmMsgSetU64(&Msg, 0, idContext);
    VbglR3HgcmMsgSetU32(&Msg, 1, uType);
    VbglR3HgcmMsgSetU32(&Msg, 2, (UINT32)rcReply);
    VbglR3HgcmMsgSetBuf(&Msg, 3, VBOXGUEST98_HGCM_PARM_BUFFER_IN, 0, NULL);
    VbglR3HgcmMsgSetU64(&Msg, 4, uHandle);
    rc = VBoxClipFileCall(hDevice, &Msg);
    VbglR3HgcmMsgFree(&Msg);
    return rc;
}

/**
 * Sends the entry count of the root list (ROOT_LIST_HDR_WRITE) or of a
 * list, with its total size (LIST_HDR_WRITE).
 */
static int vboxClipFileWriteHdr(HANDLE hDevice, UINT32 idClient, UINT64 idContext, BOOL fRoot, UINT64 hList,
                                UINT64 cEntries, UINT64 cbTotal)
{
    VBGLR3HGCMMSG Msg;
    int rc;

    if (fRoot)
    {
        /* VBoxShClRootListHdrMsg: context, root flags, entries. */
        if (!VbglR3HgcmMsgInit(&Msg, idClient, VBOX_SHCL_GUEST_FN_ROOT_LIST_HDR_WRITE, 3,
                               VBOXCLIPFILE_CALL_TIMEOUT_MS, 0))
            return VERR_NO_MEMORY;
        VbglR3HgcmMsgSetU64(&Msg, 0, idContext);
        VbglR3HgcmMsgSetU32(&Msg, 1, 0);
        VbglR3HgcmMsgSetU64(&Msg, 2, cEntries);
    }
    else
    {
        /* VBoxShClListHdrMsg: context, handle, flags, features, objects, size. */
        if (!VbglR3HgcmMsgInit(&Msg, idClient, VBOX_SHCL_GUEST_FN_LIST_HDR_WRITE, 6,
                               VBOXCLIPFILE_CALL_TIMEOUT_MS, 0))
            return VERR_NO_MEMORY;
        VbglR3HgcmMsgSetU64(&Msg, 0, idContext);
        VbglR3HgcmMsgSetU64(&Msg, 1, hList);
        VbglR3HgcmMsgSetU32(&Msg, 2, 0);
        VbglR3HgcmMsgSetU32(&Msg, 3, 0);
        VbglR3HgcmMsgSetU64(&Msg, 4, cEntries);
        VbglR3HgcmMsgSetU64(&Msg, 5, cbTotal);
    }
    rc = VBoxClipFileCall(hDevice, &Msg);
    VbglR3HgcmMsgFree(&Msg);
    return rc;
}

/**
 * Converts a FILETIME to ns since 1970 (RTTIMESPEC), 0 if the file system
 * doesn't keep that time.
 */
static INT64 vboxClipFileTimeFromFileTime(const FILETIME *pFileTime)
{
    INT64 iTime = ((INT64)pFileTime->dwHighDateTime << 32) | pFileTime->dwLowDateTime;
    return iTime ? (iTime - VBOXCLIPFILE_FILETIME_1970) * 100 : 0;
}

/**
 * Sends an entry of the root list (ROOT_LIST_ENTRY_WRITE) or of a list
 * (LIST_ENTRY_WRITE), with its SHCLFSOBJINFO.
 *
 * @param   pszName     The name (ANSI).
 * @param   pData       Its directory entry.
 */
static int vboxClipFileWriteEntry(HANDLE hDevice, UINT32 idClient, UINT64 idContext, BOOL fRoot,
                                  UINT64 uIndexOrHandle, const char *pszName, const WIN32_FIND_DATA *pData)
{
    VBGLR3HGCMMSG Msg;
    BYTE abInfo[VBOXCLIPFILE_INFO_SIZE];
    BOOL fDir = (pData->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    UINT64 cbObject = fDir ? 0 : ((UINT64)pData->nFileSizeHigh << 32) | pData->nFileSizeLow;
    UINT32 fMode;
    char *pszUtf8;
    DWORD cbName;
    int rc;

    /* Like IPRT's: the type, UNIX permissions, then the FAT attributes. */
    fMode = fDir ? VBOXCLIPFILE_TYPE_DIRECTORY | 0777 : VBOXCLIPFILE_TYPE_FILE | 0666;
    if (pData->dwFileAttributes & FILE_ATTRIBUTE_READONLY)
        fMode &= ~0222U;
    fMode |= (pData->dwFileAttributes & VBOXCLIPFILE_DOS_MASK) << VBOXCLIPFILE_DOS_SHIFT;
    memset(abInfo, 0, sizeof(abInfo));
    *(UINT64 *)&abInfo[VBOXCLIPFILE_INFO_OFF_SIZE]  = cbObject;
    *(UINT64 *)&abInfo[VBOXCLIPFILE_INFO_OFF_ALLOC] = cbObject;
    *(INT64 *)&abInfo[VBOXCLIPFILE_INFO_OFF_ATIME]  = vboxClipFileTimeFromFileTime(&pData->ftLastAccessTime);
    *(INT64 *)&abInfo[VBOXCLIPFILE_INFO_OFF_MTIME]  = vboxClipFileTimeFromFileTime(&pData->ftLastWriteTime);
    *(INT64 *)&abInfo[VBOXCLIPFILE_INFO_OFF_CTIME]  = vboxClipFileTimeFromFileTime(&pData->ftLastWriteTime);
    *(INT64 *)&abInfo[VBOXCLIPFILE_INFO_OFF_BTIME]  = vboxClipFileTimeFromFileTime(&pData->ftCreationTime);
    *(UINT32 *)&abInfo[VBOXCLIPFILE_INFO_OFF_MODE]  = fMode;
    *(UINT32 *)&abInfo[VBOXCLIPFILE_INFO_OFF_ADD]   = VBOXCLIPFILE_ATTRADD_NOTHING;

    pszUtf8 = VBoxTrayAnsiToUtf8(pszName);
    if (!pszUtf8)
        return VERR_NO_MEMORY;
    cbName = (DWORD)strlen(pszUtf8) + 1;
    if (!VbglR3HgcmMsgInit(&Msg, idClient, fRoot ? VBOX_SHCL_GUEST_FN_ROOT_LIST_ENTRY_WRITE : VBOX_SHCL_GUEST_FN_LIST_ENTRY_WRITE,
                           6, VBOXCLIPFILE_CALL_TIMEOUT_MS, cbName + VBOXCLIPFILE_INFO_SIZE))
    {
        free(pszUtf8);
        return VERR_NO_MEMORY;
    }
    VbglR3HgcmMsgSetU64(&Msg, 0, idContext);
    if (fRoot)
    {
        /* VBoxShClRootListEntryParms: context, info flags, index. */
        VbglR3HgcmMsgSetU32(&Msg, 1, VBOX_SHCL_INFO_F_FSOBJINFO);
        VbglR3HgcmMsgSetU64(&Msg, 2, uIndexOrHandle);
    }
    else
    {
        /* VBoxShClListEntryReqParms: context, handle, info flags. */
        VbglR3HgcmMsgSetU64(&Msg, 1, uIndexOrHandle);
        VbglR3HgcmMsgSetU32(&Msg, 2, VBOX_SHCL_INFO_F_FSOBJINFO);
    }
    VbglR3HgcmMsgSetBuf(&Msg, 3, VBOXGUEST98_HGCM_PARM_BUFFER_IN, cbName, pszUtf8);
    VbglR3HgcmMsgSetU32(&Msg, 4, VBOXCLIPFILE_INFO_SIZE);
    VbglR3HgcmMsgSetBuf(&Msg, 5, VBOXGUEST98_HGCM_PARM_BUFFER_IN, VBOXCLIPFILE_INFO_SIZE, abInfo);
    rc = VBoxClipFileCall(hDevice, &Msg);
    VbglR3HgcmMsgFree(&Msg);
    free(pszUtf8);
    return rc;
}

/**
 * Sends a chunk of a guest file (OBJ_WRITE, the answer to OBJ_READ); empty
 * at the end.
 */
static int vboxClipFileWriteObj(HANDLE hDevice, UINT32 idClient, UINT64 idContext, UINT64 hObj, const void *pv,
                                DWORD cb)
{
    VBGLR3HGCMMSG Msg;
    int rc;

    /* VBoxShClObjReadWriteMsg: context, handle, size, data, checksum size, checksum. */
    if (!VbglR3HgcmMsgInit(&Msg, idClient, VBOX_SHCL_GUEST_FN_OBJ_WRITE, 6, VBOXCLIPFILE_CALL_TIMEOUT_MS, cb))
        return VERR_NO_MEMORY;
    VbglR3HgcmMsgSetU64(&Msg, 0, idContext);
    VbglR3HgcmMsgSetU64(&Msg, 1, hObj);
    VbglR3HgcmMsgSetU32(&Msg, 2, cb);
    VbglR3HgcmMsgSetBuf(&Msg, 3, VBOXGUEST98_HGCM_PARM_BUFFER_IN, cb, pv);
    VbglR3HgcmMsgSetU32(&Msg, 4, 0);
    VbglR3HgcmMsgSetBuf(&Msg, 5, VBOXGUEST98_HGCM_PARM_BUFFER_IN, 0, NULL);
    rc = VBoxClipFileCall(hDevice, &Msg);
    VbglR3HgcmMsgFree(&Msg);
    return rc;
}

/**
 * Closes a directory or file the host had open and frees its slot.
 */
static void vboxClipFileSendClose(VBOXCLIPFILEOPEN *pOpen)
{
    if (pOpen->h != INVALID_HANDLE_VALUE)
    {
        if (pOpen->fList)
            FindClose(pOpen->h);
        else
            CloseHandle(pOpen->h);
    }
    pOpen->h       = INVALID_HANDLE_VALUE;
    pOpen->uHandle = 0;
}

/**
 * Ends the transfer of guest files, if there is one.
 *
 * @param   pszHow      How it ended, for the log: the host's status, or why
 *                      the guest ends it (the host doesn't report the end of
 *                      every transfer, so the next paste ends the last one).
 * @param   rc          The status code with it, logged on failure.
 */
static void vboxClipFileSendEnd(const char *pszHow, int rc)
{
    unsigned i;

    if (!g_ClipFileSend.idContext)
        return;
    for (i = 0; i < VBOXCLIPFILE_MAX_OPEN; i++)
        if (g_ClipFileSend.aOpen[i].uHandle)
            vboxClipFileSendClose(&g_ClipFileSend.aOpen[i]);
    if (RT_SUCCESS(rc))
        VBoxTrayLogRel("Clipboard: file transfer to the host %s, %lu file(s), %lu KB sent\n", pszHow,
                       g_ClipFileSend.cFilesSent, (UINT32)(g_ClipFileSend.cbSent / 1024));
    else
        VBoxTrayLogRel("Clipboard: file transfer to the host %s, rc=%d, %lu file(s), %lu KB sent\n", pszHow, rc,
                       g_ClipFileSend.cFilesSent, (UINT32)(g_ClipFileSend.cbSent / 1024));
    free(g_ClipFileSend.pszRoots);
    g_ClipFileSend.pszRoots   = NULL;
    g_ClipFileSend.cRoots     = 0;
    g_ClipFileSend.idContext  = 0;
    g_ClipFileSend.cFilesSent = 0;
    g_ClipFileSend.cbSent     = 0;
}

/**
 * Starts a transfer of guest files: the files copied in the guest become
 * its roots.  Like upstream, the directory of the first one is the
 * transfer's root directory and the others must be in it.
 *
 * @param   idContext   The context ID of the transfer.
 */
static int vboxClipFileSendBegin(UINT64 idContext)
{
    char *pszFiles;
    char *pszSlash = NULL;
    char *psz;
    DWORD cchDir;
    UINT32 cRoots = 0;

    vboxClipFileSendEnd("over (the host pastes again)", VINF_SUCCESS);

    pszFiles = VBoxClipboardGetFiles();
    if (!pszFiles || !*pszFiles)
    {
        free(pszFiles);
        return VERR_NO_DATA;
    }
    for (psz = pszFiles; *psz; psz = CharNext(psz))
        if (*psz == '\\')
            pszSlash = psz;
    cchDir = pszSlash ? (DWORD)(pszSlash - pszFiles) + 1 : 0;
    if (!cchDir || cchDir >= MAX_PATH)
    {
        free(pszFiles);
        return VERR_PATH_NOT_FOUND;
    }
    for (psz = pszFiles; *psz; psz += strlen(psz) + 1)
    {
        if (   strlen(psz) <= cchDir
            || CompareString(LOCALE_SYSTEM_DEFAULT, NORM_IGNORECASE, psz, cchDir, pszFiles, cchDir) != CSTR_EQUAL)
        {
            VBoxTrayLogRel("Clipboard: \"%s\" isn't in the folder of the other files copied\n", psz);
            free(pszFiles);
            return VERR_PATH_NOT_FOUND;
        }
        cRoots++;
    }

    memcpy(g_ClipFileSend.szDir, pszFiles, cchDir);
    g_ClipFileSend.szDir[cchDir] = '\0';
    g_ClipFileSend.pszRoots  = pszFiles;
    g_ClipFileSend.cRoots    = cRoots;
    g_ClipFileSend.idContext = idContext;
    VBoxTrayLogRel("Clipboard: the host pastes %lu file(s) or folder(s) from %s\n", cRoots, g_ClipFileSend.szDir);
    return VINF_SUCCESS;
}

/**
 * Turns a path in the transfer (UTF-8, '/' or '\\' separated: the host joins
 * the names of directory entries with '\\') into the guest path.  Like
 * upstream's ShClTransferResolvePathAbs, it must be relative, without "."
 * or ".." and start with a root's name.
 *
 * @param   pszAbs      Where to return the guest path, MAX_PATH.
 */
static int vboxClipFileSendResolve(const char *pszPath, char *pszAbs)
{
    DWORD cchDir = (DWORD)strlen(g_ClipFileSend.szDir);
    const char *pszRoot;
    char *pszAnsi;
    char *pszComp;
    char *psz;
    DWORD cch;
    int rc = VERR_INVALID_NAME;

    /* Characters the code page doesn't have can't be in a guest name: '?'
       makes opening it fail. */
    pszAnsi = VBoxTrayUtf8ToAnsi(pszPath, (DWORD)strlen(pszPath), '?');
    if (!pszAnsi)
        return VERR_NO_MEMORY;
    for (psz = pszComp = pszAnsi; ; psz = CharNext(psz))
    {
        if (*psz == '/')
            *psz = '\\';
        if (*psz == '\\' || !*psz)
        {
            DWORD cchComp = (DWORD)(psz - pszComp);
            if (!cchComp || (pszComp[0] == '.' && (cchComp == 1 || (cchComp == 2 && pszComp[1] == '.'))))
                break;
            if (!*psz)
            {
                rc = VERR_PATH_NOT_FOUND;
                break;
            }
            pszComp = psz + 1;
        }
        else if (*psz == ':')
            break;
    }

    cch = (DWORD)strlen(pszAnsi);
    for (pszRoot = g_ClipFileSend.pszRoots; rc == VERR_PATH_NOT_FOUND && *pszRoot; pszRoot += strlen(pszRoot) + 1)
    {
        const char *pszName = pszRoot + cchDir;
        DWORD cchName = (DWORD)strlen(pszName);
        if (   cch >= cchName
            && (pszAnsi[cchName] == '\0' || pszAnsi[cchName] == '\\')
            && CompareString(LOCALE_SYSTEM_DEFAULT, NORM_IGNORECASE, pszAnsi, cchName, pszName, cchName) == CSTR_EQUAL)
            rc = cchDir + cch < MAX_PATH ? VINF_SUCCESS : VERR_FILENAME_TOO_LONG;
    }
    if (RT_SUCCESS(rc))
    {
        memcpy(pszAbs, g_ClipFileSend.szDir, cchDir);
        memcpy(&pszAbs[cchDir], pszAnsi, cch + 1);
    }
    else
        VBoxTrayLogRel("Clipboard: the host asked for \"%s\", rc=%d\n", pszPath, rc);
    free(pszAnsi);
    return rc;
}

/**
 * Takes a free slot for a directory or file the host opens.
 *
 * @returns The slot with its new handle, NULL if all are taken.
 */
static VBOXCLIPFILEOPEN *vboxClipFileSendAlloc(BOOL fList)
{
    unsigned i;
    for (i = 0; i < VBOXCLIPFILE_MAX_OPEN; i++)
    {
        VBOXCLIPFILEOPEN *pOpen = &g_ClipFileSend.aOpen[i];
        if (!pOpen->uHandle)
        {
            pOpen->uHandle = ++g_ClipFileSend.uLastHandle;
            pOpen->fList   = fList;
            pOpen->h       = INVALID_HANDLE_VALUE;
            return pOpen;
        }
    }
    return NULL;
}

/**
 * Finds the directory or file the host opened with a handle.
 */
static VBOXCLIPFILEOPEN *vboxClipFileSendFind(UINT64 uHandle, BOOL fList)
{
    unsigned i;
    for (i = 0; uHandle && i < VBOXCLIPFILE_MAX_OPEN; i++)
        if (g_ClipFileSend.aOpen[i].uHandle == uHandle && g_ClipFileSend.aOpen[i].fList == fList)
            return &g_ClipFileSend.aOpen[i];
    return NULL;
}

/**
 * Whether a directory entry is "." or "..".
 */
static BOOL vboxClipFileIsDots(const WIN32_FIND_DATA *pData)
{
    return    pData->cFileName[0] == '.'
           && (!pData->cFileName[1] || (pData->cFileName[1] == '.' && !pData->cFileName[2]));
}

/**
 * Moves a list to its next entry, skipping "." and "..".
 *
 * @param   fFirst      The current entry is the one FindFirstFile found.
 */
static void vboxClipFileListNext(VBOXCLIPFILEOPEN *pOpen, BOOL fFirst)
{
    while (pOpen->h != INVALID_HANDLE_VALUE)
    {
        if (!fFirst && !FindNextFile(pOpen->h, &pOpen->Next))
        {
            FindClose(pOpen->h);
            pOpen->h = INVALID_HANDLE_VALUE;
            break;
        }
        fFirst = FALSE;
        if (!vboxClipFileIsDots(&pOpen->Next))
            break;
    }
}

/**
 * Opens a guest directory for listing (LIST_OPEN); the reply carries the
 * outcome.
 */
static int vboxClipFileSendListOpen(HANDLE hDevice, UINT32 idClient, const VBOXCLIPFILEREQ *pReq)
{
    VBOXCLIPFILEOPEN *pOpen = NULL;
    char szAbs[MAX_PATH];
    DWORD dwAttr;
    DWORD dwErr;
    int rc;

    rc = vboxClipFileSendResolve(pReq->szPath, szAbs);
    if (RT_SUCCESS(rc))
    {
        dwAttr = GetFileAttributes(szAbs);
        if (dwAttr == 0xffffffff)
            rc = VBoxTrayWin32ToRc(GetLastError(), VERR_READ_ERROR);
        else if (!(dwAttr & FILE_ATTRIBUTE_DIRECTORY))
            rc = VERR_NOT_SUPPORTED; /* The host only lists directories. */
        else if (strlen(szAbs) + sizeof("\\*.*") > MAX_PATH)
            rc = VERR_FILENAME_TOO_LONG;
    }
    if (RT_SUCCESS(rc))
    {
        pOpen = vboxClipFileSendAlloc(TRUE);
        if (!pOpen)
            rc = VERR_TOO_MANY_OPEN_FILES;
    }
    if (RT_SUCCESS(rc))
    {
        lstrcpy(pOpen->szPattern, szAbs);
        lstrcat(pOpen->szPattern, "\\*.*");
        pOpen->h = FindFirstFile(pOpen->szPattern, &pOpen->Next);
        if (pOpen->h == INVALID_HANDLE_VALUE)
        {
            dwErr = GetLastError();
            if (dwErr != ERROR_FILE_NOT_FOUND && dwErr != ERROR_NO_MORE_FILES)
            {
                rc = VBoxTrayWin32ToRc(dwErr, VERR_READ_ERROR);
                vboxClipFileSendClose(pOpen);
                pOpen = NULL;
            }
        }
        else
            vboxClipFileListNext(pOpen, TRUE);
    }
    vboxClipFileReply(hDevice, idClient, pReq->idContext, VBOX_SHCL_TX_REPLYMSGTYPE_LIST_OPEN, rc,
                      pOpen ? pOpen->uHandle : 0);
    return rc;
}

/**
 * Sends the header of a list (LIST_HDR_READ): the number of entries and
 * the size of the files among them, like upstream's
 * shclTransferLocalListHdrFromDir.
 */
static int vboxClipFileSendListHdr(HANDLE hDevice, UINT32 idClient, const VBOXCLIPFILEREQ *pReq)
{
    VBOXCLIPFILEOPEN *pOpen = vboxClipFileSendFind(pReq->u64, TRUE);
    WIN32_FIND_DATA Data;
    UINT64 cEntries = 0;
    UINT64 cbTotal = 0;
    HANDLE hFind;

    if (!pOpen)
        return VERR_NOT_FOUND;
    hFind = FindFirstFile(pOpen->szPattern, &Data);
    if (hFind != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (vboxClipFileIsDots(&Data))
                continue;
            cEntries++;
            if (!(Data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                cbTotal += ((UINT64)Data.nFileSizeHigh << 32) | Data.nFileSizeLow;
        } while (FindNextFile(hFind, &Data));
        FindClose(hFind);
    }
    return vboxClipFileWriteHdr(hDevice, idClient, pReq->idContext, FALSE, pReq->u64, cEntries, cbTotal);
}

/**
 * Sends the next entry of a list (LIST_ENTRY_READ).
 */
static int vboxClipFileSendListEntry(HANDLE hDevice, UINT32 idClient, const VBOXCLIPFILEREQ *pReq)
{
    VBOXCLIPFILEOPEN *pOpen = vboxClipFileSendFind(pReq->u64, TRUE);
    int rc;

    if (!pOpen)
        return VERR_NOT_FOUND;
    if (pOpen->h == INVALID_HANDLE_VALUE)
        return VERR_NO_MORE_FILES;
    rc = vboxClipFileWriteEntry(hDevice, idClient, pReq->idContext, FALSE, pReq->u64, pOpen->Next.cFileName,
                                &pOpen->Next);
    vboxClipFileListNext(pOpen, FALSE);
    return rc;
}

/**
 * Sends an entry of the root list (ROOT_LIST_ENTRY_READ).
 */
static int vboxClipFileSendRoot(HANDLE hDevice, UINT32 idClient, const VBOXCLIPFILEREQ *pReq)
{
    WIN32_FIND_DATA Data;
    const char *pszRoot = g_ClipFileSend.pszRoots;
    HANDLE hFind;
    UINT64 i;

    if (pReq->u64 >= g_ClipFileSend.cRoots)
        return VERR_NOT_FOUND;
    for (i = 0; i < pReq->u64; i++)
        pszRoot += strlen(pszRoot) + 1;
    hFind = FindFirstFile(pszRoot, &Data);
    if (hFind == INVALID_HANDLE_VALUE)
        return VBoxTrayWin32ToRc(GetLastError(), VERR_READ_ERROR);
    FindClose(hFind);
    return vboxClipFileWriteEntry(hDevice, idClient, pReq->idContext, TRUE, pReq->u64,
                                  pszRoot + strlen(g_ClipFileSend.szDir), &Data);
}

/**
 * Opens a guest file for reading (OBJ_OPEN); the reply carries the outcome.
 */
static int vboxClipFileSendObjOpen(HANDLE hDevice, UINT32 idClient, const VBOXCLIPFILEREQ *pReq)
{
    VBOXCLIPFILEOPEN *pOpen = NULL;
    char szAbs[MAX_PATH];
    HANDLE hFile = INVALID_HANDLE_VALUE;
    int rc;

    rc = vboxClipFileSendResolve(pReq->szPath, szAbs);
    if (RT_SUCCESS(rc))
    {
        hFile = CreateFile(szAbs, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (hFile == INVALID_HANDLE_VALUE)
        {
            rc = VBoxTrayWin32ToRc(GetLastError(), VERR_READ_ERROR);
            VBoxTrayLogRel("Clipboard: opening \"%s\" for the host failed, rc=%d\n", szAbs, rc);
        }
    }
    if (RT_SUCCESS(rc))
    {
        pOpen = vboxClipFileSendAlloc(FALSE);
        if (pOpen)
            pOpen->h = hFile;
        else
        {
            CloseHandle(hFile);
            rc = VERR_TOO_MANY_OPEN_FILES;
        }
    }
    vboxClipFileReply(hDevice, idClient, pReq->idContext, VBOX_SHCL_TX_REPLYMSGTYPE_OBJ_OPEN, rc,
                      pOpen ? pOpen->uHandle : 0);
    return rc;
}

/**
 * Sends the next chunk of a guest file (OBJ_READ), at most the size asked
 * and the agreed chunk size, like upstream.
 */
static int vboxClipFileSendObjRead(HANDLE hDevice, UINT32 idClient, const VBOXCLIPFILEREQ *pReq)
{
    VBOXCLIPFILEOPEN *pOpen = vboxClipFileSendFind(pReq->u64, FALSE);
    DWORD cbToRead = pReq->u32;
    DWORD cbRead = 0;
    BYTE *pbBuf;
    int rc;

    if (!pOpen)
        return VERR_NOT_FOUND;
    if (cbToRead > g_ClipFile.cbChunk)
        cbToRead = g_ClipFile.cbChunk;
    pbBuf = (BYTE *)malloc(cbToRead ? cbToRead : 1);
    if (!pbBuf)
        return VERR_NO_MEMORY;
    if (ReadFile(pOpen->h, pbBuf, cbToRead, &cbRead, NULL))
    {
        rc = vboxClipFileWriteObj(hDevice, idClient, pReq->idContext, pReq->u64, pbBuf, cbRead);
        g_ClipFileSend.cbSent += cbRead;
    }
    else
        rc = VBoxTrayWin32ToRc(GetLastError(), VERR_READ_ERROR);
    free(pbBuf);
    return rc;
}

/**
 * Closes a list or a file (LIST_CLOSE, OBJ_CLOSE); the reply carries the
 * outcome.
 */
static int vboxClipFileSendCloseReq(HANDLE hDevice, UINT32 idClient, const VBOXCLIPFILEREQ *pReq, BOOL fList)
{
    VBOXCLIPFILEOPEN *pOpen = vboxClipFileSendFind(pReq->u64, fList);
    int rc = VINF_SUCCESS;

    if (pOpen)
    {
        vboxClipFileSendClose(pOpen);
        if (!fList)
            g_ClipFileSend.cFilesSent++;
    }
    else
        rc = VERR_NOT_FOUND;
    vboxClipFileReply(hDevice, idClient, pReq->idContext,
                      fList ? VBOX_SHCL_TX_REPLYMSGTYPE_LIST_CLOSE : VBOX_SHCL_TX_REPLYMSGTYPE_OBJ_CLOSE, rc, pReq->u64);
    return rc;
}


/*********************************************************************************************************************************
*   Entry points                                                                                                                 *
*********************************************************************************************************************************/

/**
 * Handles a transfer status for the transfer of guest files (worker thread,
 * VBoxClipFileOnStatus): one the host made or one it ends.
 */
void VBoxClipFileSendOnStatus(HANDLE hDevice, UINT32 idClient, UINT64 idContext, UINT32 enmDir, UINT32 uStatus,
                              int rcHost)
{
    int rc;

    /* A host application pastes guest files (direction from the host's point
       of view): set the transfer up and tell the host, which then asks for
       the roots. */
    if (uStatus == SHCLTRANSFERSTATUS_INITIALIZED && enmDir == SHCLTRANSFERDIR_FROM_REMOTE)
    {
        rc = vboxClipFileSendBegin(idContext);
        if (RT_FAILURE(rc))
            VBoxTrayLogRel("Clipboard: cannot give the guest files to the host, rc=%d\n", rc);
        VBoxClipFileReportStatus(hDevice, idClient, idContext,
                                 RT_SUCCESS(rc) ? SHCLTRANSFERSTATUS_INITIALIZED : SHCLTRANSFERSTATUS_ERROR, rc);
    }
    else if (   g_ClipFileSend.idContext
             && VBOX_SHCL_CONTEXTID_GET_TRANSFER(idContext) == VBOX_SHCL_CONTEXTID_GET_TRANSFER(g_ClipFileSend.idContext))
    {
        switch (uStatus)
        {
            case SHCLTRANSFERSTATUS_COMPLETED:     vboxClipFileSendEnd("done", rcHost); break;
            case SHCLTRANSFERSTATUS_CANCELED:      vboxClipFileSendEnd("canceled by the host", rcHost); break;
            case SHCLTRANSFERSTATUS_ERROR:         vboxClipFileSendEnd("failed on the host", rcHost); break;
            case SHCLTRANSFERSTATUS_KILLED:
            case SHCLTRANSFERSTATUS_UNINITIALIZED: vboxClipFileSendEnd("ended by the host", rcHost); break;
            default: break;
        }
    }
}

/**
 * Handles a host request of the transfer of guest files (worker thread):
 * fetches it and answers.  Like upstream, a request that fails without a
 * reply of its own is answered with the status ERROR.
 *
 * @returns VBox status code of fetching the request: on failure the request
 *          stays queued on the host, the transfer is ended with the status
 *          ERROR and the worker thread pauses as for any failed wait.
 * @param   uMsg        VBOX_SHCL_HOST_MSG_TRANSFER_XXX.
 */
int VBoxClipFileOnHostMsg(HANDLE hDevice, UINT32 idClient, UINT32 uMsg)
{
    VBOXCLIPFILEREQ *pReq;
    int rc;

    pReq = (VBOXCLIPFILEREQ *)malloc(sizeof(*pReq));
    if (!pReq)
        return VERR_NO_MEMORY;
    rc = vboxClipFileFetchReq(hDevice, idClient, uMsg, pReq);
    if (RT_FAILURE(rc))
    {
        if (g_ClipFileSend.idContext)
        {
            VBoxTrayLogRel("Clipboard: fetching the transfer request %lu failed, rc=%d\n", uMsg, rc);
            VBoxClipFileReportStatus(hDevice, idClient, g_ClipFileSend.idContext, SHCLTRANSFERSTATUS_ERROR, rc);
            vboxClipFileSendEnd("failed", rc);
        }
        free(pReq);
        return rc;
    }

    if (   !g_ClipFileSend.idContext
        || VBOX_SHCL_CONTEXTID_GET_TRANSFER(pReq->idContext) != VBOX_SHCL_CONTEXTID_GET_TRANSFER(g_ClipFileSend.idContext))
        rc = VERR_NOT_FOUND;
    else
    {
        switch (uMsg)
        {
            case VBOX_SHCL_HOST_MSG_TRANSFER_ROOT_LIST_HDR_READ:
                rc = vboxClipFileWriteHdr(hDevice, idClient, pReq->idContext, TRUE, 0, g_ClipFileSend.cRoots, 0);
                break;
            case VBOX_SHCL_HOST_MSG_TRANSFER_ROOT_LIST_ENTRY_READ:
                rc = vboxClipFileSendRoot(hDevice, idClient, pReq);
                break;
            case VBOX_SHCL_HOST_MSG_TRANSFER_LIST_OPEN:
                rc = vboxClipFileSendListOpen(hDevice, idClient, pReq);
                break;
            case VBOX_SHCL_HOST_MSG_TRANSFER_LIST_CLOSE:
                rc = vboxClipFileSendCloseReq(hDevice, idClient, pReq, TRUE);
                break;
            case VBOX_SHCL_HOST_MSG_TRANSFER_LIST_HDR_READ:
                rc = vboxClipFileSendListHdr(hDevice, idClient, pReq);
                break;
            case VBOX_SHCL_HOST_MSG_TRANSFER_LIST_ENTRY_READ:
                rc = vboxClipFileSendListEntry(hDevice, idClient, pReq);
                break;
            case VBOX_SHCL_HOST_MSG_TRANSFER_OBJ_OPEN:
                rc = vboxClipFileSendObjOpen(hDevice, idClient, pReq);
                break;
            case VBOX_SHCL_HOST_MSG_TRANSFER_OBJ_CLOSE:
                rc = vboxClipFileSendCloseReq(hDevice, idClient, pReq, FALSE);
                break;
            default: /* VBOX_SHCL_HOST_MSG_TRANSFER_OBJ_READ */
                rc = vboxClipFileSendObjRead(hDevice, idClient, pReq);
                break;
        }
    }
    if (RT_FAILURE(rc))
    {
        VBoxTrayLogRel("Clipboard: the transfer request %lu failed, rc=%d\n", uMsg, rc);
        VBoxClipFileReportStatus(hDevice, idClient, pReq->idContext, SHCLTRANSFERSTATUS_ERROR, rc);
    }
    free(pReq);
    return VINF_SUCCESS;
}

/**
 * The worker thread is disconnecting: ends the transfer of guest files.
 */
void VBoxClipFileWorkerDone(void)
{
    vboxClipFileSendEnd("over (VBoxTray stops)", VINF_SUCCESS);
}
