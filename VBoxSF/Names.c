/** @file
 * VBoxSF - 8.3 names for DOS programs.
 *
 * An entry's 8.3 name is the host's short name, the long name itself if that
 * is a valid 8.3 name in ASCII, or else a generated alias in the NT style:
 * the first two valid characters, four hex digits of a hash of the long
 * name, "~1" and the first three valid characters of the extension
 * ("Long File Name.txt" -> "LO3F2A~1.TXT").  The alias only depends on the
 * name, so nothing has to be remembered: a path element with that exact
 * shape is resolved by listing its directory and looking for the entry whose
 * alias it is (unless an entry has that very name).  Two long names with the
 * same two first characters and the same 16-bit hash would share an alias;
 * the first one listed wins.
 */
#include "VBoxSF.h"

#pragma VxD_LOCKED_CODE_SEG
#pragma VxD_LOCKED_DATA_SEG


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/

/** Listing buffer for the alias lookups. */
#define SF_NAMES_BUF_SIZE       (16 * 1024)


/*********************************************************************************************************************************
*   8.3 names                                                                                                                    *
*********************************************************************************************************************************/

/** Characters a DOS name may have (besides letters and digits). */
static BOOL sfIsDosChar(WCHAR wc)
{
    static const char s_szSpecial[] = "!#$%&'()-@^_`{}~";
    ULONG i;
    if ((wc >= 'A' && wc <= 'Z') || (wc >= 'a' && wc <= 'z') || (wc >= '0' && wc <= '9'))
        return TRUE;
    for (i = 0; i < sizeof(s_szSpecial) - 1; i++)
        if (wc == (WCHAR)s_szSpecial[i])
            return TRUE;
    return FALSE;
}

/**
 * Checks a name for 8.3 and gives it in upper case.
 *
 * @returns Length, 0 if it isn't a valid 8.3 name in ASCII.
 * @param   pszName     Where to put it (13 bytes).
 */
ULONG SfTo83(const WCHAR *pwc, ULONG cwc, char *pszName)
{
    ULONG i, cchBase = 0, cchExt = 0;
    BOOL fDot = FALSE;

    if (!cwc || cwc > 12)
        return 0;
    for (i = 0; i < cwc; i++)
    {
        WCHAR wc = pwc[i];
        if (wc == '.')
        {
            if (fDot || !i)
                return 0;
            fDot = TRUE;
        }
        else if (!sfIsDosChar(wc))
            return 0;
        else if (fDot)
            cchExt++;
        else
            cchBase++;
        if (wc >= 'a' && wc <= 'z')
            wc -= 'a' - 'A';
        pszName[i] = (char)wc;
    }
    if (!cchBase || cchBase > 8 || cchExt > 3 || (fDot && !cchExt))
        return 0;
    pszName[cwc] = '\0';
    return cwc;
}

/**
 * Makes the alias of a long name (see the file comment).
 *
 * @returns Length.
 * @param   pszAlias    Where to put it (13 bytes).
 */
static ULONG sfMakeAlias(const WCHAR *pwc, ULONG cwc, char *pszAlias)
{
    static const char s_achHex[] = "0123456789ABCDEF";
    UINT32 uHash = 2166136261U;
    ULONG i, iDot = cwc, off = 0, cchExt = 0;

    /* FNV-1a over the upper case name, folded to 16 bits. */
    for (i = 0; i < cwc; i++)
    {
        WCHAR wc = pwc[i];
        if (wc >= 'a' && wc <= 'z')
            wc -= 'a' - 'A';
        uHash = (uHash ^ (wc & 0xff)) * 16777619U;
        uHash = (uHash ^ (wc >> 8)) * 16777619U;
        if (wc == '.')
            iDot = i;
    }
    uHash = (uHash ^ (uHash >> 16)) & 0xffff;
    if (iDot == 0)
        iDot = cwc;                     /* ".profile": no extension. */

    for (i = 0; i < iDot && off < 2; i++)
        if (pwc[i] != ' ' && pwc[i] != '.')
            pszAlias[off++] = sfIsDosChar(pwc[i]) ? (char)(pwc[i] >= 'a' && pwc[i] <= 'z' ? pwc[i] - 'a' + 'A' : pwc[i])
                                                  : '_';
    while (off < 2)
        pszAlias[off++] = '_';
    pszAlias[off++] = s_achHex[(uHash >> 12) & 0xf];
    pszAlias[off++] = s_achHex[(uHash >> 8) & 0xf];
    pszAlias[off++] = s_achHex[(uHash >> 4) & 0xf];
    pszAlias[off++] = s_achHex[uHash & 0xf];
    pszAlias[off++] = '~';
    pszAlias[off++] = '1';
    for (i = iDot + 1; i < cwc && cchExt < 3; i++)
        if (pwc[i] != ' ' && pwc[i] != '.')
        {
            if (!cchExt)
                pszAlias[off++] = '.';
            pszAlias[off++] = sfIsDosChar(pwc[i]) ? (char)(pwc[i] >= 'a' && pwc[i] <= 'z' ? pwc[i] - 'a' + 'A' : pwc[i])
                                                  : '_';
            cchExt++;
        }
    pszAlias[off] = '\0';
    return off;
}

/**
 * Checks whether a name has the shape of a generated alias.
 */
BOOL SfIsAlias(const WCHAR *pwc, ULONG cwc)
{
    ULONG i;
    if (cwc < 8 || cwc > 12 || (cwc > 8 && pwc[8] != '.') || pwc[6] != '~' || pwc[7] != '1')
        return FALSE;
    for (i = 2; i < 6; i++)
        if (!((pwc[i] >= '0' && pwc[i] <= '9') || (pwc[i] >= 'A' && pwc[i] <= 'F')))
            return FALSE;
    return TRUE;
}

/**
 * Gets the 8.3 name of a directory entry (see the file comment).
 *
 * @returns Length.
 * @param   pszName     Where to put it, upper case (13 bytes).
 * @param   pfMade      Where to say whether it is a generated alias (optional).
 */
ULONG SfGetShortName(const SHFLDIRINFO *pEntry, char *pszName, BOOL *pfMade)
{
    ULONG cch = 0;

    if (pfMade)
        *pfMade = FALSE;
    if (   pEntry->name.String.utf16[0] == '.'
        && (   pEntry->name.u16Length == sizeof(WCHAR)
            || (pEntry->name.u16Length == 2 * sizeof(WCHAR) && pEntry->name.String.utf16[1] == '.')))
    {
        /* "." and "..": as they are. */
        pszName[0] = '.';
        pszName[1] = pEntry->name.u16Length == 2 * sizeof(WCHAR) ? '.' : '\0';
        pszName[2] = '\0';
        return pEntry->name.u16Length / sizeof(WCHAR);
    }
#ifndef VBOXSF_TEST_NO_HOST_SHORT_NAMES /* Test builds: act as on a host without 8.3 names. */
    if (pEntry->cucShortName && pEntry->cucShortName <= 12)
        cch = SfTo83(pEntry->uszShortName, pEntry->cucShortName, pszName);
#endif
    if (!cch)
        cch = SfTo83(pEntry->name.String.utf16, pEntry->name.u16Length / sizeof(WCHAR), pszName);
    if (!cch)
    {
        cch = sfMakeAlias(pEntry->name.String.utf16, pEntry->name.u16Length / sizeof(WCHAR), pszName);
        if (pfMade)
            *pfMade = TRUE;
    }
    return cch;
}


/*********************************************************************************************************************************
*   Path translation                                                                                                             *
*********************************************************************************************************************************/

static BOOL sfEqualNoCase(const WCHAR *pwc1, ULONG cwc1, const WCHAR *pwc2, ULONG cwc2)
{
    ULONG i;
    if (cwc1 != cwc2)
        return FALSE;
    for (i = 0; i < cwc1; i++)
    {
        WCHAR wc1 = pwc1[i], wc2 = pwc2[i];
        if (wc1 >= 'a' && wc1 <= 'z')
            wc1 -= 'a' - 'A';
        if (wc2 >= 'a' && wc2 <= 'z')
            wc2 -= 'a' - 'A';
        if (wc1 != wc2)
        {
            /* Not ASCII: let IFSMgr compare. */
            WCHAR awc[2];
            SfIfsUniToUpper(&awc[0], &wc1, sizeof(WCHAR));
            SfIfsUniToUpper(&awc[1], &wc2, sizeof(WCHAR));
            if (awc[0] != awc[1])
                return FALSE;
        }
    }
    return TRUE;
}

/**
 * Looks for a name in a host directory, as it is or as an 8.3 name.
 *
 * @returns VBox status code, VERR_FILE_NOT_FOUND if there is no such entry.
 * @param   pDir        The directory's host path.
 * @param   pwcName     The name looked for (an element of the guest path).
 * @param   fShort      Return the entry's 8.3 name rather than its long name.
 * @param   pwszOut     Where to put the name found (SHFL_MAX_LEN + 1).
 * @param   pcwcOut     Its length.
 */
static int sfFindInDir(SHFLROOT idRoot, const SHFLSTRING *pDir, const WCHAR *pwcName, ULONG cwcName, BOOL fShort,
                       WCHAR *pwszOut, ULONG *pcwcOut)
{
    SHFLCREATEPARMS Parms;
    BYTE *pbBuf;
    BOOL fAliasLike = SfIsAlias(pwcName, cwcName);
    int rcFound = VERR_FILE_NOT_FOUND;
    int rc;

    pbBuf = (BYTE *)SfHeapAllocate(SF_NAMES_BUF_SIZE, 0);
    if (!pbBuf)
        return VERR_NO_MEMORY;
    SfMemZero(&Parms, sizeof(Parms));
    Parms.Handle      = SHFL_HANDLE_NIL;
    Parms.CreateFlags = SHFL_CF_DIRECTORY | SHFL_CF_ACT_OPEN_IF_EXISTS | SHFL_CF_ACT_FAIL_IF_NEW | SHFL_CF_ACCESS_READ;
    rc = SfHostCreate(idRoot, pDir, &Parms);
    if (rc >= 0 && Parms.Handle == SHFL_HANDLE_NIL)
        rc = VERR_PATH_NOT_FOUND;
    while (rc >= 0 && rcFound < 0)
    {
        ULONG cbBuf = SF_NAMES_BUF_SIZE, off = 0;
        UINT32 cEntries, i;
        rc = SfHostList(idRoot, Parms.Handle, SHFL_LIST_NONE, NULL, pbBuf, &cbBuf, &cEntries);
        if (rc < 0 || !cEntries)
            break;
        for (i = 0; i < cEntries && rcFound < 0; i++)
        {
            const SHFLDIRINFO *pEntry = (const SHFLDIRINFO *)(pbBuf + off);
            const WCHAR *pwcEntry = pEntry->name.String.utf16;
            ULONG cwcEntry = pEntry->name.u16Length / sizeof(WCHAR);
            char szShort[13];
            ULONG cchShort, j;
            BOOL fMatch = sfEqualNoCase(pwcEntry, cwcEntry, pwcName, cwcName);

            off += SHFLDIRINFO_NAME_OFFSET + pEntry->name.u16Size;
            if (cwcEntry > SHFL_MAX_LEN)
                continue;
            cchShort = (fShort || (!fMatch && (fAliasLike || cwcName <= 12))) ? SfGetShortName(pEntry, szShort, NULL) : 0;
            if (!fMatch && cchShort == cwcName)
            {
                for (j = 0; j < cchShort; j++)
                {
                    WCHAR wc = pwcName[j];
                    if (wc >= 'a' && wc <= 'z')
                        wc -= 'a' - 'A';
                    if (wc != (WCHAR)(BYTE)szShort[j])
                        break;
                }
                fMatch = j == cchShort;
            }
            if (!fMatch)
                continue;
            if (fShort)
            {
                for (j = 0; j < cchShort; j++)
                    pwszOut[j] = (WCHAR)(BYTE)szShort[j];
                *pcwcOut = cchShort;
            }
            else
            {
                SfMemCopy(pwszOut, pwcEntry, cwcEntry * sizeof(WCHAR));
                *pcwcOut = cwcEntry;
            }
            rcFound = VINF_SUCCESS;
        }
    }
    if (Parms.Handle != SHFL_HANDLE_NIL)
        SfHostClose(idRoot, Parms.Handle);
    SfHeapFree(pbBuf, 0);
    return rcFound >= 0 ? rcFound : rc < 0 && rc != VERR_NO_MORE_FILES ? rc : VERR_FILE_NOT_FOUND;
}

/**
 * Translates a guest path element by element, listing the host directories
 * where needed.
 *
 * @returns SHFLSTRING on the heap ("\A\B", "\" for the root), NULL if out of
 *          memory.  An element that isn't found is kept as it is (the host
 *          then reports what is missing).
 * @param   pRes        The shared folder.
 * @param   ppath       The path relative to the share.
 * @param   cSkipLast   Number of elements to leave out at the end.
 * @param   enmMode     SF_PATH_HOST (aliases to long names, the rest as it
 *                      is), SF_PATH_LONG (also every element in the host's
 *                      case) or SF_PATH_SHORT (every element as 8.3).
 */
SHFLSTRING *SfTranslatePath(SFRESOURCE *pRes, path_t ppath, ULONG cSkipLast, ULONG enmMode)
{
    char *pszEnd = (char *)ppath + ppath->pp_totalLength;
    ULONG cElements = 0, iElement, cwcMax;
    PathElement *pe;
    SHFLSTRING *pStr;
    WCHAR *pwszFound;

    for (pe = ppath->pp_elements; (char *)pe < pszEnd && pe->pe_length > sizeof(USHORT); pe = IFSNextElement(pe))
        cElements++;
    cElements -= cSkipLast < cElements ? cSkipLast : cElements;
    cwcMax = cElements * (SHFL_MAX_LEN + 1) + 2;

    pStr = (SHFLSTRING *)SfHeapAllocate(SHFLSTRING_HEADER_SIZE + cwcMax * sizeof(WCHAR), 0);
    pwszFound = (WCHAR *)SfHeapAllocate((SHFL_MAX_LEN + 1) * sizeof(WCHAR), 0);
    if (!pStr || !pwszFound)
    {
        if (pStr)
            SfHeapFree(pStr, 0);
        if (pwszFound)
            SfHeapFree(pwszFound, 0);
        return NULL;
    }
    pStr->u16Length = 0;
    pStr->u16Size   = (USHORT)(cwcMax * sizeof(WCHAR));
    pStr->String.utf16[0] = '\\';
    pStr->String.utf16[1] = 0;

    for (pe = ppath->pp_elements, iElement = 0; iElement < cElements; pe = IFSNextElement(pe), iElement++)
    {
        ULONG cwcDir = pStr->u16Length / sizeof(WCHAR);
        ULONG cwcElem = (pe->pe_length - sizeof(USHORT)) / sizeof(WCHAR);
        ULONG cwcFound = cwcElem;
        const WCHAR *pwcAdd = pe->pe_unichars;
        char szShort[13];

        if (   enmMode != SF_PATH_HOST
            || SfIsAlias(pe->pe_unichars, cwcElem))
        {
            /* The directory so far: "\" for the root. */
            if (!cwcDir)
                pStr->u16Length = sizeof(WCHAR);
            pStr->String.utf16[pStr->u16Length / sizeof(WCHAR)] = 0;
            if (   enmMode == SF_PATH_SHORT
                && SfTo83(pe->pe_unichars, cwcElem, szShort))
                ; /* Already 8.3: as it is. */
            else if (sfFindInDir(pRes->idRoot, pStr, pe->pe_unichars, cwcElem, enmMode == SF_PATH_SHORT,
                                 pwszFound, &cwcFound) >= 0)
                pwcAdd = pwszFound;
            else
                cwcFound = cwcElem;
            pStr->u16Length = (USHORT)(cwcDir * sizeof(WCHAR));
        }
        pStr->String.utf16[cwcDir] = '\\';
        SfMemCopy(&pStr->String.utf16[cwcDir + 1], pwcAdd, cwcFound * sizeof(WCHAR));
        pStr->u16Length = (USHORT)((cwcDir + 1 + cwcFound) * sizeof(WCHAR));
    }
    if (!pStr->u16Length)
        pStr->u16Length = sizeof(WCHAR);
    pStr->String.utf16[pStr->u16Length / sizeof(WCHAR)] = 0;
    pStr->u16Size = (USHORT)(pStr->u16Length + sizeof(WCHAR));
    SfHeapFree(pwszFound, 0);
    return pStr;
}

/**
 * Puts a host style path ("\A\B") into a parsed path buffer.
 */
void SfHostPathToParsed(const SHFLSTRING *pStr, path_t ppath)
{
    const WCHAR *pwc = pStr->String.utf16;
    ULONG cwc = pStr->u16Length / sizeof(WCHAR);
    PathElement *pe = ppath->pp_elements, *peLast = pe;
    ULONG i = 0;

    while (i < cwc)
    {
        ULONG cwcElem = 0;
        if (pwc[i] == '\\')
            i++;
        while (i + cwcElem < cwc && pwc[i + cwcElem] != '\\')
            cwcElem++;
        if (!cwcElem)
            break;
        SfMemCopy(pe->pe_unichars, &pwc[i], cwcElem * sizeof(WCHAR));
        pe->pe_length = (USHORT)(sizeof(USHORT) + cwcElem * sizeof(WCHAR));
        peLast = pe;
        pe = IFSNextElement(pe);
        i += cwcElem;
    }
    ppath->pp_totalLength  = (USHORT)((char *)pe - (char *)ppath);
    ppath->pp_prefixLength = (USHORT)((char *)peLast - (char *)ppath);
    pe->pe_length = 0;
}
