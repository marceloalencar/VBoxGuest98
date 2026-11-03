/** @file
 * VBoxTray - Helpers shared by the services: Win32 errors, strings, the paths
 * of host files and CF_HDROP data (drag and drop, clipboard).
 */

#include "VBoxTray.h"
#include <shlobj.h>
#include <stdlib.h>
#include <string.h>


/*********************************************************************************************************************************
*   Win32 errors                                                                                                                 *
*********************************************************************************************************************************/

/**
 * Turns a Win32 error into a VBox status code.
 *
 * @returns The status code, rcDefault for errors without a specific one.
 * @param   dwErr       The Win32 error (GetLastError).
 * @param   rcDefault   What the caller reports for the others (VERR_READ_ERROR,
 *                      VERR_OPEN_FAILED, ...).
 */
int VBoxTrayWin32ToRc(DWORD dwErr, int rcDefault)
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
        default:                        return rcDefault;
    }
}


/*********************************************************************************************************************************
*   Strings, paths and CF_HDROP                                                                                                  *
*********************************************************************************************************************************/

/**
 * Turns one UTF-8 path component from the host into a safe Windows 98 name.
 *
 * Characters Windows doesn't allow in names become '_', so do trailing dots
 * and spaces (Windows would drop them, and CF_HDROP must name the files we
 * wrote).  "." and ".." are refused, so nothing gets out of the drop directory.
 *
 * @returns VINF_SUCCESS, VERR_INVALID_NAME or VERR_NO_MEMORY.
 * @param   pszDst  Where to append the name.
 * @param   cbDst   Size of pszDst.
 */
static int vboxTrayAppendName(char *pszDst, DWORD cbDst, const char *pchName, DWORD cchName)
{
    DWORD offDst = lstrlen(pszDst);
    char *pszName;
    char *psz;
    DWORD cch;

    if (!cchName)
        return VERR_INVALID_NAME;
    pszName = VBoxTrayUtf8ToAnsi(pchName, cchName, '_');
    if (!pszName)
        return VERR_NO_MEMORY;
    if (!lstrcmp(pszName, ".") || !lstrcmp(pszName, ".."))
    {
        free(pszName);
        return VERR_INVALID_NAME;
    }

    for (psz = pszName; *psz; psz++)
    {
        if (IsDBCSLeadByte((BYTE)*psz) && psz[1])
        {
            psz++; /* The trail byte may look like a backslash. */
            continue;
        }
        if ((BYTE)*psz < 0x20 || strchr("\\/:*?\"<>|", *psz))
            *psz = '_';
    }
    cch = lstrlen(pszName);
    while (cch && (pszName[cch - 1] == '.' || pszName[cch - 1] == ' '))
        pszName[--cch] = '_';

    if (offDst + 1 + cch + 1 > cbDst)
    {
        free(pszName);
        return VERR_FILENAME_TOO_LONG;
    }
    if (offDst && pszDst[offDst - 1] != '\\')
        pszDst[offDst++] = '\\';
    memcpy(&pszDst[offDst], pszName, cch + 1);
    free(pszName);
    return VINF_SUCCESS;
}

/**
 * Builds the guest path of an object of the transfer.
 *
 * @param   pszDropDir  The drop directory.
 * @param   pchRel      The host's relative path (UTF-8, '/' separated).
 * @param   cchRel      Its length.
 * @param   pszPath     Where to return the path (MAX_PATH bytes).
 */
int VBoxTrayBuildPath(const char *pszDropDir, const char *pchRel, DWORD cchRel, char *pszPath)
{
    DWORD off = 0;
    int rc = VERR_INVALID_NAME;

    lstrcpyn(pszPath, pszDropDir, MAX_PATH);
    while (off < cchRel)
    {
        DWORD offEnd = off;
        while (offEnd < cchRel && pchRel[offEnd] != '/' && pchRel[offEnd] != '\\')
            offEnd++;
        if (offEnd > off)
        {
            rc = vboxTrayAppendName(pszPath, MAX_PATH, &pchRel[off], offEnd - off);
            if (RT_FAILURE(rc))
                return rc;
        }
        off = offEnd + 1;
    }
    return rc; /* VERR_INVALID_NAME if there was no component at all. */
}

/**
 * Returns the length of a string in a buffer that may lack the terminator.
 */
DWORD VBoxTrayStrNLen(const BYTE *pb, DWORD cb)
{
    DWORD cch = 0;
    while (cch < cb && pb[cch])
        cch++;
    return cch;
}

/**
 * Decodes the %XX escapes of a URI in place.
 */
static void vboxTrayUriDecode(char *psz)
{
    char *pszDst = psz;
    while (*psz)
    {
        if (   psz[0] == '%'
            && strchr("0123456789abcdefABCDEF", psz[1]) && psz[1]
            && strchr("0123456789abcdefABCDEF", psz[2]) && psz[2])
        {
            char szHex[3];
            szHex[0] = psz[1];
            szHex[1] = psz[2];
            szHex[2] = '\0';
            *pszDst++ = (char)strtoul(szHex, NULL, 16);
            psz += 3;
        }
        else
            *pszDst++ = *psz++;
    }
    *pszDst = '\0';
}

/**
 * Builds the CF_HDROP data (DROPFILES and the paths) from the host's URI
 * list: one entry per transfer root, in the drop directory.
 *
 * @returns VBox status code.
 * @param   pszUriList  The URI list (zero terminated, modified).
 * @param   pszDropDir  The drop directory.
 * @param   ppv         Where to return the data (malloc'ed).
 * @param   pcb         Where to return its size.
 */
int VBoxTrayBuildHDrop(char *pszUriList, const char *pszDropDir, void **ppv, DWORD *pcb)
{
    char szPath[MAX_PATH];
    DROPFILES *pDropFiles;
    char *pszLine = pszUriList;
    DWORD cbAlloc = sizeof(DROPFILES) + 1;
    DWORD off = sizeof(DROPFILES);
    BYTE *pb;

    pb = (BYTE *)malloc(cbAlloc);
    if (!pb)
        return VERR_NO_MEMORY;

    while (*pszLine)
    {
        char *pszEnd = pszLine + strcspn(pszLine, "\r\n");
        char *pszName;
        char chSaved = *pszEnd;
        DWORD cchPath;

        *pszEnd = '\0';
        if (*pszLine && *pszLine != '#')
        {
            /* file:///C:/dir/name, file://host/dir/name/ or a plain path: the last component counts. */
            vboxTrayUriDecode(pszLine);
            pszName = pszLine + lstrlen(pszLine);
            while (pszName > pszLine && (pszName[-1] == '/' || pszName[-1] == '\\'))
                *--pszName = '\0';
            while (pszName > pszLine && pszName[-1] != '/' && pszName[-1] != '\\')
                pszName--;

            lstrcpyn(szPath, pszDropDir, sizeof(szPath));
            if (RT_SUCCESS(vboxTrayAppendName(szPath, sizeof(szPath), pszName, lstrlen(pszName))))
            {
                BYTE *pbNew;
                cchPath = lstrlen(szPath) + 1;
                pbNew = (BYTE *)realloc(pb, cbAlloc + cchPath);
                if (!pbNew)
                {
                    free(pb);
                    return VERR_NO_MEMORY;
                }
                pb = pbNew;
                memcpy(pb + off, szPath, cchPath);
                off     += cchPath;
                cbAlloc += cchPath;
            }
        }
        *pszEnd = chSaved;
        pszLine = pszEnd;
        while (*pszLine == '\r' || *pszLine == '\n')
            pszLine++;
    }

    if (off == sizeof(DROPFILES))
    {
        free(pb);
        return VERR_NOT_FOUND;
    }
    pb[off] = '\0'; /* The list ends with an empty string. */

    pDropFiles = (DROPFILES *)pb;
    pDropFiles->pFiles = sizeof(DROPFILES);
    pDropFiles->pt.x   = 0;
    pDropFiles->pt.y   = 0;
    pDropFiles->fNC    = FALSE;
    pDropFiles->fWide  = FALSE;
    *ppv = pb;
    *pcb = cbAlloc;
    return VINF_SUCCESS;
}

/**
 * Copies the paths of a CF_HDROP into a list of zero terminated strings
 * ending with an empty one.
 *
 * @returns The list (malloc'ed), NULL if out of memory.
 */
char *VBoxTrayCopyHDrop(HDROP hDrop)
{
    UINT cFiles = DragQueryFile(hDrop, 0xffffffff, NULL, 0);
    DWORD cb = 1;
    DWORD off = 0;
    char *pszRet;
    UINT i;

    for (i = 0; i < cFiles; i++)
        cb += DragQueryFile(hDrop, i, NULL, 0) + 1;
    pszRet = (char *)malloc(cb);
    if (!pszRet)
        return NULL;
    for (i = 0; i < cFiles; i++)
        off += DragQueryFile(hDrop, i, &pszRet[off], cb - off) + 1;
    pszRet[off] = '\0';
    return pszRet;
}
