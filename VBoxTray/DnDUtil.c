/** @file
 * VBoxTray - Drag and drop, text conversions and the paths and formats handed
 * to the drop targets.
 */

#include "DnD.h"
#include <shlobj.h>
#include <stdlib.h>
#include <string.h>


/*********************************************************************************************************************************
*   Text and names                                                                                                               *
*********************************************************************************************************************************/

/**
 * Converts UTF-8 to UTF-16 (invalid sequences become U+FFFD; characters
 * outside the BMP become surrogate pairs).
 *
 * @returns Zero terminated text (malloc'ed), NULL if out of memory.
 */
static WCHAR *vboxDnDUtf8ToUtf16(const char *pch, DWORD cch)
{
    const BYTE *pb = (const BYTE *)pch;
    WCHAR *pwszRet = (WCHAR *)malloc((cch + 1) * sizeof(WCHAR));
    DWORD off = 0;
    DWORD cwc = 0;

    if (!pwszRet)
        return NULL;
    while (off < cch)
    {
        BYTE b = pb[off++];
        DWORD uc;
        DWORD cTrail;
        DWORD ucMin;

        if (b < 0x80)
        {
            pwszRet[cwc++] = b;
            continue;
        }
        if ((b & 0xe0) == 0xc0)
        {
            uc = b & 0x1f;
            cTrail = 1;
            ucMin = 0x80;
        }
        else if ((b & 0xf0) == 0xe0)
        {
            uc = b & 0x0f;
            cTrail = 2;
            ucMin = 0x800;
        }
        else if ((b & 0xf8) == 0xf0)
        {
            uc = b & 0x07;
            cTrail = 3;
            ucMin = 0x10000;
        }
        else
        {
            pwszRet[cwc++] = 0xfffd;
            continue;
        }
        while (cTrail && off < cch && (pb[off] & 0xc0) == 0x80)
        {
            uc = (uc << 6) | (pb[off++] & 0x3f);
            cTrail--;
        }
        if (cTrail || uc < ucMin || uc > 0x10ffff || (uc >= 0xd800 && uc <= 0xdfff))
            pwszRet[cwc++] = 0xfffd;
        else if (uc >= 0x10000)
        {
            /* Room: a 4 byte sequence makes 2 UTF-16 units. */
            uc -= 0x10000;
            pwszRet[cwc++] = (WCHAR)(0xd800 + (uc >> 10));
            pwszRet[cwc++] = (WCHAR)(0xdc00 + (uc & 0x3ff));
        }
        else
            pwszRet[cwc++] = (WCHAR)uc;
    }
    pwszRet[cwc] = 0;
    return pwszRet;
}

/**
 * Converts UTF-8 to the ANSI code page.
 *
 * @returns Zero terminated text (malloc'ed), NULL if out of memory.
 * @param   chDefault   Replaces characters the code page doesn't have.
 */
static char *vboxDnDUtf8ToAnsi(const char *pch, DWORD cch, char chDefault)
{
    char szDefault[2];
    WCHAR *pwsz;
    char *pszRet = NULL;
    int cb;

    szDefault[0] = chDefault;
    szDefault[1] = '\0';
    pwsz = vboxDnDUtf8ToUtf16(pch, cch);
    if (!pwsz)
        return NULL;
    cb = WideCharToMultiByte(CP_ACP, 0, pwsz, -1, NULL, 0, szDefault, NULL);
    if (cb > 0)
    {
        pszRet = (char *)malloc(cb);
        if (pszRet && !WideCharToMultiByte(CP_ACP, 0, pwsz, -1, pszRet, cb, szDefault, NULL))
        {
            free(pszRet);
            pszRet = NULL;
        }
    }
    free(pwsz);
    return pszRet;
}

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
static int vboxDnDAppendName(char *pszDst, DWORD cbDst, const char *pchName, DWORD cchName)
{
    DWORD offDst = lstrlen(pszDst);
    char *pszName;
    char *psz;
    DWORD cch;

    if (!cchName)
        return VERR_INVALID_NAME;
    pszName = vboxDnDUtf8ToAnsi(pchName, cchName, '_');
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
int VBoxDnDBuildPath(const char *pszDropDir, const char *pchRel, DWORD cchRel, char *pszPath)
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
            rc = vboxDnDAppendName(pszPath, MAX_PATH, &pchRel[off], offEnd - off);
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
DWORD VBoxDnDStrNLen(const BYTE *pb, DWORD cb)
{
    DWORD cch = 0;
    while (cch < cb && pb[cch])
        cch++;
    return cch;
}

/**
 * Decodes the %XX escapes of a URI in place.
 */
static void vboxDnDUriDecode(char *psz)
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
int VBoxDnDBuildHDrop(char *pszUriList, const char *pszDropDir, void **ppv, DWORD *pcb)
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
            vboxDnDUriDecode(pszLine);
            pszName = pszLine + lstrlen(pszLine);
            while (pszName > pszLine && (pszName[-1] == '/' || pszName[-1] == '\\'))
                *--pszName = '\0';
            while (pszName > pszLine && pszName[-1] != '/' && pszName[-1] != '\\')
                pszName--;

            lstrcpyn(szPath, pszDropDir, sizeof(szPath));
            if (RT_SUCCESS(vboxDnDAppendName(szPath, sizeof(szPath), pszName, lstrlen(pszName))))
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
 * Builds the CF_TEXT data from UTF-8 text: ANSI, CRLF line ends.
 */
int VBoxDnDBuildText(const char *pch, DWORD cch, void **ppv, DWORD *pcb)
{
    char *pszAnsi;
    char *pszRet;
    DWORD cLf = 0;
    DWORD offSrc;
    DWORD offDst = 0;

    pszAnsi = vboxDnDUtf8ToAnsi(pch, cch, '?');
    if (!pszAnsi)
        return VERR_NO_MEMORY;
    for (offSrc = 0; pszAnsi[offSrc]; offSrc++)
        if (pszAnsi[offSrc] == '\n' && (offSrc == 0 || pszAnsi[offSrc - 1] != '\r'))
            cLf++;

    pszRet = (char *)malloc(offSrc + cLf + 1);
    if (!pszRet)
    {
        free(pszAnsi);
        return VERR_NO_MEMORY;
    }
    for (offSrc = 0; pszAnsi[offSrc]; offSrc++)
    {
        if (pszAnsi[offSrc] == '\n' && (offSrc == 0 || pszAnsi[offSrc - 1] != '\r'))
            pszRet[offDst++] = '\r';
        pszRet[offDst++] = pszAnsi[offSrc];
    }
    pszRet[offDst++] = '\0';
    free(pszAnsi);

    *ppv = pszRet;
    *pcb = offDst;
    return VINF_SUCCESS;
}
