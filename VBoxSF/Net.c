/** @file
 * VBoxSF - The IFSMgr network file system driver: connections and the
 * volume (path based) functions.
 *
 * IFSMgr calls SfNetConnect for every connection to a network resource
 * (NET USE, INT 21h 5F03h, a UNC path); we take \\VBOXSVR\<share> and
 * \\VBOXSRV\<share>, map the shared folder and hand back the volume
 * functions.  One resource per shared folder: UNC requests connect again on
 * every access, and IFSMgr keeps a single resource of its own when it gets
 * back the one it handed in first (ir_fh).  It counts the uses and
 * disconnects once, when the folder is unmapped.
 *
 * The paths IFSMgr passes after the connection are relative to the share
 * (ir_pathSkip = 2) and upper case; the host is asked with them as they are,
 * except that names being created (files, directories, rename targets) get
 * their last element in the case the program gave (ir_uFName).
 */
#include "VBoxSF.h"

#pragma VxD_LOCKED_CODE_SEG
#pragma VxD_LOCKED_DATA_SEG


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/

/** Number of characters in a parsed path element. */
#define SF_PE_CWC(pe)           (((pe)->pe_length - sizeof(USHORT)) / sizeof(WCHAR))

/** Server name the share names are given back with (FS_QueryResourceInfo). */
#define SF_SERVER_NAME          "\\VBOXSVR"

/** File system name for GetVolumeInformation.  IFSMgr offers 8 bytes, too
 *  few for the official additions' "VBoxSharedFolderFS". */
#define SF_FS_NAME              "VBoxSF"

/** Logs a call, with its path or without (where ir_ppath isn't one);
 *  development aid. */
#ifdef VBOXSF_LOG_CALLS
# define SF_LOG_CALL(pszFunction, pir)          sfLogCall(pszFunction, pir, TRUE)
# define SF_LOG_CALL_NO_PATH(pszFunction, pir)  sfLogCall(pszFunction, pir, FALSE)
#else
# define SF_LOG_CALL(pszFunction, pir)          do { } while (0)
# define SF_LOG_CALL_NO_PATH(pszFunction, pir)  do { } while (0)
#endif


/*********************************************************************************************************************************
*   Internal Functions                                                                                                           *
*********************************************************************************************************************************/

static int __cdecl sfNetDelete(pioreq pir);
static int __cdecl sfNetDir(pioreq pir);
static int __cdecl sfNetFileAttrib(pioreq pir);
static int __cdecl sfNetFlush(pioreq pir);
static int __cdecl sfNetGetDiskInfo(pioreq pir);
static int __cdecl sfNetRename(pioreq pir);
static int __cdecl sfNetQuery(pioreq pir);
static int __cdecl sfNetDisconnect(pioreq pir);


/*********************************************************************************************************************************
*   Global Variables                                                                                                             *
*********************************************************************************************************************************/

/** The connected shared folders. */
static SFRESOURCE *g_pResources;

/** The volume functions of every resource. */
static volfunc g_VolFuncs =
{
    IFS_VERSION, IFS_REVISION, NUM_VOLFUNC,
    {
        sfNetDelete,            /* VFN_DELETE */
        sfNetDir,               /* VFN_DIR */
        sfNetFileAttrib,        /* VFN_FILEATTRIB */
        sfNetFlush,             /* VFN_FLUSH */
        sfNetGetDiskInfo,       /* VFN_GETDISKINFO */
        SfFileOpen,             /* VFN_OPEN */
        sfNetRename,            /* VFN_RENAME */
        SfSearch,               /* VFN_SEARCH */
        sfNetQuery,             /* VFN_QUERY */
        sfNetDisconnect,        /* VFN_DISCONNECT */
        SfNetBadFunction,       /* VFN_UNCPIPEREQ */
        SfNetBadFunction,       /* VFN_IOCTL16DRIVE */
        SfNetBadFunction,       /* VFN_GETDISKPARMS */
        SfFindOpen,             /* VFN_FINDOPEN */
        SfNetBadFunction,       /* VFN_DASDIO */
    }
};


/*********************************************************************************************************************************
*   Path helpers                                                                                                                 *
*********************************************************************************************************************************/

/**
 * Writes a parsed path as ASCII ('?' for the rest) for the log.
 */
static void sfPathToLog(path_t ppath, char *pszBuf, ULONG cbBuf)
{
    PathElement *pe = ppath->pp_elements;
    char *pszEnd = (char *)ppath + ppath->pp_totalLength;
    ULONG off = 0;

    while ((char *)pe < pszEnd && pe->pe_length > sizeof(USHORT) && off + 2 < cbBuf)
    {
        ULONG i;
        pszBuf[off++] = '\\';
        for (i = 0; i < SF_PE_CWC(pe) && off + 1 < cbBuf; i++)
        {
            WCHAR wc = pe->pe_unichars[i];
            pszBuf[off++] = wc >= 0x20 && wc < 0x7f ? (char)wc : '?';
        }
        pe = IFSNextElement(pe);
    }
    if (!off)
        pszBuf[off++] = '\\';
    pszBuf[off] = '\0';
}

#ifdef VBOXSF_LOG_CALLS
static void sfLogCall(const char *pszFunction, pioreq pir, BOOL fPath)
{
    char szPath[80];
    if (fPath && pir->ir_ppath)
        sfPathToLog(pir->ir_ppath, szPath, sizeof(szPath));
    else
        szPath[0] = '\0';
    SfLogRel("%s '%s' flags %#x options %#x attr %#x length %u data %p\n", pszFunction, szPath, pir->ir_flags,
             pir->ir_options, pir->ir_attr, pir->ir_length, pir->ir_data);
}
#endif

/**
 * Builds the host path of a parsed path: "\A\B", "\" for the root.
 *
 * @returns SHFLSTRING on the heap (SfHeapFree), NULL if out of memory.
 * Elements with the shape of a generated 8.3 alias are looked up on the host
 * (SfTranslatePath).
 *
 * @param   pRes        The shared folder.
 * @param   ppath       The path relative to the share.
 * @param   cSkipLast   Number of elements to leave out at the end (1 for
 *                      the directory of a search pattern).
 */
SHFLSTRING *SfPathToHost(SFRESOURCE *pRes, path_t ppath, ULONG cSkipLast)
{
    char *pszEnd = (char *)ppath + ppath->pp_totalLength;
    PathElement *pe;
    SHFLSTRING *pStr;
    ULONG cElements = 0;
    ULONG cwc = 0;
    ULONG iElement;
    BOOL fAlias = FALSE;
    WCHAR *pwc;

    for (pe = ppath->pp_elements; (char *)pe < pszEnd && pe->pe_length > sizeof(USHORT); pe = IFSNextElement(pe))
    {
        cElements++;
        cwc += 1 + SF_PE_CWC(pe);
    }
    if (cSkipLast > cElements)
        cSkipLast = cElements;
    cElements -= cSkipLast;
    if (!cElements)
        cwc = 1;
    for (pe = ppath->pp_elements, iElement = 0; iElement < cElements; pe = IFSNextElement(pe), iElement++)
        if (SfIsAlias(pe->pe_unichars, SF_PE_CWC(pe)))
            fAlias = TRUE;
    if (fAlias)
        return SfTranslatePath(pRes, ppath, cSkipLast, SF_PATH_HOST);

    pStr = (SHFLSTRING *)SfHeapAllocate(SHFLSTRING_HEADER_SIZE + (cwc + 1) * sizeof(WCHAR), 0);
    if (!pStr)
        return NULL;
    pwc = pStr->String.utf16;
    for (pe = ppath->pp_elements, iElement = 0; iElement < cElements; pe = IFSNextElement(pe), iElement++)
    {
        *pwc++ = '\\';
        SfMemCopy(pwc, pe->pe_unichars, SF_PE_CWC(pe) * sizeof(WCHAR));
        pwc += SF_PE_CWC(pe);
    }
    if (!cElements)
        *pwc++ = '\\';
    *pwc = 0;
    pStr->u16Length = (USHORT)((pwc - pStr->String.utf16) * sizeof(WCHAR));
    pStr->u16Size   = (USHORT)(pStr->u16Length + sizeof(WCHAR));
    return pStr;
}

/**
 * Builds the host path with the last element in the case the program gave
 * (ir_uFName), for names that are created: IFSMgr's parsed paths are upper
 * case.  The earlier elements exist already, their case doesn't matter.
 *
 * @returns SHFLSTRING on the heap (SfHeapFree), NULL if out of memory.
 * @param   pRes            The shared folder.
 * @param   ppath           The path relative to the share.
 * @param   fAttr           ir_attr (or ir_attr2) with the path parsing flags.
 * @param   pwszCaseName    ir_uFName, may be NULL.
 */
SHFLSTRING *SfPathToHostKeepCase(SFRESOURCE *pRes, path_t ppath, ULONG fAttr, string_t pwszCaseName)
{
    SHFLSTRING *pStr = SfPathToHost(pRes, ppath, 0);
    ULONG cwcCase = 0;

    if (   !pStr || !(fAttr & FILE_FLAG_KEEP_CASE) || !pwszCaseName || IFSIsRoot(ppath)
        || SfIsAlias(IFSLastElement(ppath)->pe_unichars, SF_PE_CWC(IFSLastElement(ppath))))
        return pStr;
    while (pwszCaseName[cwcCase])
        cwcCase++;
    if (cwcCase && cwcCase == SF_PE_CWC(IFSLastElement(ppath)))
        SfMemCopy(pStr->String.utf16 + pStr->u16Length / sizeof(WCHAR) - cwcCase, pwszCaseName,
                  cwcCase * sizeof(WCHAR));
    return pStr;
}

/**
 * Looks up an object.
 *
 * @returns VBox status code: VERR_FILE_NOT_FOUND or VERR_PATH_NOT_FOUND if
 *          it isn't there.
 */
int SfLookup(SFRESOURCE *pRes, path_t ppath, SHFLFSOBJINFO *pInfo)
{
    SHFLCREATEPARMS Parms;
    SHFLSTRING *pPath = SfPathToHost(pRes, ppath, 0);
    int rc;

    if (!pPath)
        return VERR_NO_MEMORY;
    SfMemZero(&Parms, sizeof(Parms));
    Parms.Handle      = SHFL_HANDLE_NIL;
    Parms.CreateFlags = SHFL_CF_LOOKUP | SHFL_CF_ACT_FAIL_IF_NEW;
    rc = SfHostCreate(pRes->idRoot, pPath, &Parms);
    SfHeapFree(pPath, 0);
    if (rc < 0)
        return rc;
    if (Parms.Result == SHFL_PATH_NOT_FOUND)
        return VERR_PATH_NOT_FOUND;
    if (Parms.Result != SHFL_FILE_EXISTS)
        return VERR_FILE_NOT_FOUND;
    if (Parms.Handle != SHFL_HANDLE_NIL)    /* Shouldn't happen with SHFL_CF_LOOKUP. */
        SfHostClose(pRes->idRoot, Parms.Handle);
    SfMemCopy(pInfo, &Parms.Info, sizeof(*pInfo));
    return VINF_SUCCESS;
}

/**
 * Sets the times that aren't 0 and the attributes if fMode isn't 0
 * (SHFL_INFO_SET | SHFL_INFO_FILE on an open handle).
 */
int SfSetInfo(SHFLROOT idRoot, SHFLHANDLE hHost, const SHFLFSOBJINFO *pInfo)
{
    SHFLFSOBJINFO Info;
    ULONG cbInfo = sizeof(Info);
    SfMemCopy(&Info, pInfo, sizeof(Info));
    return SfHostInformation(idRoot, hHost, SHFL_INFO_SET | SHFL_INFO_FILE, &Info, &cbInfo);
}

/**
 * Sets the attributes or times of an object by path.
 */
static int sfSetPathInfo(SFRESOURCE *pRes, path_t ppath, const SHFLFSOBJINFO *pInfo)
{
    SHFLCREATEPARMS Parms;
    SHFLSTRING *pPath = SfPathToHost(pRes, ppath, 0);
    int rc;

    if (!pPath)
        return VERR_NO_MEMORY;
    SfMemZero(&Parms, sizeof(Parms));
    Parms.Handle      = SHFL_HANDLE_NIL;
    Parms.CreateFlags = SHFL_CF_ACT_OPEN_IF_EXISTS | SHFL_CF_ACT_FAIL_IF_NEW | SHFL_CF_ACCESS_ATTR_WRITE;
    rc = SfHostCreate(pRes->idRoot, pPath, &Parms);
    SfHeapFree(pPath, 0);
    if (rc >= 0 && Parms.Handle == SHFL_HANDLE_NIL)
        rc = Parms.Result == SHFL_PATH_NOT_FOUND ? VERR_PATH_NOT_FOUND : VERR_FILE_NOT_FOUND;
    if (rc < 0)
        return rc;
    rc = SfSetInfo(pRes->idRoot, Parms.Handle, pInfo);
    SfHostClose(pRes->idRoot, Parms.Handle);
    return rc;
}

/**
 * Checks the server element of a UNC path, which IFSMgr gives with the
 * second leading backslash: "\VBOXSVR".
 */
static BOOL sfIsServerName(PathElement *pe)
{
    static const char * const s_apszNames[] = { "VBOXSVR", "VBOXSRV" };
    const WCHAR *pwc = pe->pe_unichars;
    ULONG cwc = SF_PE_CWC(pe);
    ULONG iName;

    if (cwc && *pwc == '\\')
    {
        pwc++;
        cwc--;
    }
    if (cwc != 7)
        return FALSE;
    for (iName = 0; iName < sizeof(s_apszNames) / sizeof(s_apszNames[0]); iName++)
    {
        const char *pszName = s_apszNames[iName];
        ULONG i;
        for (i = 0; i < 7; i++)
        {
            WCHAR wc = pwc[i];
            if (wc >= 'a' && wc <= 'z')
                wc -= 'a' - 'A';
            if (wc != (WCHAR)pszName[i])
                break;
        }
        if (i == 7)
            return TRUE;
    }
    return FALSE;
}

/** Called with the lock held. */
static SFRESOURCE *sfFindResource(const WCHAR *pwszName, ULONG cwcName)
{
    SFRESOURCE *pRes;
    for (pRes = g_pResources; pRes; pRes = pRes->pNext)
    {
        ULONG i;
        if (pRes->cwcName != cwcName)
            continue;
        for (i = 0; i < cwcName && pRes->wszName[i] == pwszName[i]; i++)
            ;
        if (i == cwcName)
            return pRes;
    }
    return NULL;
}


/*********************************************************************************************************************************
*   Connections                                                                                                                  *
*********************************************************************************************************************************/

/**
 * FS_ConnectNetResource.
 */
int __cdecl SfNetConnect(pioreq pir)
{
    path_t ppath = pir->ir_ppath;
    PathElement *peServer = ppath->pp_elements;
    PathElement *peShare;
    SFRESOURCE *pRes;
    SHFLROOT idRoot;
    ULONG cwcShare;
    char szPath[80];
    int rc;

    sfPathToLog(ppath, szPath, sizeof(szPath));

    /* \\VBOXSVR\share: two elements at least, the server first. */
    if (   IFSIsRoot(ppath)
        || !sfIsServerName(peServer)
        || (char *)(peShare = IFSNextElement(peServer)) >= (char *)ppath + ppath->pp_totalLength)
    {
        pir->ir_error = SF_ERROR_BAD_NETPATH;
        return pir->ir_error;
    }
    if (pir->ir_flags != RESTYPE_DISK && pir->ir_flags != RESTYPE_WILD)
    {
        pir->ir_error = SF_ERROR_BAD_NET_NAME;
        return pir->ir_error;
    }
    cwcShare = SF_PE_CWC(peShare);
    if (!cwcShare || cwcShare > SHFL_MAX_LEN)
    {
        pir->ir_error = SF_ERROR_BAD_NET_NAME;
        return pir->ir_error;
    }

    SF_LOCK();
    pRes = sfFindResource(peShare->pe_unichars, cwcShare);
    if (!pRes)
    {
        if (pir->ir_options & RESOPT_NO_CREATE)
        {
            SF_UNLOCK();
            pir->ir_error = SF_ERROR_BAD_NET_NAME;
            return pir->ir_error;
        }
        rc = SfHostConnect(FALSE);
        if (rc >= 0)
            rc = SfHostMapFolder(peShare->pe_unichars, cwcShare, &idRoot);
        if (rc < 0)
        {
            SF_UNLOCK();
            SfLogRel("connect '%s': %d\n", szPath, rc);
            pir->ir_error = rc == VERR_FILE_NOT_FOUND ? SF_ERROR_BAD_NET_NAME : SF_ERROR_UNEXP_NET_ERR;
            return pir->ir_error;
        }
        pRes = (SFRESOURCE *)SfHeapAllocate(sizeof(*pRes), HEAPZEROINIT);
        if (!pRes)
        {
            SfHostUnmapFolder(idRoot);
            SF_UNLOCK();
            pir->ir_error = SF_ERROR_NOT_ENOUGH_MEMORY;
            return pir->ir_error;
        }
        pRes->idRoot   = idRoot;
        pRes->cwcName  = cwcShare;
        SfMemCopy(pRes->wszName, peShare->pe_unichars, cwcShare * sizeof(WCHAR));
        pRes->pvIfsRes = pir->ir_fh;
        pRes->pNext    = g_pResources;
        g_pResources   = pRes;
        SfLogRel("connected '%s', root %u\n", szPath, idRoot);
    }
    else
        pir->ir_fh = pRes->pvIfsRes;    /* Tell IFSMgr it's the resource it already has. */
    SF_UNLOCK();

    pir->ir_rh       = pRes;
    pir->ir_pathSkip = 2;
    pir->ir_vfunc    = &g_VolFuncs;
    if (pir->ir_flags == RESTYPE_WILD)
        pir->ir_flags = RESTYPE_DISK;
    pir->ir_error    = 0;
    return 0;
}

/**
 * FS_DisconnectResource.
 */
static int __cdecl sfNetDisconnect(pioreq pir)
{
    SFRESOURCE *pRes = (SFRESOURCE *)pir->ir_rh;
    SFRESOURCE **ppPrev;

    SF_LOG_CALL_NO_PATH("Disconnect", pir);
    SF_LOCK();
    for (ppPrev = &g_pResources; *ppPrev; ppPrev = &(*ppPrev)->pNext)
        if (*ppPrev == pRes)
        {
            *ppPrev = pRes->pNext;
            break;
        }
    SF_UNLOCK();
    SfHostUnmapFolder(pRes->idRoot);
    SfHeapFree(pRes, 0);
    pir->ir_error = 0;
    return 0;
}

/**
 * FS_QueryResourceInfo: levels 0 and 1 give the resource name back
 * (\\VBOXSVR\share as a parsed path), level 2 the file system.
 */
static int __cdecl sfNetQuery(pioreq pir)
{
    SFRESOURCE *pRes = (SFRESOURCE *)pir->ir_rh;

    SF_LOG_CALL_NO_PATH("Query", pir);
    if ((pir->ir_options == 0 || pir->ir_options == 1) && !pir->ir_ppath)
    {
        /* IFSMgr asks this way from a timer, a couple of minutes after a
           UNC connection was last used; on an error it asks again forever. */
        if (pir->ir_options == 1)
        {
            pir->ir_options = RESSTAT_OK;
            pir->ir_flags   = RESTYPE_DISK;
        }
        pir->ir_error = 0;
        return 0;
    }
    if (pir->ir_options == 0 || pir->ir_options == 1)
    {
        static const char s_szServer[] = SF_SERVER_NAME;
        path_t ppath = pir->ir_ppath;
        PathElement *pe = ppath->pp_elements;
        ULONG i;

        pe->pe_length = (USHORT)(sizeof(USHORT) + (sizeof(s_szServer) - 1) * sizeof(WCHAR));
        for (i = 0; i < sizeof(s_szServer) - 1; i++)
            pe->pe_unichars[i] = s_szServer[i];
        pe = IFSNextElement(pe);
        pe->pe_length = (USHORT)(sizeof(USHORT) + pRes->cwcName * sizeof(WCHAR));
        SfMemCopy(pe->pe_unichars, pRes->wszName, pRes->cwcName * sizeof(WCHAR));
        ppath->pp_prefixLength = (USHORT)((char *)pe - (char *)ppath);
        pe = IFSNextElement(pe);
        ppath->pp_totalLength  = (USHORT)((char *)pe - (char *)ppath);
        pe->pe_length = 0;
        if (pir->ir_options == 1)
        {
            pir->ir_options = RESSTAT_OK;
            pir->ir_flags   = RESTYPE_DISK;
        }
        pir->ir_error = 0;
        return 0;
    }
    if (pir->ir_options == 2)
    {
        static const char s_szFsName[] = SF_FS_NAME;
        if (pir->ir_data && pir->ir_length >= sizeof(s_szFsName))
            SfMemCopy(pir->ir_data, s_szFsName, sizeof(s_szFsName));
        pir->ir_length  = (260UL << 16) | 255;
        pir->ir_options = FS_CASE_IS_PRESERVED | FS_UNICODE_STORED_ON_DISK | FS_VOL_SUPPORTS_LONG_NAMES;
        pir->ir_pos     = 4096;
        pir->ir_error   = 0;
        return 0;
    }
    pir->ir_error = SF_ERROR_INVALID_FUNCTION;
    return pir->ir_error;
}


/*********************************************************************************************************************************
*   Volume functions                                                                                                             *
*********************************************************************************************************************************/

/**
 * FS_GetDiskInfo: the host's volume, in clusters small enough to count in
 * 32 bits.
 */
static int __cdecl sfNetGetDiskInfo(pioreq pir)
{
    SFRESOURCE *pRes = (SFRESOURCE *)pir->ir_rh;
    static const WCHAR s_wszRoot[] = { '\\', 0 };
    struct
    {
        USHORT u16Size;
        USHORT u16Length;
        WCHAR  wsz[2];
    } Root;
    SHFLCREATEPARMS Parms;
    SHFLVOLINFO VolInfo;
    ULONG cbInfo = sizeof(VolInfo);
    ULONG cbSector, cSectorsPerCluster, cMaxClusters;
    UINT64 cClusters, cFree;
    int rc;

    SF_LOG_CALL_NO_PATH("GetDiskInfo", pir);
    Root.u16Size   = sizeof(s_wszRoot);
    Root.u16Length = sizeof(s_wszRoot) - sizeof(WCHAR);
    SfMemCopy(Root.wsz, s_wszRoot, sizeof(s_wszRoot));
    SfMemZero(&Parms, sizeof(Parms));
    Parms.Handle      = SHFL_HANDLE_NIL;
    Parms.CreateFlags = SHFL_CF_DIRECTORY | SHFL_CF_ACT_OPEN_IF_EXISTS | SHFL_CF_ACT_FAIL_IF_NEW | SHFL_CF_ACCESS_READ;
    rc = SfHostCreate(pRes->idRoot, (SHFLSTRING *)&Root, &Parms);
    if (rc >= 0 && Parms.Handle == SHFL_HANDLE_NIL)
        rc = VERR_PATH_NOT_FOUND;
    if (rc >= 0)
    {
        SfMemZero(&VolInfo, sizeof(VolInfo));
        rc = SfHostInformation(pRes->idRoot, Parms.Handle, SHFL_INFO_GET | SHFL_INFO_VOLUME, &VolInfo, &cbInfo);
        SfHostClose(pRes->idRoot, Parms.Handle);
    }
    if (rc < 0)
    {
        pir->ir_error = SfRcToWin32(rc);
        return pir->ir_error;
    }

    cbSector = VolInfo.ulBytesPerSector ? VolInfo.ulBytesPerSector : 512;
    if (pir->ir_options & 1)
    {
        /* Extended request (INT 21h 7303h, GetDiskFreeSpaceEx): 32-bit counts. */
        cSectorsPerCluster = VolInfo.ulBytesPerAllocationUnit / cbSector;
        if (!cSectorsPerCluster)
            cSectorsPerCluster = 1;
        cMaxClusters = 0x7fffffff;
    }
    else
    {
        /* INT 21h 36h and GetDiskFreeSpace: IFSMgr keeps 16 bits, so at
           most about 2 GB in 32 KB clusters, as VFAT reports big drives. */
        cSectorsPerCluster = 32768 / cbSector ? 32768 / cbSector : 1;
        cMaxClusters = 0xffff;
    }
    for (;;)
    {
        cClusters = SfU64DivU32((UINT64)VolInfo.ullTotalAllocationBytes, cbSector * cSectorsPerCluster);
        if (cClusters <= cMaxClusters || cSectorsPerCluster >= 0x8000 || cMaxClusters == 0xffff)
            break;
        cSectorsPerCluster <<= 1;
    }
    cFree = SfU64DivU32((UINT64)VolInfo.ullAvailableAllocationBytes, cbSector * cSectorsPerCluster);
    pir->ir_length   = cbSector;
    pir->ir_sectors  = (USHORT)cSectorsPerCluster;
    pir->ir_size     = (ULONG)(cClusters > cMaxClusters ? cMaxClusters : cClusters);
    pir->ir_numfree  = (ULONG)(cFree > cMaxClusters ? cMaxClusters : cFree);
    pir->ir_data     = NULL;
    pir->ir_error    = 0;
    return 0;
}

/**
 * FS_FileAttributes: attributes, size and times.
 */
static int __cdecl sfNetFileAttrib(pioreq pir)
{
    SFRESOURCE *pRes = (SFRESOURCE *)pir->ir_rh;
    SHFLFSOBJINFO Info;
    int rc;

    SF_LOG_CALL("FileAttrib", pir);
    switch (pir->ir_flags)
    {
        case GET_ATTRIBUTES:
        case GET_ATTRIB_COMP_FILESIZE:
        case GET_ATTRIB_MODIFY_DATETIME:
        case GET_ATTRIB_LAST_ACCESS_DATETIME:
        case GET_ATTRIB_CREATION_DATETIME:
        case GET_ATTRIB_FIRST_CLUST:
            break;

        case SET_ATTRIBUTES:
        case SET_ATTRIB_MODIFY_DATETIME:
        case SET_ATTRIB_LAST_ACCESS_DATETIME:
        case SET_ATTRIB_CREATION_DATETIME:
            SfMemZero(&Info, sizeof(Info));
            if (pir->ir_flags == SET_ATTRIBUTES)
            {
                ULONG fDosAttr = pir->ir_attr & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN
                                                 | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE);
                Info.Attr.fMode = fDosAttr ? fDosAttr << RTFS_DOS_SHIFT : RTFS_DOS_NT_NORMAL;
            }
            else if (pir->ir_flags == SET_ATTRIB_MODIFY_DATETIME)
                Info.ModificationTime = SfDosTimeToTime(pir->ir_dostime);
            else if (pir->ir_flags == SET_ATTRIB_LAST_ACCESS_DATETIME)
                Info.AccessTime = SfDosTimeToTime(pir->ir_dostime);
            else
                Info.BirthTime = SfDosTimeToTime(pir->ir_dostime);
            rc = sfSetPathInfo(pRes, pir->ir_ppath, &Info);
            pir->ir_error = rc >= 0 ? 0 : SfRcToWin32(rc);
            return pir->ir_error;

        default:
            pir->ir_error = SF_ERROR_INVALID_FUNCTION;
            return pir->ir_error;
    }

    rc = SfLookup(pRes, pir->ir_ppath, &Info);
    if (rc < 0)
    {
        pir->ir_error = SfRcToWin32(rc);
        return pir->ir_error;
    }
    switch (pir->ir_flags)
    {
        case GET_ATTRIBUTES:
            pir->ir_attr = SF_DOS_ATTR(&Info);
            pir->ir_size = (ULONG)Info.cbObject;
            break;
        case GET_ATTRIB_COMP_FILESIZE:
            pir->ir_size = (ULONG)Info.cbObject;
            break;
        case GET_ATTRIB_MODIFY_DATETIME:
            pir->ir_dostime = SfTimeToDosTime(Info.ModificationTime);
            break;
        case GET_ATTRIB_LAST_ACCESS_DATETIME:
            pir->ir_dostime = SfTimeToDosTime(Info.AccessTime);
            break;
        case GET_ATTRIB_CREATION_DATETIME:
            pir->ir_dostime = SfTimeToDosTime(Info.BirthTime);
            pir->ir_options = 0;
            break;
        case GET_ATTRIB_FIRST_CLUST:
            pir->ir_pos = 0;
            break;
    }
    pir->ir_error = 0;
    return 0;
}

/**
 * FS_Dir: makes, removes and checks directories.
 */
static int __cdecl sfNetDir(pioreq pir)
{
    SFRESOURCE *pRes = (SFRESOURCE *)pir->ir_rh;
    SHFLFSOBJINFO Info;
    int rc;

    SF_LOG_CALL("Dir", pir);
    switch (pir->ir_flags)
    {
        case CHECK_DIR:
            rc = SfLookup(pRes, pir->ir_ppath, &Info);
            if (rc >= 0 && !(SF_DOS_ATTR(&Info) & FILE_ATTRIBUTE_DIRECTORY))
                rc = VERR_PATH_NOT_FOUND;
            pir->ir_error = rc >= 0 ? 0 : (rc == VERR_FILE_NOT_FOUND ? SF_ERROR_PATH_NOT_FOUND : SfRcToWin32(rc));
            return pir->ir_error;

        case QUERY83_DIR:
        case QUERYLONG_DIR:
        {
            SHFLSTRING *pPath = SfTranslatePath(pRes, pir->ir_ppath, 0,
                                                pir->ir_flags == QUERY83_DIR ? SF_PATH_SHORT : SF_PATH_LONG);
            if (!pPath)
            {
                pir->ir_error = SF_ERROR_NOT_ENOUGH_MEMORY;
                return pir->ir_error;
            }
            SfHostPathToParsed(pPath, pir->ir_ppath2);
            SfHeapFree(pPath, 0);
            pir->ir_error = 0;
            return 0;
        }

        case CREATE_DIR:
        {
            SHFLCREATEPARMS Parms;
            SHFLSTRING *pPath = SfPathToHostKeepCase(pRes, pir->ir_ppath, pir->ir_attr, pir->ir_uFName);
            if (!pPath)
            {
                pir->ir_error = SF_ERROR_NOT_ENOUGH_MEMORY;
                return pir->ir_error;
            }
            SfMemZero(&Parms, sizeof(Parms));
            Parms.Handle      = SHFL_HANDLE_NIL;
            Parms.CreateFlags = SHFL_CF_DIRECTORY | SHFL_CF_ACT_FAIL_IF_EXISTS | SHFL_CF_ACT_CREATE_IF_NEW
                              | SHFL_CF_ACCESS_READ;
            rc = SfHostCreate(pRes->idRoot, pPath, &Parms);
            SfHeapFree(pPath, 0);
            if (rc >= 0 && Parms.Handle == SHFL_HANDLE_NIL)
                rc = Parms.Result == SHFL_FILE_EXISTS ? VERR_ALREADY_EXISTS
                   : Parms.Result == SHFL_PATH_NOT_FOUND ? VERR_PATH_NOT_FOUND : VERR_ACCESS_DENIED;
            if (rc >= 0)
                SfHostClose(pRes->idRoot, Parms.Handle);
            /* DOS answers "access denied" when the name exists already. */
            pir->ir_error = rc >= 0 ? 0 : rc == VERR_ALREADY_EXISTS ? SF_ERROR_ACCESS_DENIED : SfRcToWin32(rc);
            return pir->ir_error;
        }

        case DELETE_DIR:
        {
            SHFLSTRING *pPath = SfPathToHost(pRes, pir->ir_ppath, 0);
            if (!pPath)
            {
                pir->ir_error = SF_ERROR_NOT_ENOUGH_MEMORY;
                return pir->ir_error;
            }
            rc = SfHostRemove(pRes->idRoot, pPath, SHFL_REMOVE_DIR);
            SfHeapFree(pPath, 0);
            pir->ir_error = rc >= 0 ? 0 : rc == VERR_DIR_NOT_EMPTY ? SF_ERROR_ACCESS_DENIED : SfRcToWin32(rc);
            return pir->ir_error;
        }

        default:
            pir->ir_error = SF_ERROR_INVALID_FUNCTION;
            return pir->ir_error;
    }
}

/**
 * FS_FlushVolume: nothing is cached here.
 */
static int __cdecl sfNetFlush(pioreq pir)
{
    SF_LOG_CALL_NO_PATH("Flush", pir);
    pir->ir_error = 0;
    return 0;
}

/**
 * FS_DeleteFile.  Wildcards are left to IFSMgr, which then finds the names
 * and deletes them one by one.
 */
static int __cdecl sfNetDelete(pioreq pir)
{
    SFRESOURCE *pRes = (SFRESOURCE *)pir->ir_rh;
    SHFLFSOBJINFO Info;
    SHFLSTRING *pPath;
    int rc;

    SF_LOG_CALL("Delete", pir);
    if (pir->ir_attr & FILE_FLAG_WILDCARDS)
    {
        pir->ir_error = SF_ERROR_INVALID_FUNCTION;
        return pir->ir_error;
    }
    rc = SfLookup(pRes, pir->ir_ppath, &Info);
    if (rc >= 0 && (SF_DOS_ATTR(&Info) & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY)))
        rc = VERR_ACCESS_DENIED;
    if (rc >= 0)
    {
        pPath = SfPathToHost(pRes, pir->ir_ppath, 0);
        rc = pPath ? SfHostRemove(pRes->idRoot, pPath, SHFL_REMOVE_FILE) : VERR_NO_MEMORY;
        if (pPath)
            SfHeapFree(pPath, 0);
    }
    pir->ir_error = rc >= 0 ? 0 : SfRcToWin32(rc);
    return pir->ir_error;
}

/**
 * FS_RenameFile (files and directories, also between directories).
 * Wildcards (DOS only) are left to IFSMgr.
 */
static int __cdecl sfNetRename(pioreq pir)
{
    SFRESOURCE *pRes = (SFRESOURCE *)pir->ir_rh;
    SHFLFSOBJINFO Info;
    SHFLSTRING *pSrc, *pDst;
    int rc;

    SF_LOG_CALL("Rename", pir);
    if ((pir->ir_attr & FILE_FLAG_WILDCARDS) || (pir->ir_attr2 & FILE_FLAG_WILDCARDS))
    {
        pir->ir_error = SF_ERROR_INVALID_FUNCTION;
        return pir->ir_error;
    }
    rc = SfLookup(pRes, pir->ir_ppath, &Info);
    if (rc < 0)
    {
        pir->ir_error = SfRcToWin32(rc);
        return pir->ir_error;
    }
    pSrc = SfPathToHost(pRes, pir->ir_ppath, 0);
    pDst = SfPathToHostKeepCase(pRes, pir->ir_ppath2, pir->ir_attr2, pir->ir_uFName);
    if (pSrc && pDst)
        rc = SfHostRename(pRes->idRoot, pSrc, pDst,
                          SF_DOS_ATTR(&Info) & FILE_ATTRIBUTE_DIRECTORY ? SHFL_RENAME_DIR : SHFL_RENAME_FILE);
    else
        rc = VERR_NO_MEMORY;
    if (pSrc)
        SfHeapFree(pSrc, 0);
    if (pDst)
        SfHeapFree(pDst, 0);
    pir->ir_error = rc >= 0 ? 0 : SfRcToWin32(rc);
    return pir->ir_error;
}

/**
 * For the functions a network FSD doesn't have (IFSMgr wants an error
 * function in those table entries).
 */
int __cdecl SfNetBadFunction(pioreq pir)
{
    pir->ir_error = SF_ERROR_INVALID_FUNCTION;
    return pir->ir_error;
}
