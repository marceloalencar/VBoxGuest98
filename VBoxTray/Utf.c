/** @file
 * VBoxTray - Text conversions between UTF-8 (the host's) and the ANSI code
 * page.  Also built into VBoxControl.
 */

#include "VBoxTray.h"
#include <stdlib.h>

/**
 * Converts UTF-8 to UTF-16 (invalid sequences become U+FFFD; characters
 * outside the BMP become surrogate pairs).
 *
 * @returns Zero terminated text (malloc'ed), NULL if out of memory.
 */
static WCHAR *vboxTrayUtf8ToUtf16(const char *pch, DWORD cch)
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
char *VBoxTrayUtf8ToAnsi(const char *pch, DWORD cch, char chDefault)
{
    char szDefault[2];
    WCHAR *pwsz;
    char *pszRet = NULL;
    int cb;

    szDefault[0] = chDefault;
    szDefault[1] = '\0';
    pwsz = vboxTrayUtf8ToUtf16(pch, cch);
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
 * Converts text in the ANSI code page to UTF-8.
 *
 * @returns Zero terminated UTF-8 (malloc'ed), NULL on failure.
 */
char *VBoxTrayAnsiToUtf8(const char *psz)
{
    WCHAR *pwsz;
    BYTE *pbRet;
    DWORD off = 0;
    int cwc;
    int i;

    cwc = MultiByteToWideChar(CP_ACP, 0, psz, -1, NULL, 0);
    if (cwc <= 0)
        return NULL;
    pwsz = (WCHAR *)malloc(cwc * sizeof(WCHAR));
    pbRet = (BYTE *)malloc(cwc * 3); /* Up to 3 bytes per UTF-16 unit, 4 per surrogate pair. */
    if (!pwsz || !pbRet || !MultiByteToWideChar(CP_ACP, 0, psz, -1, pwsz, cwc))
    {
        free(pwsz);
        free(pbRet);
        return NULL;
    }

    for (i = 0; i < cwc; i++)
    {
        DWORD uc = pwsz[i];
        if (   uc >= 0xd800 && uc <= 0xdbff
            && i + 1 < cwc && pwsz[i + 1] >= 0xdc00 && pwsz[i + 1] <= 0xdfff)
        {
            uc = 0x10000 + ((uc - 0xd800) << 10) + (pwsz[i + 1] - 0xdc00);
            i++;
        }
        if (uc < 0x80)
            pbRet[off++] = (BYTE)uc;
        else if (uc < 0x800)
        {
            pbRet[off++] = (BYTE)(0xc0 | (uc >> 6));
            pbRet[off++] = (BYTE)(0x80 | (uc & 0x3f));
        }
        else if (uc < 0x10000)
        {
            pbRet[off++] = (BYTE)(0xe0 | (uc >> 12));
            pbRet[off++] = (BYTE)(0x80 | ((uc >> 6) & 0x3f));
            pbRet[off++] = (BYTE)(0x80 | (uc & 0x3f));
        }
        else
        {
            pbRet[off++] = (BYTE)(0xf0 | (uc >> 18));
            pbRet[off++] = (BYTE)(0x80 | ((uc >> 12) & 0x3f));
            pbRet[off++] = (BYTE)(0x80 | ((uc >> 6) & 0x3f));
            pbRet[off++] = (BYTE)(0x80 | (uc & 0x3f));
        }
    }
    free(pwsz);
    return (char *)pbRet; /* The terminator was converted too. */
}
