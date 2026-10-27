/** @file
 * VBoxTray - Drag and drop, receiving the data dropped on the guest and sending
 * the data dropped on the host (worker thread).
 */

#include "DnD.h"
#include <stdlib.h>
#include <string.h>


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/

/** Largest meta data (text or URI list) accepted. */
#define VBOXDND_MAX_META_SIZE       (16U * 1024 * 1024)
/** Base directory of the dropped files, under %TEMP% (as upstream). */
#define VBOXDND_DROP_DIR_NAME       "VirtualBox Dropped Files"


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/

/** A directory or file sent to the host. */
typedef struct VBOXDNDGHOBJ
{
    /** The guest path. */
    char   *pszAbs;
    /** The path in the transfer (UTF-8, '/' separated). */
    char   *pszRel;
    BOOL    fDir;
    UINT64  cb;
} VBOXDNDGHOBJ;

/** The objects sent to the host, directories before their contents. */
typedef struct VBOXDNDGHLIST
{
    VBOXDNDGHOBJ *paObjs;
    DWORD         cObjs;
    DWORD         cAlloc;
    /** Sum of the file sizes. */
    UINT64        cbFiles;
} VBOXDNDGHLIST;


/*********************************************************************************************************************************
*   Receiving data                                                                                                               *
*********************************************************************************************************************************/

/**
 * Creates the drop directory, %TEMP%\VirtualBox Dropped Files\<UTC time
 * stamp> like upstream's DnDDroppedFilesOpenTemp.
 */
static int vboxDnDCreateDropDir(char *pszDir, DWORD cbDir)
{
    char szStamp[64];
    SYSTEMTIME Now;
    DWORD cch;
    DWORD i;

    cch = GetTempPath(cbDir, pszDir);
    if (!cch || cch >= cbDir)
        return VERR_PATH_NOT_FOUND;
    if (pszDir[cch - 1] != '\\')
        lstrcat(pszDir, "\\");
    if (lstrlen(pszDir) + sizeof(VBOXDND_DROP_DIR_NAME) + 40 > cbDir)
        return VERR_FILENAME_TOO_LONG;
    lstrcat(pszDir, VBOXDND_DROP_DIR_NAME);
    CreateDirectory(pszDir, NULL); /* May exist already. */

    /* RTTimeSpecToString with the colons sanitized, e.g. 2026-10-25T18_30_00.123000000Z. */
    GetSystemTime(&Now);
    wsprintf(szStamp, "\\%04u-%02u-%02uT%02u_%02u_%02u.%03u000000Z", Now.wYear, Now.wMonth, Now.wDay,
             Now.wHour, Now.wMinute, Now.wSecond, Now.wMilliseconds);
    lstrcat(pszDir, szStamp);
    if (CreateDirectory(pszDir, NULL))
        return VINF_SUCCESS;

    /* Taken: add a number, like RTDirCreateUniqueNumbered. */
    cch = lstrlen(pszDir);
    for (i = 1; i < 1000; i++)
    {
        wsprintf(&pszDir[cch], "-%03lu", i);
        if (CreateDirectory(pszDir, NULL))
            return VINF_SUCCESS;
    }
    return VERR_OPEN_FAILED;
}

/**
 * Receives the directories and files of a URI transfer (vbglR3DnDHGRecvURIData).
 *
 * @param   cbToRecv    File bytes to receive.
 * @param   cObjects    Directories and files to receive.
 */
static int vboxDnDRecvUriData(HANDLE hDevice, UINT32 idClient, const char *pszDropDir, UINT64 cbToRecv, UINT64 cObjects,
                              UINT32 *pcFiles)
{
    char szPath[MAX_PATH];
    HANDLE hFile = INVALID_HANDLE_VALUE;
    UINT64 cbFileSize = 0;
    UINT64 cbFileWritten = 0;
    VBGLR3HGCMMSG Msg;
    int rc = VINF_SUCCESS;

    *pcFiles = 0;
    while (cObjects || cbToRecv)
    {
        UINT32 uMsg;
        UINT32 cParms;
        BYTE *pb;
        DWORD cb;

        rc = VBoxDnDGetNextMsg(hDevice, idClient, &uMsg, &cParms);
        if (RT_FAILURE(rc))
            break;

        if (uMsg == HOST_DND_FN_HG_SND_DIR)
        {
            if (!VbglR3HgcmMsgInit(&Msg, idClient, uMsg, 4, VBOXDND_CALL_TIMEOUT_MS, MAX_PATH * 4))
            {
                rc = VERR_NO_MEMORY;
                break;
            }
            VbglR3HgcmMsgSetU32(&Msg, 0, 0);                                                       /* uContext */
            pb = VbglR3HgcmMsgSetBuf(&Msg, 1, VBOXGUEST98_HGCM_PARM_BUFFER_OUT, MAX_PATH * 4, NULL); /* pvName */
            VbglR3HgcmMsgSetU32(&Msg, 2, MAX_PATH * 4);                                            /* cbName */
            VbglR3HgcmMsgSetU32(&Msg, 3, 0);                                                       /* fMode */
            rc = VbglR3HgcmMsgCall(hDevice, &Msg);
            if (RT_SUCCESS(rc))
            {
                cb = VbglR3HgcmMsgGetBufSize(&Msg, 1);
                if (cb > MAX_PATH * 4)
                    rc = VERR_TOO_MUCH_DATA;
                else
                    rc = VBoxDnDBuildPath(pszDropDir, (const char *)pb, VBoxDnDStrNLen(pb, cb), szPath);
                if (RT_SUCCESS(rc) && !CreateDirectory(szPath, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
                    rc = VERR_OPEN_FAILED;
                if (RT_SUCCESS(rc) && cObjects)
                    cObjects--;
            }
            VbglR3HgcmMsgFree(&Msg);
        }
        else if (uMsg == HOST_DND_FN_HG_SND_FILE_HDR)
        {
            if (hFile != INVALID_HANDLE_VALUE)
            {
                rc = VERR_WRONG_ORDER; /* The previous file isn't complete. */
                break;
            }
            if (!VbglR3HgcmMsgInit(&Msg, idClient, uMsg, 6, VBOXDND_CALL_TIMEOUT_MS, MAX_PATH * 4))
            {
                rc = VERR_NO_MEMORY;
                break;
            }
            VbglR3HgcmMsgSetU32(&Msg, 0, 0);                                                       /* uContext */
            pb = VbglR3HgcmMsgSetBuf(&Msg, 1, VBOXGUEST98_HGCM_PARM_BUFFER_OUT, MAX_PATH * 4, NULL); /* pvName */
            VbglR3HgcmMsgSetU32(&Msg, 2, MAX_PATH * 4);                                            /* cbName */
            VbglR3HgcmMsgSetU32(&Msg, 3, 0);                                                       /* uFlags */
            VbglR3HgcmMsgSetU32(&Msg, 4, 0);                                                       /* fMode */
            VbglR3HgcmMsgSetU64(&Msg, 5, 0);                                                       /* cbTotal */
            rc = VbglR3HgcmMsgCall(hDevice, &Msg);
            if (RT_SUCCESS(rc))
            {
                cb = VbglR3HgcmMsgGetBufSize(&Msg, 1);
                cbFileSize = VbglR3HgcmMsgGetU64(&Msg, 5);
                cbFileWritten = 0;
                if (cb > MAX_PATH * 4 || cbFileSize > cbToRecv)
                    rc = VERR_TOO_MUCH_DATA;
                else
                    rc = VBoxDnDBuildPath(pszDropDir, (const char *)pb, VBoxDnDStrNLen(pb, cb), szPath);
                if (RT_SUCCESS(rc))
                {
                    hFile = CreateFile(szPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                    if (hFile == INVALID_HANDLE_VALUE)
                        rc = VERR_OPEN_FAILED;
                    else
                        (*pcFiles)++;
                }
            }
            VbglR3HgcmMsgFree(&Msg);
        }
        else if (uMsg == HOST_DND_FN_HG_SND_FILE_DATA)
        {
            if (hFile == INVALID_HANDLE_VALUE)
            {
                rc = VERR_WRONG_ORDER;
                break;
            }
            if (!VbglR3HgcmMsgInit(&Msg, idClient, uMsg, 5, VBOXDND_CALL_TIMEOUT_MS, VBOX_DND_CHUNK_SIZE))
            {
                rc = VERR_NO_MEMORY;
                break;
            }
            VbglR3HgcmMsgSetU32(&Msg, 0, 0);                                                           /* uContext */
            pb = VbglR3HgcmMsgSetBuf(&Msg, 1, VBOXGUEST98_HGCM_PARM_BUFFER_OUT, VBOX_DND_CHUNK_SIZE, NULL); /* pvData */
            VbglR3HgcmMsgSetU32(&Msg, 2, 0);                                                           /* cbData */
            VbglR3HgcmMsgSetBuf(&Msg, 3, VBOXGUEST98_HGCM_PARM_BUFFER_OUT, 0, NULL);                   /* pvChecksum */
            VbglR3HgcmMsgSetU32(&Msg, 4, 0);                                                           /* cbChecksum */
            rc = VbglR3HgcmMsgCall(hDevice, &Msg);
            if (RT_SUCCESS(rc))
            {
                DWORD cbWritten = 0;
                cb = VbglR3HgcmMsgGetU32(&Msg, 2);
                if (cb > VBOX_DND_CHUNK_SIZE || cb > cbFileSize - cbFileWritten)
                    rc = VERR_TOO_MUCH_DATA;
                else if (cb && (!WriteFile(hFile, pb, cb, &cbWritten, NULL) || cbWritten != cb))
                    rc = VERR_WRITE_ERROR;
                else
                {
                    cbFileWritten += cb;
                    cbToRecv      -= cb;
                }
            }
            VbglR3HgcmMsgFree(&Msg);
        }
        else if (uMsg == HOST_DND_FN_CANCEL)
        {
            VBoxDnDRecvSimple(hDevice, idClient, uMsg);
            rc = VERR_CANCELLED;
        }
        else
            rc = VERR_WRONG_ORDER;
        if (RT_FAILURE(rc))
            break;

        /* A file is complete when all its bytes are here (at once for empty files). */
        if (hFile != INVALID_HANDLE_VALUE && cbFileWritten == cbFileSize)
        {
            CloseHandle(hFile);
            hFile = INVALID_HANDLE_VALUE;
            if (cObjects)
                cObjects--;
        }
    }

    if (hFile != INVALID_HANDLE_VALUE)
    {
        CloseHandle(hFile);
        DeleteFile(szPath); /* Incomplete. */
    }
    return rc;
}

/**
 * Receives the dropped data, after HOST_DND_FN_HG_SND_DATA_HDR arrived
 * (vbglR3DnDHGRecvDataMain), telling the host if it failed.
 *
 * @param   ppvData     Where to return the data for the drop target
 *                      (malloc'ed): CF_HDROP for files, else CF_TEXT.
 * @param   pcbData     Where to return its size.
 */
int VBoxDnDRecvData(HANDLE hDevice, UINT32 idClient, void **ppvData, DWORD *pcbData)
{
    char szDropDir[MAX_PATH];
    char *pszMetaFmt = NULL;
    BYTE *pbMeta = NULL;
    void *pvData = NULL;
    DWORD cbData = 0;
    UINT64 cbTotal;
    UINT64 cObjects;
    UINT32 cbMeta;
    UINT32 cbMetaRecv = 0;
    UINT32 cFiles = 0;
    VBGLR3HGCMMSG Msg;
    BYTE *pb;
    int rc;

    /*
     * The header.
     */
    if (!VbglR3HgcmMsgInit(&Msg, idClient, HOST_DND_FN_HG_SND_DATA_HDR, 12, VBOXDND_CALL_TIMEOUT_MS, VBOX_DND_CHUNK_SIZE))
        return VERR_NO_MEMORY;
    VbglR3HgcmMsgSetU32(&Msg, 0, 0);                                                           /* uContext */
    VbglR3HgcmMsgSetU32(&Msg, 1, 0);                                                           /* uFlags */
    VbglR3HgcmMsgSetU32(&Msg, 2, 0);                                                           /* uScreenId */
    VbglR3HgcmMsgSetU64(&Msg, 3, 0);                                                           /* cbTotal */
    VbglR3HgcmMsgSetU32(&Msg, 4, 0);                                                           /* cbMeta */
    pb = VbglR3HgcmMsgSetBuf(&Msg, 5, VBOXGUEST98_HGCM_PARM_BUFFER_OUT, VBOX_DND_CHUNK_SIZE, NULL); /* pvMetaFmt */
    VbglR3HgcmMsgSetU32(&Msg, 6, 0);                                                           /* cbMetaFmt */
    VbglR3HgcmMsgSetU64(&Msg, 7, 0);                                                           /* cObjects */
    VbglR3HgcmMsgSetU32(&Msg, 8, 0);                                                           /* enmCompression */
    VbglR3HgcmMsgSetU32(&Msg, 9, 0);                                                           /* enmChecksumType */
    VbglR3HgcmMsgSetBuf(&Msg, 10, VBOXGUEST98_HGCM_PARM_BUFFER_OUT, 0, NULL);                  /* pvChecksum */
    VbglR3HgcmMsgSetU32(&Msg, 11, 0);                                                          /* cbChecksum */
    rc = VbglR3HgcmMsgCall(hDevice, &Msg);
    if (RT_SUCCESS(rc))
    {
        cbTotal  = VbglR3HgcmMsgGetU64(&Msg, 3);
        cbMeta   = VbglR3HgcmMsgGetU32(&Msg, 4);
        cObjects = VbglR3HgcmMsgGetU64(&Msg, 7);
        pb[VBOX_DND_CHUNK_SIZE - 1] = '\0';
        pszMetaFmt = _strdup((const char *)pb);
        if (!pszMetaFmt)
            rc = VERR_NO_MEMORY;
        else if (cbMeta > VBOXDND_MAX_META_SIZE || cbMeta > cbTotal)
            rc = VERR_TOO_MUCH_DATA;
    }
    VbglR3HgcmMsgFree(&Msg);

    /*
     * The meta data: the text, or the URI list of the transfer roots.
     */
    if (RT_SUCCESS(rc))
    {
        pbMeta = (BYTE *)malloc(cbMeta + 1);
        if (!pbMeta)
            rc = VERR_NO_MEMORY;
    }
    while (RT_SUCCESS(rc) && cbMetaRecv < cbMeta)
    {
        DWORD cbChunk = cbMeta - cbMetaRecv < VBOX_DND_CHUNK_SIZE ? cbMeta - cbMetaRecv : VBOX_DND_CHUNK_SIZE;
        DWORD cbGot;

        if (!VbglR3HgcmMsgInit(&Msg, idClient, HOST_DND_FN_HG_SND_DATA, 5, VBOXDND_CALL_TIMEOUT_MS, cbChunk))
        {
            rc = VERR_NO_MEMORY;
            break;
        }
        VbglR3HgcmMsgSetU32(&Msg, 0, 0);                                                       /* uContext */
        pb = VbglR3HgcmMsgSetBuf(&Msg, 1, VBOXGUEST98_HGCM_PARM_BUFFER_OUT, cbChunk, NULL);    /* pvData */
        VbglR3HgcmMsgSetU32(&Msg, 2, 0);                                                       /* cbData */
        VbglR3HgcmMsgSetBuf(&Msg, 3, VBOXGUEST98_HGCM_PARM_BUFFER_OUT, 0, NULL);               /* pvChecksum */
        VbglR3HgcmMsgSetU32(&Msg, 4, 0);                                                       /* cbChecksum */
        rc = VbglR3HgcmMsgCall(hDevice, &Msg);
        if (RT_SUCCESS(rc))
        {
            cbGot = VbglR3HgcmMsgGetU32(&Msg, 2);
            if (!cbGot || cbGot > cbChunk)
                rc = VERR_TOO_MUCH_DATA;
            else
            {
                memcpy(pbMeta + cbMetaRecv, pb, cbGot);
                cbMetaRecv += cbGot;
            }
        }
        VbglR3HgcmMsgFree(&Msg);
    }
    if (pbMeta)
        pbMeta[cbMetaRecv] = '\0';

    /*
     * Files and directories, or text.
     */
    if (RT_SUCCESS(rc))
    {
        if (!lstrcmpi(pszMetaFmt, "text/uri-list"))
        {
            rc = vboxDnDCreateDropDir(szDropDir, sizeof(szDropDir));
            if (RT_SUCCESS(rc))
                rc = vboxDnDRecvUriData(hDevice, idClient, szDropDir, cbTotal - cbMeta, cObjects, &cFiles);
            if (RT_SUCCESS(rc))
                rc = VBoxDnDBuildHDrop((char *)pbMeta, szDropDir, &pvData, &cbData);
            if (RT_SUCCESS(rc))
                VBoxTrayLogRel("DnD: received %lu file(s), %lu objects, %lu KB in \"%s\"\n", cFiles,
                               (unsigned long)cObjects, (unsigned long)((cbTotal - cbMeta) / 1024), szDropDir);
        }
        else
        {
            rc = VBoxDnDBuildText((const char *)pbMeta, cbMetaRecv, &pvData, &cbData);
            if (RT_SUCCESS(rc))
                VBoxTrayLogRel("DnD: received %lu bytes of text (%s)\n", cbMetaRecv, pszMetaFmt);
        }
    }
    free(pbMeta);
    free(pszMetaFmt);

    *ppvData = pvData;
    *pcbData = cbData;
    if (RT_FAILURE(rc) && rc != VERR_CANCELLED)
    {
        VBoxTrayLogRel("DnD: receiving the data failed, rc=%d\n", rc);
        VBoxDnDSendProgress(hDevice, idClient, DND_PROGRESS_ERROR, 100, rc);
    }
    return rc;
}


/*********************************************************************************************************************************
*   Sending data to the host                                                                                                     *
*********************************************************************************************************************************/

/**
 * Adds a directory or file to the transfer list.
 *
 * @param   pszRel      Its path in the transfer (ANSI, '\' separated).
 */
static int vboxDnDGhListAdd(VBOXDNDGHLIST *pList, const char *pszAbs, const char *pszRel, BOOL fDir, UINT64 cb)
{
    VBOXDNDGHOBJ *pObj;
    char *psz;

    if (pList->cObjs == pList->cAlloc)
    {
        VBOXDNDGHOBJ *paNew = (VBOXDNDGHOBJ *)realloc(pList->paObjs, (pList->cAlloc + 64) * sizeof(VBOXDNDGHOBJ));
        if (!paNew)
            return VERR_NO_MEMORY;
        pList->paObjs  = paNew;
        pList->cAlloc += 64;
    }

    pObj = &pList->paObjs[pList->cObjs];
    pObj->pszAbs = _strdup(pszAbs);
    pObj->pszRel = VBoxTrayAnsiToUtf8(pszRel);
    if (!pObj->pszAbs || !pObj->pszRel)
    {
        free(pObj->pszAbs);
        free(pObj->pszRel);
        return VERR_NO_MEMORY;
    }
    /* After the conversion, since a DBCS trail byte can look like a backslash. */
    for (psz = pObj->pszRel; *psz; psz++)
        if (*psz == '\\')
            *psz = '/';
    pObj->fDir = fDir;
    pObj->cb   = cb;
    pList->cObjs++;
    if (!fDir)
        pList->cbFiles += cb;
    return VINF_SUCCESS;
}

/**
 * Adds a file, or a directory and everything in it, to the transfer list.
 *
 * @param   pFd         What FindFirstFile/FindNextFile found for it.
 */
static int vboxDnDGhListAddTree(VBOXDNDGHLIST *pList, const char *pszAbs, const char *pszRel, const WIN32_FIND_DATA *pFd)
{
    char szAbs[MAX_PATH];
    char szRel[MAX_PATH];
    WIN32_FIND_DATA Fd;
    HANDLE hFind;
    int rc;

    if (!(pFd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return vboxDnDGhListAdd(pList, pszAbs, pszRel, FALSE, ((UINT64)pFd->nFileSizeHigh << 32) | pFd->nFileSizeLow);

    rc = vboxDnDGhListAdd(pList, pszAbs, pszRel, TRUE, 0);
    if (RT_FAILURE(rc))
        return rc;
    if (lstrlen(pszAbs) + 4 >= MAX_PATH)
        return VERR_FILENAME_TOO_LONG;
    wsprintf(szAbs, "%s\\*.*", pszAbs);
    hFind = FindFirstFile(szAbs, &Fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return VINF_SUCCESS; /* Empty. */
    do
    {
        if (!lstrcmp(Fd.cFileName, ".") || !lstrcmp(Fd.cFileName, ".."))
            continue;
        if (   lstrlen(pszAbs) + 1 + lstrlen(Fd.cFileName) >= MAX_PATH
            || lstrlen(pszRel) + 1 + lstrlen(Fd.cFileName) >= MAX_PATH)
        {
            rc = VERR_FILENAME_TOO_LONG;
            break;
        }
        wsprintf(szAbs, "%s\\%s", pszAbs, Fd.cFileName);
        wsprintf(szRel, "%s\\%s", pszRel, Fd.cFileName);
        rc = vboxDnDGhListAddTree(pList, szAbs, szRel, &Fd);
    } while (RT_SUCCESS(rc) && FindNextFile(hFind, &Fd));
    FindClose(hFind);
    return rc;
}

static void vboxDnDGhListFree(VBOXDNDGHLIST *pList)
{
    DWORD i;
    for (i = 0; i < pList->cObjs; i++)
    {
        free(pList->paObjs[i].pszAbs);
        free(pList->paObjs[i].pszRel);
    }
    free(pList->paObjs);
    memset(pList, 0, sizeof(*pList));
}

/**
 * Appends a transfer root to the URI list: file:///<name>, %XX escaped, as
 * DnDTransferListGetRoots makes them.
 *
 * @param   ppszMeta    The list (malloc'ed, reallocated).
 * @param   pcchMeta    Its length.
 * @param   pszName     The root's name (ANSI).
 */
static int vboxDnDGhMetaAddRoot(char **ppszMeta, DWORD *pcchMeta, const char *pszName)
{
    static const char s_szHex[] = "0123456789ABCDEF";
    char *pszUtf8 = VBoxTrayAnsiToUtf8(pszName);
    char *pszNew;
    BYTE *pb;
    DWORD off;

    if (!pszUtf8)
        return VERR_NO_MEMORY;
    pszNew = (char *)realloc(*ppszMeta, *pcchMeta + sizeof("file:///\r\n") + lstrlen(pszUtf8) * 3);
    if (!pszNew)
    {
        free(pszUtf8);
        return VERR_NO_MEMORY;
    }
    *ppszMeta = pszNew;

    off = *pcchMeta;
    memcpy(&pszNew[off], "file:///", 8);
    off += 8;
    for (pb = (BYTE *)pszUtf8; *pb; pb++)
    {
        if (   (*pb >= 'a' && *pb <= 'z') || (*pb >= 'A' && *pb <= 'Z') || (*pb >= '0' && *pb <= '9')
            || *pb == '-' || *pb == '.' || *pb == '_' || *pb == '~')
            pszNew[off++] = (char)*pb;
        else
        {
            pszNew[off++] = '%';
            pszNew[off++] = s_szHex[*pb >> 4];
            pszNew[off++] = s_szHex[*pb & 0xf];
        }
    }
    memcpy(&pszNew[off], VBOX_DND_FORMATS_SEPARATOR, 2);
    off += 2;
    pszNew[off] = '\0';
    *pcchMeta = off;
    free(pszUtf8);
    return VINF_SUCCESS;
}

/**
 * Sends dropped guest files and directories to the host
 * (vbglR3DnDGHSendTransferData): the URI list of the roots as meta data,
 * then every directory and file below them.
 *
 * @param   pszPaths    The dropped paths, CF_HDROP style (each zero
 *                      terminated, an empty one ends the list).
 */
static int vboxDnDGhSendFiles(HANDLE hDevice, UINT32 idClient, const char *pszPaths)
{
    VBOXDNDGHLIST List;
    char *pszMeta = NULL;
    DWORD cchMeta = 0;
    const char *psz;
    DWORD i;
    int rc = VINF_SUCCESS;

    memset(&List, 0, sizeof(List));
    for (psz = pszPaths; *psz && RT_SUCCESS(rc); psz += lstrlen(psz) + 1)
    {
        WIN32_FIND_DATA Fd;
        HANDLE hFind = FindFirstFile(psz, &Fd);
        if (hFind == INVALID_HANDLE_VALUE)
        {
            VBoxTrayLogRel("DnD: \"%s\" not found, not sent\n", psz);
            continue; /* A drive or such. */
        }
        FindClose(hFind);
        rc = vboxDnDGhListAddTree(&List, psz, Fd.cFileName, &Fd);
        if (RT_SUCCESS(rc))
            rc = vboxDnDGhMetaAddRoot(&pszMeta, &cchMeta, Fd.cFileName);
    }
    if (RT_SUCCESS(rc) && !List.cObjs)
        rc = VERR_NO_DATA;

    /* The meta data includes the terminator, like upstream's. */
    if (RT_SUCCESS(rc))
        rc = VBoxDnDGhSendMeta(hDevice, idClient, "text/uri-list", pszMeta, cchMeta + 1, cchMeta + 1 + List.cbFiles,
                               List.cObjs);
    for (i = 0; RT_SUCCESS(rc) && i < List.cObjs; i++)
    {
        if (List.paObjs[i].fDir)
            rc = VBoxDnDGhSendDir(hDevice, idClient, List.paObjs[i].pszRel);
        else
            rc = VBoxDnDGhSendFile(hDevice, idClient, List.paObjs[i].pszAbs, List.paObjs[i].pszRel, List.paObjs[i].cb);
        if (RT_FAILURE(rc))
            VBoxTrayLogRel("DnD: sending \"%s\" failed, rc=%d\n", List.paObjs[i].pszAbs, rc);
    }
    if (RT_SUCCESS(rc))
        VBoxTrayLogRel("DnD: sent %lu objects, %lu KB to the host\n", List.cObjs, (unsigned long)(List.cbFiles / 1024));

    free(pszMeta);
    vboxDnDGhListFree(&List);
    return rc;
}

/**
 * Sends the data of the guest drag to the host, telling the host if it failed.
 *
 * @param   pszData     The data the proxy window caught, NULL if none: a
 *                      CF_HDROP style path list if fFiles, else UTF-8 text.
 */
int VBoxDnDGhSendData(HANDLE hDevice, UINT32 idClient, const char *pszData, BOOL fFiles)
{
    int rc;

    if (!pszData)
        rc = VERR_NO_DATA;
    else if (fFiles)
        rc = vboxDnDGhSendFiles(hDevice, idClient, pszData);
    else
    {
        DWORD cb = lstrlen(pszData) + 1;
        rc = VBoxDnDGhSendMeta(hDevice, idClient, NULL, pszData, cb, cb, 0);
        if (RT_SUCCESS(rc))
            VBoxTrayLogRel("DnD: sent %lu bytes of text to the host\n", cb);
    }

    if (RT_FAILURE(rc))
    {
        VBoxTrayLogRel("DnD: sending to the host failed, rc=%d\n", rc);
        VBoxDnDSendError(hDevice, idClient, rc); /* So the host doesn't wait for the data. */
    }
    return rc;
}
