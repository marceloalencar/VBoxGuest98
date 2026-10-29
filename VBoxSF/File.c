/** @file
 * VBoxSF - Open files: FS_OpenFile and the handle based functions.
 *
 * Every open is a host handle; IFSMgr keeps the file position and passes it
 * on each read and write.
 */
#include "VBoxSF.h"

#pragma VxD_LOCKED_CODE_SEG
#pragma VxD_LOCKED_DATA_SEG


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/

/** Serial number reported for every share ('VBSF'). */
#define SF_VOLUME_SERIAL        0x56425346

/** Win32 error for a seek before the start of the file. */
#define SF_ERROR_NEGATIVE_SEEK  131


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/

/** An open file (the FSD file handle, ir_fh). */
typedef struct SFFILE
{
    SHFLROOT   idRoot;
    SHFLHANDLE hHost;
    /** The path relative to the share, for ENUMH_GETFILENAME. */
    path_t     ppath;
} SFFILE;


/*********************************************************************************************************************************
*   Internal Functions                                                                                                           *
*********************************************************************************************************************************/

static int __cdecl sfFileRead(pioreq pir);
static int __cdecl sfFileWrite(pioreq pir);
static int __cdecl sfFileSeek(pioreq pir);
static int __cdecl sfFileClose(pioreq pir);
static int __cdecl sfFileCommit(pioreq pir);
static int __cdecl sfFileLocks(pioreq pir);
static int __cdecl sfFileTimes(pioreq pir);
static int __cdecl sfFileEnumHandle(pioreq pir);


/*********************************************************************************************************************************
*   Global Variables                                                                                                             *
*********************************************************************************************************************************/

/** The handle functions of open files. */
static hndlmisc g_FileMisc =
{
    IFS_VERSION, IFS_REVISION, NUM_HNDLMISC,
    {
        sfFileSeek,             /* HM_SEEK */
        sfFileClose,            /* HM_CLOSE */
        sfFileCommit,           /* HM_COMMIT */
        sfFileLocks,            /* HM_FILELOCKS */
        sfFileTimes,            /* HM_FILETIMES */
        SfNetBadFunction,       /* HM_PIPEREQUEST */
        SfNetBadFunction,       /* HM_HANDLEINFO */
        sfFileEnumHandle,       /* HM_ENUMHANDLE */
    }
};


/*********************************************************************************************************************************
*   Helpers                                                                                                                      *
*********************************************************************************************************************************/

/**
 * Gets the current information of an open file from the host.
 */
static int sfFileQueryInfo(SFFILE *pFile, SHFLFSOBJINFO *pInfo)
{
    ULONG cbInfo = sizeof(*pInfo);
    SfMemZero(pInfo, sizeof(*pInfo));
    return SfHostInformation(pFile->idRoot, pFile->hHost, SHFL_INFO_GET | SHFL_INFO_FILE, pInfo, &cbInfo);
}


/*********************************************************************************************************************************
*   FS_OpenFile                                                                                                                  *
*********************************************************************************************************************************/

int __cdecl SfFileOpen(pioreq pir)
{
    SFRESOURCE *pRes = (SFRESOURCE *)pir->ir_rh;
    ULONG fAccess = pir->ir_flags & ACCESS_MODE_MASK;
    ULONG fShare  = pir->ir_flags & SHARE_MODE_MASK;
    ULONG fAction = pir->ir_options & ACTION_MASK;
    SHFLCREATEPARMS Parms;
    SHFLSTRING *pPath;
    SFFILE *pFile;
    int rc;

    /* No volume labels on a share, and no taking over of files left open before Windows. */
    if (!pir->ir_ppath || (pir->ir_attr & FILE_ATTRIBUTE_LABEL))
    {
        pir->ir_error = SF_ERROR_ACCESS_DENIED;
        return pir->ir_error;
    }

#ifdef VBOXSF_LOG_CALLS
    SfLogRel("Open: flags %#x options %#x attr %#x\n", pir->ir_flags, pir->ir_options, pir->ir_attr);
#endif
    pPath = SfPathToHostKeepCase(pRes, pir->ir_ppath, pir->ir_attr, pir->ir_uFName);
    pFile = (SFFILE *)SfHeapAllocate(sizeof(*pFile), HEAPZEROINIT);
    if (pFile)
        pFile->ppath = (path_t)SfHeapAllocate(IFSPathSize(pir->ir_ppath), 0);
    if (!pPath || !pFile || !pFile->ppath)
    {
        if (pPath)
            SfHeapFree(pPath, 0);
        if (pFile && pFile->ppath)
            SfHeapFree(pFile->ppath, 0);
        if (pFile)
            SfHeapFree(pFile, 0);
        pir->ir_error = SF_ERROR_NOT_ENOUGH_MEMORY;
        return pir->ir_error;
    }
    SfMemCopy(pFile->ppath, pir->ir_ppath, IFSPathSize(pir->ir_ppath));

    SfMemZero(&Parms, sizeof(Parms));
    Parms.Handle      = SHFL_HANDLE_NIL;
    Parms.CreateFlags = SHFL_CF_ACCESS_ATTR_READ;
    if (fAction & ACTION_TRUNCATE)
        Parms.CreateFlags |= SHFL_CF_ACT_OVERWRITE_IF_EXISTS;
    else if (fAction & ACTION_EXISTS_OPEN)
        Parms.CreateFlags |= SHFL_CF_ACT_OPEN_IF_EXISTS;
    else
        Parms.CreateFlags |= SHFL_CF_ACT_FAIL_IF_EXISTS;
    if (fAction & ACTION_NEXISTS_CREATE)
    {
        ULONG fDosAttr = pir->ir_attr & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN
                                         | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE);
        Parms.CreateFlags |= SHFL_CF_ACT_CREATE_IF_NEW;
        Parms.Info.Attr.fMode = fDosAttr << RTFS_DOS_SHIFT;
    }
    else
        Parms.CreateFlags |= SHFL_CF_ACT_FAIL_IF_NEW;
    if (fAccess == ACCESS_WRITEONLY)
        Parms.CreateFlags |= SHFL_CF_ACCESS_WRITE;
    else if (fAccess == ACCESS_READWRITE)
        Parms.CreateFlags |= SHFL_CF_ACCESS_READ | SHFL_CF_ACCESS_WRITE;
    else
        Parms.CreateFlags |= SHFL_CF_ACCESS_READ;
    if (fShare == SHARE_DENYREADWRITE)
        Parms.CreateFlags |= SHFL_CF_ACCESS_DENYREAD | SHFL_CF_ACCESS_DENYWRITE;
    else if (fShare == SHARE_DENYWRITE)
        Parms.CreateFlags |= SHFL_CF_ACCESS_DENYWRITE;
    else if (fShare == SHARE_DENYREAD)
        Parms.CreateFlags |= SHFL_CF_ACCESS_DENYREAD;
    rc = SfHostCreate(pRes->idRoot, pPath, &Parms);
    SfHeapFree(pPath, 0);
    if (rc >= 0 && Parms.Handle == SHFL_HANDLE_NIL)
        rc = Parms.Result == SHFL_PATH_NOT_FOUND ? VERR_PATH_NOT_FOUND
           : Parms.Result == SHFL_FILE_NOT_FOUND ? VERR_FILE_NOT_FOUND
           : Parms.Result == SHFL_FILE_EXISTS    ? VERR_ALREADY_EXISTS : VERR_ACCESS_DENIED;
    if (rc >= 0 && (SF_DOS_ATTR(&Parms.Info) & FILE_ATTRIBUTE_DIRECTORY))
    {
        SfHostClose(pRes->idRoot, Parms.Handle);
        rc = VERR_ACCESS_DENIED;
    }
    if (rc < 0)
    {
        SfHeapFree(pFile->ppath, 0);
        SfHeapFree(pFile, 0);
        /* DOS and Win32 want ERROR_FILE_EXISTS for a create of an existing file. */
        pir->ir_error = rc == VERR_ALREADY_EXISTS ? SF_ERROR_FILE_EXISTS : SfRcToWin32(rc);
        return pir->ir_error;
    }

    pFile->idRoot = pRes->idRoot;
    pFile->hHost  = Parms.Handle;
    pir->ir_fh                 = pFile;
    pir->ir_hfunc->hf_read     = sfFileRead;
    pir->ir_hfunc->hf_write    = sfFileWrite;
    pir->ir_hfunc->hf_misc     = &g_FileMisc;
    pir->ir_options            = Parms.Result == SHFL_FILE_CREATED  ? ACTION_CREATED
                               : Parms.Result == SHFL_FILE_REPLACED ? ACTION_REPLACED : ACTION_OPENED;
    pir->ir_size               = (ULONG)Parms.Info.cbObject;
    pir->ir_attr               = SF_DOS_ATTR(&Parms.Info);
    pir->ir_dostime            = SfTimeToDosTime(Parms.Info.ModificationTime);
    pir->ir_error              = 0;
    return 0;
}


/*********************************************************************************************************************************
*   Handle functions                                                                                                             *
*********************************************************************************************************************************/

/**
 * FS_ReadFile, in host calls of up to SF_MAX_IO_CHUNK bytes.
 *
 * Paging reads (the program loader, memory mapped files) come with the pager
 * busy: NTKERN then returns from I/O controls without calling the driver,
 * and its worker threads don't run either, so those go through the driver's
 * direct HGCM entry point.
 */
static int __cdecl sfFileRead(pioreq pir)
{
    SFFILE *pFile = (SFFILE *)pir->ir_fh;
    BYTE *pb = (BYTE *)pir->ir_data;
    ULONG cbLeft = pir->ir_length;
    ULONG cbDone = 0;
    BOOL fPaging = (pir->ir_options & (R0_LOADER_IO | R0_SWAPPER_CALL | R0_MM_READ_WRITE)) != 0;
    int rc = VINF_SUCCESS;

    while (cbLeft)
    {
        ULONG cbChunk = cbLeft < SF_MAX_IO_CHUNK ? cbLeft : SF_MAX_IO_CHUNK;
        ULONG cbRead = cbChunk;
        rc = SfHostRead(pFile->idRoot, pFile->hHost, (UINT64)(pir->ir_pos + cbDone), pb + cbDone, &cbRead, fPaging);
        if (rc < 0)
            break;
        cbDone += cbRead;
        cbLeft -= cbRead;
        if (cbRead < cbChunk)
            break;
    }
#ifdef VBOXSF_LOG_CALLS
    SfLogRel("Read: pos %#x length %u options %#x: %d, %u bytes\n",
             pir->ir_pos, pir->ir_length, pir->ir_options, rc, cbDone);
#endif
    if (rc < 0 && !cbDone)
    {
        pir->ir_length = 0;
        pir->ir_error = SfRcToWin32(rc);
        return pir->ir_error;
    }
    pir->ir_length = cbDone;
    pir->ir_pos   += cbDone;
    pir->ir_error  = 0;
    return 0;
}

/**
 * FS_WriteFile, in host calls of up to SF_MAX_IO_CHUNK bytes; a write of
 * zero bytes sets the size of the file to the position.  Paging writes
 * (memory mapped files) take the driver's direct entry point, as reads.
 */
static int __cdecl sfFileWrite(pioreq pir)
{
    SFFILE *pFile = (SFFILE *)pir->ir_fh;
    const BYTE *pb = (const BYTE *)pir->ir_data;
    ULONG cbLeft = pir->ir_length;
    ULONG cbDone = 0;
    BOOL fPaging = (pir->ir_options & (R0_SWAPPER_CALL | R0_MM_READ_WRITE)) != 0;
    int rc = VINF_SUCCESS;

    if (!cbLeft)
    {
        rc = SfHostSetFileSize(pFile->idRoot, pFile->hHost, (UINT64)pir->ir_pos);
        pir->ir_error = rc >= 0 ? 0 : SfRcToWin32(rc);
        return pir->ir_error;
    }
    while (cbLeft)
    {
        ULONG cbChunk = cbLeft < SF_MAX_IO_CHUNK ? cbLeft : SF_MAX_IO_CHUNK;
        ULONG cbWritten = cbChunk;
        rc = SfHostWrite(pFile->idRoot, pFile->hHost, (UINT64)(pir->ir_pos + cbDone), pb + cbDone, &cbWritten,
                         fPaging);
        if (rc < 0)
            break;
        cbDone += cbWritten;
        cbLeft -= cbWritten;
        if (cbWritten < cbChunk)
            break;
    }
    if (rc < 0 && !cbDone)
    {
        pir->ir_length = 0;
        pir->ir_error = SfRcToWin32(rc);
        return pir->ir_error;
    }
    pir->ir_length = cbDone;
    pir->ir_pos   += cbDone;
    pir->ir_error  = 0;
    return 0;
}

/**
 * FS_FileSeek: only FILE_END needs us (the size).
 */
static int __cdecl sfFileSeek(pioreq pir)
{
    SFFILE *pFile = (SFFILE *)pir->ir_fh;
    SHFLFSOBJINFO Info;
    LONG offNew;
    int rc;

    if (pir->ir_flags != FILE_END)
    {
        pir->ir_error = 0;
        return 0;
    }
    rc = sfFileQueryInfo(pFile, &Info);
    if (rc < 0)
    {
        pir->ir_error = SfRcToWin32(rc);
        return pir->ir_error;
    }
    offNew = (LONG)Info.cbObject + (LONG)pir->ir_pos;
    if (offNew < 0)
    {
        pir->ir_error = SF_ERROR_NEGATIVE_SEEK;
        return pir->ir_error;
    }
    pir->ir_pos   = (ULONG)offNew;
    pir->ir_error = 0;
    return 0;
}

/**
 * FS_CloseFile: the host handle goes with the final close.
 */
static int __cdecl sfFileClose(pioreq pir)
{
    SFFILE *pFile = (SFFILE *)pir->ir_fh;

    if (pir->ir_flags == CLOSE_FINAL)
    {
        SfHostClose(pFile->idRoot, pFile->hHost);
        SfHeapFree(pFile->ppath, 0);
        SfHeapFree(pFile, 0);
    }
    pir->ir_pos   = 0;
    pir->ir_error = 0;
    return 0;
}

/**
 * FS_CommitFile: nothing is buffered here, the host flushes its file.
 */
static int __cdecl sfFileCommit(pioreq pir)
{
    SFFILE *pFile = (SFFILE *)pir->ir_fh;
    int rc = SfHostFlush(pFile->idRoot, pFile->hHost);
    /* A handle opened for reading only can't be flushed on some hosts; nothing to lose then. */
    pir->ir_error = rc >= 0 || rc == VERR_ACCESS_DENIED || rc == VERR_INVALID_HANDLE ? 0 : SfRcToWin32(rc);
    return pir->ir_error;
}

/**
 * FS_LockFile: DOS (exclusive) record locks on the host file; they go away
 * with the host handle at the final close.
 */
static int __cdecl sfFileLocks(pioreq pir)
{
    SFFILE *pFile = (SFFILE *)pir->ir_fh;
    int rc;

    if (pir->ir_flags != LOCK_REGION && pir->ir_flags != UNLOCK_REGION)
    {
        pir->ir_error = SF_ERROR_INVALID_FUNCTION;
        return pir->ir_error;
    }
    rc = SfHostLock(pFile->idRoot, pFile->hHost, (UINT64)pir->ir_pos, (UINT64)pir->ir_locklen,
                    pir->ir_flags == LOCK_REGION ? SHFL_LOCK_EXCLUSIVE | SHFL_LOCK_NOWAIT | SHFL_LOCK_PARTIAL
                                                 : SHFL_LOCK_CANCEL);
    pir->ir_error = rc >= 0 ? 0 : SfRcToWin32(rc);
    return pir->ir_error;
}

/**
 * FS_FileDateTime: gets or sets the times.
 */
static int __cdecl sfFileTimes(pioreq pir)
{
    SFFILE *pFile = (SFFILE *)pir->ir_fh;
    SHFLFSOBJINFO Info;
    int rc;

    if (   pir->ir_flags == SET_MODIFY_DATETIME
        || pir->ir_flags == SET_LAST_ACCESS_DATETIME
        || pir->ir_flags == SET_CREATION_DATETIME)
    {
        SfMemZero(&Info, sizeof(Info));
        if (pir->ir_flags == SET_MODIFY_DATETIME)
            Info.ModificationTime = SfDosTimeToTime(pir->ir_dostime);
        else if (pir->ir_flags == SET_LAST_ACCESS_DATETIME)
            Info.AccessTime = SfDosTimeToTime(pir->ir_dostime);
        else
            Info.BirthTime = SfDosTimeToTime(pir->ir_dostime);
        rc = SfSetInfo(pFile->idRoot, pFile->hHost, &Info);
        pir->ir_error = rc >= 0 ? 0 : SfRcToWin32(rc);
        return pir->ir_error;
    }
    if (   pir->ir_flags != GET_MODIFY_DATETIME
        && pir->ir_flags != GET_LAST_ACCESS_DATETIME
        && pir->ir_flags != GET_CREATION_DATETIME)
    {
        pir->ir_error = SF_ERROR_INVALID_FUNCTION;
        return pir->ir_error;
    }
    rc = sfFileQueryInfo(pFile, &Info);
    if (rc < 0)
    {
        pir->ir_error = SfRcToWin32(rc);
        return pir->ir_error;
    }
    if (pir->ir_flags == GET_MODIFY_DATETIME)
        pir->ir_dostime = SfTimeToDosTime(Info.ModificationTime);
    else if (pir->ir_flags == GET_LAST_ACCESS_DATETIME)
        pir->ir_dostime = SfTimeToDosTime(Info.AccessTime);
    else
    {
        pir->ir_dostime = SfTimeToDosTime(Info.BirthTime);
        pir->ir_options = 0;
    }
    pir->ir_error = 0;
    return 0;
}

/**
 * FS_EnumerateHandle: GetFileInformationByHandle and the file's name.
 */
static int __cdecl sfFileEnumHandle(pioreq pir)
{
    SFFILE *pFile = (SFFILE *)pir->ir_fh;

    if (pir->ir_flags == ENUMH_GETFILEINFO)
    {
        _BY_HANDLE_FILE_INFORMATION *pBhfi = (_BY_HANDLE_FILE_INFORMATION *)pir->ir_data;
        SHFLFSOBJINFO Info;
        int rc = sfFileQueryInfo(pFile, &Info);
        if (rc < 0)
        {
            pir->ir_error = SfRcToWin32(rc);
            return pir->ir_error;
        }
        pBhfi->bhfi_dwFileAttributes = SF_DOS_ATTR(&Info);
        SfTimeToFileTime(Info.BirthTime, &pBhfi->bhfi_ftCreationTime);
        SfTimeToFileTime(Info.AccessTime, &pBhfi->bhfi_ftLastAccessTime);
        SfTimeToFileTime(Info.ModificationTime, &pBhfi->bhfi_ftLastWriteTime);
        pBhfi->bhfi_dwVolumeSerialNumber = SF_VOLUME_SERIAL;
        pBhfi->bhfi_nFileSizeHigh        = SF_HI32(Info.cbObject);
        pBhfi->bhfi_nFileSizeLow         = (ULONG)Info.cbObject;
        pBhfi->bhfi_nNumberOfLinks       = 1;
        pBhfi->bhfi_nFileIndexHigh       = pFile->idRoot;
        pBhfi->bhfi_nFileIndexLow        = (ULONG)pFile->hHost;
        pir->ir_error = 0;
        return 0;
    }
    if (pir->ir_flags == ENUMH_GETFILENAME)
    {
        SfMemCopy(pir->ir_ppath, pFile->ppath, IFSPathSize(pFile->ppath));
        pir->ir_error = 0;
        return 0;
    }
    pir->ir_error = SF_ERROR_INVALID_FUNCTION;
    return pir->ir_error;
}
