/** @file
 * VBoxSF - VirtualBox shared folders for Windows 98 and ME: internal header.
 *
 * VBoxSF is a static VxD that registers with IFSMgr as a network file system
 * driver (a redirector) and talks to the host's VBoxSharedFolders HGCM
 * service through the VBoxGst driver, which it opens with NTKERN like any
 * other client.  \\VBOXSVR\<share> (or \\VBOXSRV\<share>) connects to a
 * shared folder.
 */

#ifndef VBOXSF_H
#define VBOXSF_H

#define WANTVXDWRAPS

#include <basedef.h>
#include <vmm.h>
#include <vwin32.h>
#include <ifs.h>
#include <stdarg.h>

/*
 * What VBoxGuest98.h and VMMDev.h expect from <wdm.h> or <windows.h>.
 */
typedef unsigned short      WCHAR;
typedef unsigned char       BOOLEAN;
typedef unsigned int        UINT32;
typedef int                 INT32;
typedef unsigned __int64    UINT64;
typedef __int64             INT64;
typedef long                NTSTATUS;

#define CTL_CODE(DeviceType, Function, Method, Access) \
    (((DeviceType) << 16) | ((Access) << 14) | ((Function) << 2) | (Method))
#define FILE_DEVICE_UNKNOWN         0x00000022
#define METHOD_BUFFERED             0
#define FILE_ANY_ACCESS             0

#include "..\inc\VBoxGuest98.h"

/*
 * NTKERN (the subset of the NT native API it gives VxDs).
 */
typedef struct SFUNICODESTRING
{
    USHORT  Length;
    USHORT  MaximumLength;
    WCHAR  *Buffer;
} SFUNICODESTRING;

typedef struct SFOBJECTATTRIBUTES
{
    ULONG            Length;
    HANDLE           RootDirectory;
    SFUNICODESTRING *ObjectName;
    ULONG            Attributes;
    PVOID            SecurityDescriptor;
    PVOID            SecurityQualityOfService;
} SFOBJECTATTRIBUTES;

typedef struct SFIOSTATUSBLOCK
{
    NTSTATUS Status;
    ULONG    Information;
} SFIOSTATUSBLOCK;

#define NT_SUCCESS(rcNt)                ((NTSTATUS)(rcNt) >= 0)
#define SF_SYNCHRONIZE                  0x00100000
#define SF_GENERIC_READ                 0x80000000
#define SF_GENERIC_WRITE                0x40000000
#define SF_FILE_SHARE_READ              0x00000001
#define SF_FILE_SHARE_WRITE             0x00000002
#define SF_FILE_OPEN                    0x00000001
#define SF_FILE_SYNCHRONOUS_IO_NONALERT 0x00000020
#define SF_OBJ_CASE_INSENSITIVE         0x00000040

/*
 * Win32 error codes the IFS functions return in ir_error.
 */
#define SF_ERROR_INVALID_FUNCTION       1
#define SF_ERROR_FILE_NOT_FOUND         2
#define SF_ERROR_PATH_NOT_FOUND         3
#define SF_ERROR_TOO_MANY_OPEN_FILES    4
#define SF_ERROR_ACCESS_DENIED          5
#define SF_ERROR_INVALID_HANDLE         6
#define SF_ERROR_NOT_ENOUGH_MEMORY      8
#define SF_ERROR_NO_MORE_FILES          18
#define SF_ERROR_WRITE_PROTECT          19
#define SF_ERROR_SHARING_VIOLATION      32
#define SF_ERROR_LOCK_VIOLATION         33
#define SF_ERROR_NOT_SUPPORTED          50
#define SF_ERROR_BAD_NETPATH            53
#define SF_ERROR_UNEXP_NET_ERR          59
#define SF_ERROR_BAD_NET_NAME           67
#define SF_ERROR_FILE_EXISTS            80
#define SF_ERROR_INVALID_PARAMETER      87
#define SF_ERROR_DISK_FULL              112
#define SF_ERROR_INVALID_NAME           123
#define SF_ERROR_DIR_NOT_EMPTY          145
#define SF_ERROR_ALREADY_EXISTS         183

/** Version given to IFSMgr_RegisterNet: IFSMGRVERSION of IFSMGR.INC, which
 *  IFS.H doesn't have (its IFS_VERSION is the one of the function tables).
 *  Windows 98 and ME return it from IFSMgr_Get_Version. */
#define VBOXSF_IFSMGR_VERSION           0x22

/** Provider type given to IFSMgr_RegisterNet (WNNC_NET_RDR2SAMPLE, as the
 *  network provider of the official Windows Guest Additions). */
#define VBOXSF_NET_ID                   0x00250000

/*
 * Win32 DeviceIoControl codes of \\.\VBOXSF.
 */
/** Logs the host's shared folders to VBox.log.  No buffers. */
#define VBOXSF_IOCTL_LOG_MAPPINGS       1
/** Names of the host's shared folders (for VBoxNP.dll): the output buffer
 *  gets them in UTF-16, each one terminated, the list ended by an empty one. */
#define VBOXSF_IOCTL_QUERY_NAMES        2

/*
 * Thunks to the VMM, IFSMgr and NTKERN services (VBoxSFA.asm).
 */
PVOID   __cdecl SfCreateSemaphore(LONG cTokens);
void    __cdecl SfDestroySemaphore(PVOID hSem);
void    __cdecl SfWaitSemaphore(PVOID hSem, ULONG fFlags);
void    __cdecl SfSignalSemaphore(PVOID hSem);
PVOID   __cdecl SfHeapAllocate(ULONG cb, ULONG fFlags);
void    __cdecl SfHeapFree(PVOID pv, ULONG fFlags);
PVOID   __cdecl SfCreateMutex(ULONG uBoost, ULONG fFlags);
void    __cdecl SfEnterMutex(PVOID hMutex, ULONG fFlags);
void    __cdecl SfLeaveMutex(PVOID hMutex);
int     __cdecl SfIfsGetVersion(void);
int     __cdecl SfIfsRegisterNet(pIFSFunc pfnConnect, ULONG uIfsVersion, ULONG uNetId);
dos_time __cdecl SfIfsWin32ToDosTime(_FILETIME ft);
_FILETIME __cdecl SfIfsDosToWin32Time(dos_time DosTime);
unsigned int __cdecl SfIfsMetaMatch(string_t pwszPattern, string_t pwszName, int fMatchSem);
unsigned int __cdecl SfIfsUniToBCS(unsigned char *pszDst, string_t pwszSrc, unsigned int cbSrc,
                                   unsigned int cbDstMax, unsigned int uCharSet);
unsigned int __cdecl SfIfsUniToUpper(string_t pwszDst, string_t pwszSrc, unsigned int cbSrc);
NTSTATUS __stdcall SfNtCreateFile(PHANDLE phFile, ULONG fAccess, SFOBJECTATTRIBUTES *pObjAttr,
                                  SFIOSTATUSBLOCK *pIos, PVOID pcbAlloc, ULONG fAttr, ULONG fShare,
                                  ULONG uDisposition, ULONG fOptions, PVOID pvEa, ULONG cbEa);
NTSTATUS __stdcall SfNtClose(HANDLE hFile);
NTSTATUS __stdcall SfNtDeviceIoControl(HANDLE hFile, HANDLE hEvent, PVOID pfnApc, PVOID pvApcCtx,
                                       SFIOSTATUSBLOCK *pIos, ULONG uCode, PVOID pvIn, ULONG cbIn,
                                       PVOID pvOut, ULONG cbOut);
/** NTKERN work item (WORK_QUEUE_ITEM). */
typedef struct SFWORKQUEUEITEM
{
    PVOID   apvList[2];
    void   (__stdcall *pfnWorker)(PVOID pvParameter);
    PVOID   pvParameter;
} SFWORKQUEUEITEM;
#define SF_DELAYED_WORK_QUEUE           1
void     __stdcall SfNtQueueWorkItem(SFWORKQUEUEITEM *pItem, ULONG enmQueue);
NTSTATUS __stdcall SfNtInternalDeviceIoControl(HANDLE hFile, HANDLE hEvent, PVOID pfnApc, PVOID pvApcCtx,
                                               SFIOSTATUSBLOCK *pIos, ULONG uCode, PVOID pvIn, ULONG cbIn,
                                               PVOID pvOut, ULONG cbOut);

/*
 * VBoxSF.c
 */
void    __cdecl SfLogRel(const char *pszFormat, ...);
void    SfMemZero(void *pv, ULONG cb);
void    SfMemCopy(void *pvDst, const void *pvSrc, ULONG cb);
UINT64  SfU64DivU32(UINT64 u64, UINT32 uDivisor);
UINT64  SfU64MulU32(UINT64 u64, UINT32 uFactor);
void    SfTimeToFileTime(INT64 nsSince1970, _FILETIME *pFileTime);
dos_time SfTimeToDosTime(INT64 nsSince1970);
INT64   SfDosTimeToTime(dos_time DosTime);
int     SfRcToWin32(int rc);
/** Serializes the resource and search tables. */
extern PVOID g_hSfMutex;
#define SF_LOCK()       SfEnterMutex(g_hSfMutex, 0)
#define SF_UNLOCK()     SfLeaveMutex(g_hSfMutex)

/*
 * Host.c: the VBoxSharedFolders client.
 */
/** An HGCM call being built: header, parameters, then the buffers. */
typedef struct SFHGCMMSG
{
    VBOXGUEST98HGCMCALL *pCall;
    ULONG                cbCall;
    ULONG                offNext;
    /** Make the call through the driver's direct entry point (paging I/O,
     *  where NTKERN returns from I/O controls without calling the driver). */
    BOOL                 fDirect;
} SFHGCMMSG;

/** Largest data a single SHFL_FN_READ or SHFL_FN_LIST call moves. */
#define SF_MAX_IO_CHUNK                 (64 * 1024)

int     SfHostConnect(BOOL fSystemContext);
void    SfHostLogMappings(void);
int     SfHostQueryNames(WCHAR *pwszBuf, ULONG cwcBuf, ULONG *pcwcRet);
int     SfHostMapFolder(const WCHAR *pwszName, ULONG cwcName, SHFLROOT *pidRoot);
int     SfHostUnmapFolder(SHFLROOT idRoot);
int     SfHostCreate(SHFLROOT idRoot, const SHFLSTRING *pPath, SHFLCREATEPARMS *pParms);
int     SfHostClose(SHFLROOT idRoot, SHFLHANDLE hHost);
int     SfHostRead(SHFLROOT idRoot, SHFLHANDLE hHost, UINT64 off, void *pvBuf, ULONG *pcb, BOOL fPaging);
int     SfHostList(SHFLROOT idRoot, SHFLHANDLE hHost, UINT32 fFlags, const SHFLSTRING *pFilter,
                   void *pvBuf, ULONG *pcbBuf, UINT32 *pcEntries);
int     SfHostInformation(SHFLROOT idRoot, SHFLHANDLE hHost, UINT32 fFlags, void *pvInfo, ULONG *pcbInfo);
int     SfHostWrite(SHFLROOT idRoot, SHFLHANDLE hHost, UINT64 off, const void *pvBuf, ULONG *pcb, BOOL fPaging);
int     SfHostSetFileSize(SHFLROOT idRoot, SHFLHANDLE hHost, UINT64 cbNew);
int     SfHostFlush(SHFLROOT idRoot, SHFLHANDLE hHost);
int     SfHostRemove(SHFLROOT idRoot, const SHFLSTRING *pPath, UINT32 fFlags);
int     SfHostRename(SHFLROOT idRoot, const SHFLSTRING *pSrc, const SHFLSTRING *pDst, UINT32 fFlags);
int     SfHostLock(SHFLROOT idRoot, SHFLHANDLE hHost, UINT64 off, UINT64 cb, UINT32 fFlags);

/*
 * Net.c: the IFSMgr network FSD, the volume functions.
 */
/** A connected shared folder (the IFSMgr resource handle, ir_rh). */
typedef struct SFRESOURCE
{
    struct SFRESOURCE *pNext;
    SHFLROOT idRoot;
    /** IFSMgr's resource handle (ir_fh of the first connect). */
    PVOID    pvIfsRes;
    /** The share name, upper case as IFSMgr parsed it. */
    ULONG    cwcName;
    WCHAR    wszName[SHFL_MAX_LEN];
} SFRESOURCE;

/** High half of a 64-bit lvalue (a shift would need __aullshr). */
#define SF_HI32(x64)        (((const ULONG *)&(x64))[1])

/** DOS attributes (FILE_ATTRIBUTE_XXX) of a host object. */
#define SF_DOS_ATTR(pInfo)  ((UCHAR)(((pInfo)->Attr.fMode >> RTFS_DOS_SHIFT) & 0x37))

int     __cdecl SfNetConnect(pioreq pir);
SHFLSTRING *SfPathToHost(SFRESOURCE *pRes, path_t ppath, ULONG cSkipLast);
SHFLSTRING *SfPathToHostKeepCase(SFRESOURCE *pRes, path_t ppath, ULONG fAttr, string_t pwszCaseName);
int     SfLookup(SFRESOURCE *pRes, path_t ppath, SHFLFSOBJINFO *pInfo);
int     SfSetInfo(SHFLROOT idRoot, SHFLHANDLE hHost, const SHFLFSOBJINFO *pInfo);
int     __cdecl SfNetBadFunction(pioreq pir);

/*
 * Names.c: 8.3 names and path translation.
 */
/** @name SfTranslatePath modes
 * @{ */
#define SF_PATH_HOST    0   /**< Generated aliases to long names, the rest as it is. */
#define SF_PATH_LONG    1   /**< Every element as the host's long name. */
#define SF_PATH_SHORT   2   /**< Every element as its 8.3 name. */
/** @} */
ULONG   SfTo83(const WCHAR *pwc, ULONG cwc, char *pszName);
BOOL    SfIsAlias(const WCHAR *pwc, ULONG cwc);
ULONG   SfGetShortName(const SHFLDIRINFO *pEntry, char *pszName, BOOL *pfMade);
SHFLSTRING *SfTranslatePath(SFRESOURCE *pRes, path_t ppath, ULONG cSkipLast, ULONG enmMode);
void    SfHostPathToParsed(const SHFLSTRING *pStr, path_t ppath);

/*
 * File.c: open files.
 */
int     __cdecl SfFileOpen(pioreq pir);

/*
 * Find.c: Win32 (FindFirstFile) and DOS (INT 21h 4Eh) searches.
 */
int     __cdecl SfFindOpen(pioreq pir);
int     __cdecl SfSearch(pioreq pir);

#endif /* !VBOXSF_H */
