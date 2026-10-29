/** @file
 * VBoxSF - Directory searches: FS_FindFirstFile (Win32 and INT 21h 714Eh)
 * and FS_SearchFile (INT 21h 4Eh/4Fh and the FCB searches).
 *
 * Both list the whole directory from the host and match the names here,
 * with IFSMgr's own matcher for the Win32 searches and the FCB rules for the
 * DOS ones, so the wildcards behave as on a local drive.
 *
 * DOS searches are never closed and keep only a few bytes of state in the
 * caller's DTA, so they live in a small table: the DTA has the slot and its
 * generation, a search whose slot was reused ends.  DOS gets the 8.3 names
 * of Names.c.
 */
#include "VBoxSF.h"

#pragma VxD_LOCKED_CODE_SEG
#pragma VxD_LOCKED_DATA_SEG


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/

/** Size of the listing buffer of a search. */
#define SF_FIND_BUF_SIZE        (16 * 1024)

/** Number of DOS searches kept at a time. */
#define SF_MAX_DOS_SEARCHES     16

/** Attributes a search has to ask for to see such entries. */
#define SF_ATTR_SPECIAL         (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_DIRECTORY)


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/

/** A directory being listed. */
typedef struct SFFIND
{
    SHFLROOT    idRoot;
    SHFLHANDLE  hDir;
    /** Search and must-match attributes (ir_attr of the first call). */
    ULONG       fAttr;
    /** Leave out "." and ".." (the share's root). */
    BOOL        fRoot;
    /** The host said there are no more entries. */
    BOOL        fEnd;
    /** Entries left in the buffer, and where the next one is. */
    ULONG       cLeft;
    ULONG       offNext;
    BYTE       *pbBuf;
} SFFIND;

/** A Win32 search (the FSD find handle, ir_fh). */
typedef struct SFWIN32FIND
{
    SFFIND      Find;
    /** IFSMgr_MetaMatch semantics (UFLG_XXX). */
    int         fMatchSem;
    /** The pattern, upper case and zero terminated. */
    WCHAR       wszPattern[SHFL_MAX_LEN + 1];
} SFWIN32FIND;

/** A DOS search. */
typedef struct SFDOSSEARCH
{
    BOOL        fUsed;
    /** Changes whenever the slot is reused (kept in the DTA). */
    BYTE        bGeneration;
    SFFIND      Find;
    /** The pattern in FCB form. */
    char        achPattern[11];
} SFDOSSEARCH;


/*********************************************************************************************************************************
*   Internal Functions                                                                                                           *
*********************************************************************************************************************************/

static int __cdecl sfFindNext(pioreq pir);
static int __cdecl sfFindClose(pioreq pir);


/*********************************************************************************************************************************
*   Global Variables                                                                                                             *
*********************************************************************************************************************************/

/** The handle functions of Win32 searches. */
static hndlmisc g_FindMisc =
{
    IFS_VERSION, IFS_REVISION, NUM_HNDLMISC,
    {
        SfNetBadFunction,       /* HM_SEEK */
        sfFindClose,            /* HM_CLOSE */
        SfNetBadFunction,       /* HM_COMMIT */
        SfNetBadFunction,       /* HM_FILELOCKS */
        SfNetBadFunction,       /* HM_FILETIMES */
        SfNetBadFunction,       /* HM_PIPEREQUEST */
        SfNetBadFunction,       /* HM_HANDLEINFO */
        SfNetBadFunction,       /* HM_ENUMHANDLE */
    }
};

/** The DOS searches (under SF_LOCK). */
static SFDOSSEARCH g_aDosSearches[SF_MAX_DOS_SEARCHES];
/** Next slot to reuse when all are taken. */
static ULONG g_iDosSearchNext;


/*********************************************************************************************************************************
*   Listing                                                                                                                      *
*********************************************************************************************************************************/

/**
 * Opens the directory of a search pattern.
 *
 * @returns Win32 error code.
 */
static int sfFindStart(SFFIND *pFind, SFRESOURCE *pRes, path_t ppath, ULONG fAttr)
{
    SHFLCREATEPARMS Parms;
    SHFLSTRING *pDir = SfPathToHost(pRes, ppath, 1);
    int rc;

    SfMemZero(pFind, sizeof(*pFind));
    pFind->hDir = SHFL_HANDLE_NIL;
    if (!pDir)
        return SF_ERROR_NOT_ENOUGH_MEMORY;
    pFind->idRoot = pRes->idRoot;
    pFind->fAttr  = fAttr;
    pFind->fRoot  = pDir->u16Length == sizeof(WCHAR);
    pFind->pbBuf  = (BYTE *)SfHeapAllocate(SF_FIND_BUF_SIZE, 0);
    if (!pFind->pbBuf)
    {
        SfHeapFree(pDir, 0);
        return SF_ERROR_NOT_ENOUGH_MEMORY;
    }

    SfMemZero(&Parms, sizeof(Parms));
    Parms.Handle      = SHFL_HANDLE_NIL;
    Parms.CreateFlags = SHFL_CF_DIRECTORY | SHFL_CF_ACT_OPEN_IF_EXISTS | SHFL_CF_ACT_FAIL_IF_NEW | SHFL_CF_ACCESS_READ;
    rc = SfHostCreate(pFind->idRoot, pDir, &Parms);
    SfHeapFree(pDir, 0);
    if (rc >= 0 && Parms.Handle == SHFL_HANDLE_NIL)
        rc = VERR_PATH_NOT_FOUND;
    if (rc < 0)
    {
        SfHeapFree(pFind->pbBuf, 0);
        pFind->pbBuf = NULL;
        return rc == VERR_FILE_NOT_FOUND ? SF_ERROR_PATH_NOT_FOUND : SfRcToWin32(rc);
    }
    pFind->hDir = Parms.Handle;
    return 0;
}

static void sfFindEnd(SFFIND *pFind)
{
    if (pFind->hDir != SHFL_HANDLE_NIL)
        SfHostClose(pFind->idRoot, pFind->hDir);
    pFind->hDir = SHFL_HANDLE_NIL;
    if (pFind->pbBuf)
        SfHeapFree(pFind->pbBuf, 0);
    pFind->pbBuf = NULL;
}

/**
 * Gets the next directory entry the search attributes let through.
 *
 * @returns The entry (in pFind's buffer), NULL at the end or on failure.
 */
static SHFLDIRINFO *sfFindNextEntry(SFFIND *pFind)
{
    for (;;)
    {
        SHFLDIRINFO *pEntry;
        ULONG fEntryAttr;

        if (!pFind->cLeft)
        {
            ULONG cbBuf = SF_FIND_BUF_SIZE;
            UINT32 cEntries;
            int rc;

            if (pFind->fEnd || !pFind->pbBuf)
                return NULL;
            rc = SfHostList(pFind->idRoot, pFind->hDir, SHFL_LIST_NONE, NULL, pFind->pbBuf, &cbBuf, &cEntries);
            if (rc < 0 || !cEntries)
            {
                pFind->fEnd = TRUE;
                return NULL;
            }
            pFind->cLeft   = cEntries;
            pFind->offNext = 0;
        }

        pEntry = (SHFLDIRINFO *)(pFind->pbBuf + pFind->offNext);
        pFind->offNext += SHFLDIRINFO_NAME_OFFSET + pEntry->name.u16Size;
        pFind->cLeft--;

        if (   pFind->fRoot
            && pEntry->name.String.utf16[0] == '.'
            && (   pEntry->name.u16Length == sizeof(WCHAR)
                || (pEntry->name.u16Length == 2 * sizeof(WCHAR) && pEntry->name.String.utf16[1] == '.')))
            continue;
        fEntryAttr = SF_DOS_ATTR(&pEntry->Info);
        if (fEntryAttr & SF_ATTR_SPECIAL & ~pFind->fAttr)
            continue;
        if (((pFind->fAttr & (fEntryAttr << 8)) ^ pFind->fAttr) & FILE_ATTRIBUTE_MUSTMATCH)
            continue;
        return pEntry;
    }
}

/**
 * Puts an 8.3 name ("NAME.EXT") or a pattern in FCB form (11 characters,
 * blank padded, '*' filled with '?').
 */
static void sfToFcbName(const char *pszName, char *pachFcb)
{
    ULONG i = 0;

    for (; i < 11; i++)
        pachFcb[i] = ' ';
    for (i = 0; *pszName && *pszName != '.'; pszName++)
        if (*pszName == '*')
            for (; i < 8; i++)
                pachFcb[i] = '?';
        else if (i < 8)
            pachFcb[i++] = *pszName;
    if (*pszName == '.')
        for (pszName++, i = 8; *pszName; pszName++)
            if (*pszName == '*')
                for (; i < 11; i++)
                    pachFcb[i] = '?';
            else if (i < 11)
                pachFcb[i++] = *pszName;
}


/*********************************************************************************************************************************
*   Win32 searches                                                                                                               *
*********************************************************************************************************************************/

/**
 * Finds the next entry matching a Win32 search and fills in the find data.
 *
 * @returns Win32 error code (ERROR_NO_MORE_FILES at the end).
 */
static int sfWin32FindFill(SFWIN32FIND *pWin32Find, _WIN32_FIND_DATA *pFindData)
{
    WCHAR wszUpper[SHFL_MAX_LEN + 1];
    SHFLDIRINFO *pEntry;

    while ((pEntry = sfFindNextEntry(&pWin32Find->Find)) != NULL)
    {
        ULONG cwc = pEntry->name.u16Length / sizeof(WCHAR);
        char szShort[13];
        ULONG cchShort, i;
        BOOL fMatch, fMade, fAlt;

        if (cwc > SHFL_MAX_LEN)
            continue;
        SfIfsUniToUpper(wszUpper, pEntry->name.String.utf16, cwc * sizeof(WCHAR));
        wszUpper[cwc] = 0;
        fMatch = SfIfsMetaMatch(pWin32Find->wszPattern, wszUpper, pWin32Find->fMatchSem) != 0;
        cchShort = SfGetShortName(pEntry, szShort, &fMade);
        fAlt = fMade || (pEntry->cucShortName && pEntry->cucShortName < 14);
        if (!fMatch && fAlt)
        {
            for (i = 0; i <= cchShort; i++)
                wszUpper[i] = (WCHAR)(BYTE)szShort[i];
            fMatch = SfIfsMetaMatch(pWin32Find->wszPattern, wszUpper, pWin32Find->fMatchSem) != 0;
        }
        if (!fMatch)
            continue;

        pFindData->dwFileAttributes = SF_DOS_ATTR(&pEntry->Info);
        SfTimeToFileTime(pEntry->Info.BirthTime, &pFindData->ftCreationTime);
        SfTimeToFileTime(pEntry->Info.AccessTime, &pFindData->ftLastAccessTime);
        SfTimeToFileTime(pEntry->Info.ModificationTime, &pFindData->ftLastWriteTime);
        pFindData->nFileSizeHigh = SF_HI32(pEntry->Info.cbObject);
        pFindData->nFileSizeLow  = (ULONG)pEntry->Info.cbObject;
        pFindData->dwReserved0   = 0;
        pFindData->dwReserved1   = 0;
        if (cwc > MAX_PATH - 1)
            cwc = MAX_PATH - 1;
        SfMemCopy(pFindData->cFileName, pEntry->name.String.utf16, cwc * sizeof(WCHAR));
        pFindData->cFileName[cwc] = 0;
        /* The 8.3 name when the long name isn't one: the host's or ours. */
        cwc = fAlt ? cchShort : 0;
        for (i = 0; i < cwc; i++)
            pFindData->cAlternateFileName[i] = (WCHAR)(BYTE)szShort[i];
        pFindData->cAlternateFileName[cwc] = 0;
        return 0;
    }
    return SF_ERROR_NO_MORE_FILES;
}

/**
 * FS_FindFirstFile.
 */
int __cdecl SfFindOpen(pioreq pir)
{
    SFRESOURCE *pRes = (SFRESOURCE *)pir->ir_rh;
    PathElement *pePattern = IFSLastElement(pir->ir_ppath);
    SFWIN32FIND *pWin32Find;
    ULONG cwcPattern;
    int rcWin32;

    /* No volume labels on a share (GetVolumeInformation looks for one). */
    if (   IFSIsRoot(pir->ir_ppath)
        || (pir->ir_attr & 0xff) == FILE_ATTRIBUTE_LABEL
        || (pir->ir_attr & (FILE_ATTRIBUTE_LABEL << 8)))
    {
        pir->ir_error = SF_ERROR_FILE_NOT_FOUND;
        return pir->ir_error;
    }
    cwcPattern = (pePattern->pe_length - sizeof(USHORT)) / sizeof(WCHAR);
    if (cwcPattern > SHFL_MAX_LEN)
    {
        pir->ir_error = SF_ERROR_INVALID_NAME;
        return pir->ir_error;
    }
    pWin32Find = (SFWIN32FIND *)SfHeapAllocate(sizeof(*pWin32Find), HEAPZEROINIT);
    if (!pWin32Find)
    {
        pir->ir_error = SF_ERROR_NOT_ENOUGH_MEMORY;
        return pir->ir_error;
    }
    SfIfsUniToUpper(pWin32Find->wszPattern, pePattern->pe_unichars, cwcPattern * sizeof(WCHAR));
    pWin32Find->wszPattern[cwcPattern] = 0;
    pWin32Find->fMatchSem = pir->ir_attr & (FILE_FLAG_KEEP_CASE | FILE_FLAG_IS_LFN) ? UFLG_NT : UFLG_DOS;
    if (pir->ir_attr & FILE_FLAG_WILDCARDS)
        pWin32Find->fMatchSem |= UFLG_META;

    rcWin32 = sfFindStart(&pWin32Find->Find, pRes, pir->ir_ppath, pir->ir_attr & 0xffff);
    if (!rcWin32)
        rcWin32 = sfWin32FindFill(pWin32Find, (_WIN32_FIND_DATA *)pir->ir_data);
    if (rcWin32)
    {
        sfFindEnd(&pWin32Find->Find);
        SfHeapFree(pWin32Find, 0);
        pir->ir_error = rcWin32 == SF_ERROR_NO_MORE_FILES ? SF_ERROR_FILE_NOT_FOUND : rcWin32;
        return pir->ir_error;
    }

    pir->ir_fh              = pWin32Find;
    pir->ir_hfunc->hf_read  = sfFindNext;
    pir->ir_hfunc->hf_write = SfNetBadFunction;
    pir->ir_hfunc->hf_misc  = &g_FindMisc;
    pir->ir_pos             = 0;
    pir->ir_error           = 0;
    return 0;
}

/**
 * FS_FindNextFile.
 */
static int __cdecl sfFindNext(pioreq pir)
{
    SFWIN32FIND *pWin32Find = (SFWIN32FIND *)pir->ir_fh;

    pir->ir_error = sfWin32FindFill(pWin32Find, (_WIN32_FIND_DATA *)pir->ir_data);
    pir->ir_pos   = 0;
    return pir->ir_error;
}

/**
 * FS_FindClose.
 */
static int __cdecl sfFindClose(pioreq pir)
{
    SFWIN32FIND *pWin32Find = (SFWIN32FIND *)pir->ir_fh;

    sfFindEnd(&pWin32Find->Find);
    SfHeapFree(pWin32Find, 0);
    pir->ir_error = 0;
    return 0;
}


/*********************************************************************************************************************************
*   DOS searches                                                                                                                 *
*********************************************************************************************************************************/

/**
 * Finds the next entry matching a DOS search and fills in the DTA part.
 *
 * @returns Win32 error code (ERROR_NO_MORE_FILES at the end).
 */
static int sfDosSearchFill(SFDOSSEARCH *pSearch, srch_entry *pEntryOut)
{
    SHFLDIRINFO *pEntry;

    while ((pEntry = sfFindNextEntry(&pSearch->Find)) != NULL)
    {
        char szName[13];
        char achFcb[11];
        dos_time DosTime;
        ULONG i;

        SfGetShortName(pEntry, szName, NULL);
        sfToFcbName(szName, achFcb);
        for (i = 0; i < 11; i++)
            if (pSearch->achPattern[i] != '?' && pSearch->achPattern[i] != achFcb[i])
                break;
        if (i < 11)
            continue;

        DosTime = SfTimeToDosTime(pEntry->Info.ModificationTime);
        pEntryOut->se_attrib = SF_DOS_ATTR(&pEntry->Info);
        pEntryOut->se_time   = DosTime.dt_time;
        pEntryOut->se_date   = DosTime.dt_date;
        pEntryOut->se_size   = (ULONG)pEntry->Info.cbObject;
        SfMemCopy(pEntryOut->se_name, szName, sizeof(szName));
        return 0;
    }
    return SF_ERROR_NO_MORE_FILES;
}

/**
 * FS_SearchFile.
 */
int __cdecl SfSearch(pioreq pir)
{
    SFRESOURCE *pRes = (SFRESOURCE *)pir->ir_rh;
    srch_entry *pEntryOut = (srch_entry *)pir->ir_data;
    SFDOSSEARCH *pSearch;
    ULONG iSlot;
    int rcWin32;

    SF_LOCK();
    if (pir->ir_flags == SEARCH_FIRST)
    {
        PathElement *pePattern = IFSLastElement(pir->ir_ppath);
        char szPattern[13];
        ULONG cwcPattern = (pePattern->pe_length - sizeof(USHORT)) / sizeof(WCHAR);
        ULONG fAttr = pir->ir_attr & 0xff;
        ULONG i;

        /* No volume labels on a share. */
        if (fAttr == FILE_ATTRIBUTE_LABEL || IFSIsRoot(pir->ir_ppath) || cwcPattern > 12)
        {
            SF_UNLOCK();
            pir->ir_error = SF_ERROR_NO_MORE_FILES;
            return pir->ir_error;
        }
        for (i = 0; i < cwcPattern; i++)
        {
            WCHAR wc = pePattern->pe_unichars[i];
            if (wc >= 'a' && wc <= 'z')
                wc -= 'a' - 'A';
            szPattern[i] = wc < 0x80 ? (char)wc : '_';
        }
        szPattern[i] = '\0';

        for (iSlot = 0; iSlot < SF_MAX_DOS_SEARCHES && g_aDosSearches[iSlot].fUsed; iSlot++)
            ;
        if (iSlot == SF_MAX_DOS_SEARCHES)
        {
            iSlot = g_iDosSearchNext++ % SF_MAX_DOS_SEARCHES;
            sfFindEnd(&g_aDosSearches[iSlot].Find);
        }
        pSearch = &g_aDosSearches[iSlot];
        pSearch->fUsed = TRUE;
        pSearch->bGeneration++;
        sfToFcbName(szPattern, pSearch->achPattern);
        rcWin32 = sfFindStart(&pSearch->Find, pRes, pir->ir_ppath, fAttr);
        if (rcWin32)
        {
            pSearch->fUsed = FALSE;
            SF_UNLOCK();
            pir->ir_error = rcWin32;
            return pir->ir_error;
        }
        SfMemCopy(pEntryOut->se_key.sk_pattern, pSearch->achPattern, sizeof(pSearch->achPattern));
        pEntryOut->se_key.sk_attr      = (unsigned char)fAttr;
        pEntryOut->se_key.sk_netFSD[0] = (unsigned char)iSlot;
        pEntryOut->se_key.sk_netFSD[1] = pSearch->bGeneration;
    }
    else
    {
        iSlot = pEntryOut->se_key.sk_netFSD[0];
        if (   iSlot >= SF_MAX_DOS_SEARCHES
            || !g_aDosSearches[iSlot].fUsed
            || g_aDosSearches[iSlot].bGeneration != pEntryOut->se_key.sk_netFSD[1])
        {
            SF_UNLOCK();
            pir->ir_error = SF_ERROR_NO_MORE_FILES;
            return pir->ir_error;
        }
        pSearch = &g_aDosSearches[iSlot];
    }

    rcWin32 = sfDosSearchFill(pSearch, pEntryOut);
    if (rcWin32)
    {
        sfFindEnd(&pSearch->Find);
        pSearch->fUsed = FALSE;
    }
    SF_UNLOCK();
    pir->ir_firstclus = 0;
    pir->ir_error = rcWin32;
    return pir->ir_error;
}
