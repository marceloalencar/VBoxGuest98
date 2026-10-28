/** @file
 * VBoxTray - Shared clipboard file transfers: pasting in the guest files
 * copied on the host, and on the host files copied in the guest.
 *
 * Follows the transfer protocol of upstream VBoxGuestR3LibClipboard.cpp
 * (VBOX_SHCL_GF_0_TRANSFERS, host 7.1 or later with clipboard file
 * transfers enabled for the VM).
 *
 * Host to guest: like upstream's Windows guest
 * (SharedClipboardWinDataObject), the files are handed over as virtual
 * files:
 *  - The host announces VBOX_SHCL_FMT_URI_LIST; the window thread puts a data
 *    object on the guest clipboard (OleSetClipboard) offering
 *    FileGroupDescriptor (CFSTR_FILEDESCRIPTORA) and FileContents.
 *  - The first FileGroupDescriptor request starts the transfer: the guest
 *    reports the status REQUESTED; the host answers REQUESTED with the
 *    transfer's context ID, the worker thread answers INITIALIZED and the
 *    host confirms with INITIALIZED (VBoxClipFileOnStatus).  The window
 *    thread then reads the root list and the directory lists; later requests
 *    reuse them (upstream's m_lstEntries), so looking at the clipboard (the
 *    Explorer context menu) costs only the listing.
 *  - A FileContents request opens the file on the host and returns a stream
 *    reading it while the application copies (OBJ_READ).
 *  - The transfer is reported COMPLETED (or ERROR) when the data object
 *    leaves the clipboard.
 *
 * Guest to host: Clipboard.c reports VBOX_SHCL_FMT_URI_LIST while the guest
 * clipboard has CF_HDROP.  When a host application pastes, the host reports
 * the status INITIALIZED; the worker thread takes the copied files as the
 * transfer's roots (upstream's ShClWinTransferGetRootsFromClipboard) and
 * answers the host's requests (VBoxClipFileOnHostMsg) like upstream's local
 * provider (clipboard-transfers-provider-local.cpp): one list entry per
 * LIST_ENTRY_READ, the direct children of a directory, and one chunk per
 * OBJ_READ.  Upstream's host only lists the roots and the directories among
 * them, so the folders inside a copied folder arrive empty on the host.
 *
 * Differences from upstream, because of Windows 98:
 *  - ANSI descriptors (FILEDESCRIPTORA); names are converted from UTF-8 to
 *    the ANSI code page, characters it doesn't have becoming '_'
 *    (VBoxDnDBuildPath).
 *  - The directories are read recursively when the transfer starts.
 *  - Guest to host, the host's paths are matched with the roots without
 *    regard to case, like the FAT file system does.
 *
 * The objects live on the window thread; other processes call them through
 * OLE marshaling, which calls in on that thread.
 */

#include "DnD.h"
#include <shlobj.h>
#include <stdlib.h>
#include <string.h>


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/

/** Timeout of each transfer call; the host may have to read a slow disk (ms). */
#define VBOXCLIPFILE_CALL_TIMEOUT_MS 60000
/** How long the first request waits for the host to set the transfer up (ms). */
#define VBOXCLIPFILE_SETUP_WAIT_MS  30000
/** Largest chunk read at once (upstream's default chunk size). */
#define VBOXCLIPFILE_CHUNK_SIZE     (64U * 1024)
/** Room for an entry name (SHCLLISTENTRY_MAX_NAME). */
#define VBOXCLIPFILE_MAX_NAME       4096
/** Room for a path the host sends (SHCL_TRANSFER_PATH_MAX, IPRT's
 *  RTPATH_MAX): the host sends its whole buffer. */
#define VBOXCLIPFILE_MAX_PATH       (4096 + 4)
/** Size of SHCLFSOBJINFO and the offsets used from it. */
#define VBOXCLIPFILE_INFO_SIZE      96
#define VBOXCLIPFILE_INFO_OFF_SIZE  0
#define VBOXCLIPFILE_INFO_OFF_ALLOC 8
#define VBOXCLIPFILE_INFO_OFF_ATIME 16
#define VBOXCLIPFILE_INFO_OFF_MTIME 24
#define VBOXCLIPFILE_INFO_OFF_CTIME 32
#define VBOXCLIPFILE_INFO_OFF_BTIME 40
#define VBOXCLIPFILE_INFO_OFF_MODE  48
#define VBOXCLIPFILE_INFO_OFF_ADD   52
/** SHCLFSOBJATTRADD_NOTHING (Attr.enmAdditional). */
#define VBOXCLIPFILE_ATTRADD_NOTHING 1
/** RTFS_TYPE_XXX of SHCLFSOBJINFO::Attr.fMode. */
#define VBOXCLIPFILE_TYPE_MASK      0170000U
#define VBOXCLIPFILE_TYPE_DIRECTORY 0040000U
#define VBOXCLIPFILE_TYPE_FILE      0100000U
/** The RTFS_DOS_XXX bits of fMode are the FAT attributes shifted by this. */
#define VBOXCLIPFILE_DOS_SHIFT      16
/** FAT attributes kept (read-only, hidden, system, directory, archive). */
#define VBOXCLIPFILE_DOS_MASK       0x37U
/** Directories and files the host may have open at once (guest to host). */
#define VBOXCLIPFILE_MAX_OPEN       16
/** 100 ns intervals from 1601 (FILETIME) to 1970 (RTTIMESPEC). */
#define VBOXCLIPFILE_FILETIME_1970  (11644473600i64 * 10000000)
/** Attempts and pause between them when another application has the
 *  clipboard open. */
#define VBOXCLIPFILE_SET_TRIES      10
#define VBOXCLIPFILE_SET_WAIT_MS    20
/** Number of formats the data object offers. */
#define VBOXCLIPFILE_FMT_COUNT      3


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/

/** The transfer being set up (g_ClipFile.CritSect). */
typedef struct VBOXCLIPFILESTATE
{
    CRITICAL_SECTION CritSect;
    /** Signalled when the host confirmed the transfer, or refused it. */
    HANDLE      hEvtReady;
    /** The host has VBOX_SHCL_HF_0_TRANSFERS. */
    BOOL        fEnabled;
    /** A request waits for its transfer to be set up. */
    BOOL        fWaiting;
    /** The host's REQUESTED came: idContext is valid. */
    BOOL        fRequested;
    /** The context ID of the transfer, used for all its calls. */
    UINT64      idContext;
    /** Outcome of the setup, VERR_TIMEOUT until known. */
    int         rcReady;
    /** Chunk size agreed with the host. */
    UINT32      cbChunk;
} VBOXCLIPFILESTATE;

/** A file or directory of the transfer. */
typedef struct VBOXCLIPFILEENTRY
{
    /** The path in the transfer (UTF-8, '/' separated: the host always takes
     *  UNIX style paths, ShClTransferTransformPath, and refuses a '\\' in a
     *  relative path). */
    char       *pszPath;
    /** The guest name relative to the paste target (ANSI, '\\' separated). */
    char        szName[MAX_PATH];
    BOOL        fDir;
    UINT64      cbObject;
    /** Modification time, ns since 1970. */
    INT64       nsModified;
} VBOXCLIPFILEENTRY;

/** Data object state. */
typedef enum VBOXCLIPFILEOBJSTATE
{
    /** Announced, the transfer isn't started yet. */
    VBOXCLIPFILEOBJSTATE_IDLE = 0,
    /** The transfer runs and the entries are listed. */
    VBOXCLIPFILEOBJSTATE_READY,
    /** Starting the transfer or listing failed (ERROR was reported). */
    VBOXCLIPFILEOBJSTATE_FAILED
} VBOXCLIPFILEOBJSTATE;

/** IDataObject implementation, one per host announcement. */
typedef struct VBOXCLIPFILEOBJ
{
    /** Must be first: the interface pointer is the object pointer. */
    IDataObject          Iface;
    LONG                 cRefs;
    HANDLE               hDevice;
    UINT32               idClient;
    /** The context ID of the transfer, 0 until the host made it. */
    UINT64               idContext;
    VBOXCLIPFILEOBJSTATE enmState;
    /** First read error, reported with the final status. */
    int                  rcRead;
    VBOXCLIPFILEENTRY   *paEntries;
    UINT32               cEntries;
    UINT32               cAlloc;
    /** For the log. */
    UINT32               cFilesRead;
    UINT64               cbRead;
} VBOXCLIPFILEOBJ;

/** IStream implementation reading one host file (FileContents). */
typedef struct VBOXCLIPFILESTREAM
{
    IStream              Iface;
    LONG                 cRefs;
    /** The data object, referenced. */
    VBOXCLIPFILEOBJ     *pObj;
    UINT32               iEntry;
    /** The host's object handle. */
    UINT64               hObj;
    /** Bytes read so far. */
    UINT64               off;
} VBOXCLIPFILESTREAM;

/** IEnumFORMATETC implementation (g_aClipFileFmts). */
typedef struct VBOXCLIPFILEENUMFMT
{
    IEnumFORMATETC       Iface;
    LONG                 cRefs;
    /** Index of the next format to return. */
    ULONG                iCur;
} VBOXCLIPFILEENUMFMT;

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

static VBOXCLIPFILESTATE g_ClipFile;
/** The clipboard formats: FileGroupDescriptor, FileContents and Preferred
 *  DropEffect (tells Explorer to copy, not move). */
static UINT g_cfFileDescriptor;
static UINT g_cfFileContents;
static UINT g_cfDropEffect;
/** The formats offered, in that order. */
static FORMATETC g_aClipFileFmts[VBOXCLIPFILE_FMT_COUNT];
/** The data object we put on the clipboard, referenced (window thread). */
static IDataObject *g_pClipFileObj;
/** OleSetClipboard is putting it there (window thread). */
static BOOL g_fClipFileSetting;
/** The transfer of guest files to the host. */
static VBOXCLIPFILESEND g_ClipFileSend;


/*********************************************************************************************************************************
*   HGCM calls                                                                                                                   *
*********************************************************************************************************************************/

/**
 * Makes a call, repeating it when interrupted.
 */
static int vboxClipFileCall(HANDLE hDevice, VBGLR3HGCMMSG *pMsg)
{
    int rc;
    do
        rc = VbglR3HgcmMsgCall(hDevice, pMsg);
    while (rc == VERR_INTERRUPTED);
    return rc;
}

/**
 * Reports a transfer status to the host (VBOX_SHCL_GUEST_FN_REPLY,
 * VBOX_SHCL_TX_REPLYMSGTYPE_TRANSFER_STATUS).
 *
 * @param   idContext   The transfer's context ID, 0 for REQUESTED.
 */
static int vboxClipFileSendStatus(HANDLE hDevice, UINT32 idClient, UINT64 idContext, UINT32 uStatus, int rcTransfer)
{
    VBGLR3HGCMMSG Msg;
    int rc;

    if (!VbglR3HgcmMsgInit(&Msg, idClient, VBOX_SHCL_GUEST_FN_REPLY, 5, VBOXCLIPFILE_CALL_TIMEOUT_MS, 0))
        return VERR_NO_MEMORY;
    VbglR3HgcmMsgSetU64(&Msg, 0, idContext);
    VbglR3HgcmMsgSetU32(&Msg, 1, VBOX_SHCL_TX_REPLYMSGTYPE_TRANSFER_STATUS);
    VbglR3HgcmMsgSetU32(&Msg, 2, (UINT32)rcTransfer);
    VbglR3HgcmMsgSetBuf(&Msg, 3, VBOXGUEST98_HGCM_PARM_BUFFER_IN, 0, NULL);
    VbglR3HgcmMsgSetU32(&Msg, 4, uStatus);
    rc = vboxClipFileCall(hDevice, &Msg);
    VbglR3HgcmMsgFree(&Msg);
    return rc;
}

/**
 * Reads an entry of the root list (index) or of a list (handle), with its
 * SHCLFSOBJINFO (ROOT_LIST_ENTRY_READ / LIST_ENTRY_READ).
 *
 * @param   pszName     Where to return the name (UTF-8), VBOXCLIPFILE_MAX_NAME.
 * @param   pfMode      Where to return the RTFS_TYPE_XXX bits.
 * @param   pcbObject   Where to return the size.
 * @param   pnsModified Where to return the modification time.
 */
static int vboxClipFileReadEntry(VBOXCLIPFILEOBJ *pObj, BOOL fRoot, UINT64 uIndexOrHandle, char *pszName,
                                 UINT32 *pfMode, UINT64 *pcbObject, INT64 *pnsModified)
{
    VBGLR3HGCMMSG Msg;
    BYTE *pbName;
    BYTE *pbInfo;
    DWORD cchName;
    int rc;

    if (!VbglR3HgcmMsgInit(&Msg, pObj->idClient,
                           fRoot ? VBOX_SHCL_GUEST_FN_ROOT_LIST_ENTRY_READ : VBOX_SHCL_GUEST_FN_LIST_ENTRY_READ, 6,
                           VBOXCLIPFILE_CALL_TIMEOUT_MS, VBOXCLIPFILE_MAX_NAME + VBOXCLIPFILE_INFO_SIZE))
        return VERR_NO_MEMORY;
    VbglR3HgcmMsgSetU64(&Msg, 0, pObj->idContext);
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
    pbName = VbglR3HgcmMsgSetBuf(&Msg, 3, VBOXGUEST98_HGCM_PARM_BUFFER_INOUT, VBOXCLIPFILE_MAX_NAME, NULL);
    VbglR3HgcmMsgSetU32(&Msg, 4, VBOXCLIPFILE_INFO_SIZE);
    pbInfo = VbglR3HgcmMsgSetBuf(&Msg, 5, VBOXGUEST98_HGCM_PARM_BUFFER_INOUT, VBOXCLIPFILE_INFO_SIZE, NULL);
    rc = vboxClipFileCall(pObj->hDevice, &Msg);
    if (RT_SUCCESS(rc))
    {
        cchName = VBoxDnDStrNLen(pbName, VbglR3HgcmMsgGetBufSize(&Msg, 3));
        if (cchName >= VBOXCLIPFILE_MAX_NAME)
            cchName = VBOXCLIPFILE_MAX_NAME - 1;
        memcpy(pszName, pbName, cchName);
        pszName[cchName] = '\0';
        if (VbglR3HgcmMsgGetBufSize(&Msg, 5) >= VBOXCLIPFILE_INFO_SIZE)
        {
            *pfMode      = *(const UINT32 *)&pbInfo[VBOXCLIPFILE_INFO_OFF_MODE] & VBOXCLIPFILE_TYPE_MASK;
            *pcbObject   = *(const UINT64 *)&pbInfo[VBOXCLIPFILE_INFO_OFF_SIZE];
            *pnsModified = *(const INT64 *)&pbInfo[VBOXCLIPFILE_INFO_OFF_MTIME];
        }
        else
            rc = VERR_INVALID_PARAMETER;
    }
    VbglR3HgcmMsgFree(&Msg);
    return rc;
}

/**
 * Sends a call whose parameters are the context, a 64-bit handle and, for
 * opening, a path and a 32-bit value (LIST_OPEN, LIST_CLOSE, OBJ_OPEN,
 * OBJ_CLOSE).
 *
 * @param   phHandle    In: the handle to close; out: the handle opened.
 */
static int vboxClipFileHandleCall(VBOXCLIPFILEOBJ *pObj, UINT32 uFunction, const char *pszPath, UINT64 *phHandle)
{
    VBGLR3HGCMMSG Msg;
    DWORD cbPath = pszPath ? (DWORD)strlen(pszPath) + 1 : 0;
    int rc;

    switch (uFunction)
    {
        case VBOX_SHCL_GUEST_FN_LIST_OPEN:
            /* VBoxShClListOpenMsg: context, flags, filter, path, handle. */
            if (!VbglR3HgcmMsgInit(&Msg, pObj->idClient, uFunction, 5, VBOXCLIPFILE_CALL_TIMEOUT_MS, cbPath + 1))
                return VERR_NO_MEMORY;
            VbglR3HgcmMsgSetU64(&Msg, 0, pObj->idContext);
            VbglR3HgcmMsgSetU32(&Msg, 1, 0);
            VbglR3HgcmMsgSetBuf(&Msg, 2, VBOXGUEST98_HGCM_PARM_BUFFER_IN, 1, "");
            VbglR3HgcmMsgSetBuf(&Msg, 3, VBOXGUEST98_HGCM_PARM_BUFFER_IN, cbPath, pszPath);
            VbglR3HgcmMsgSetU64(&Msg, 4, 0);
            break;
        case VBOX_SHCL_GUEST_FN_OBJ_OPEN:
            /* VBoxShClObjOpenMsg: context, handle, path, create flags. */
            if (!VbglR3HgcmMsgInit(&Msg, pObj->idClient, uFunction, 4, VBOXCLIPFILE_CALL_TIMEOUT_MS, cbPath))
                return VERR_NO_MEMORY;
            VbglR3HgcmMsgSetU64(&Msg, 0, pObj->idContext);
            VbglR3HgcmMsgSetU64(&Msg, 1, 0);
            VbglR3HgcmMsgSetBuf(&Msg, 2, VBOXGUEST98_HGCM_PARM_BUFFER_IN, cbPath, pszPath);
            VbglR3HgcmMsgSetU32(&Msg, 3, SHCL_OBJ_CF_ACCESS_READ | SHCL_OBJ_CF_ACCESS_DENYWRITE);
            break;
        default:
            /* LIST_CLOSE, OBJ_CLOSE: context, handle. */
            if (!VbglR3HgcmMsgInit(&Msg, pObj->idClient, uFunction, 2, VBOXCLIPFILE_CALL_TIMEOUT_MS, 0))
                return VERR_NO_MEMORY;
            VbglR3HgcmMsgSetU64(&Msg, 0, pObj->idContext);
            VbglR3HgcmMsgSetU64(&Msg, 1, *phHandle);
            break;
    }
    rc = vboxClipFileCall(pObj->hDevice, &Msg);
    if (RT_SUCCESS(rc) && uFunction == VBOX_SHCL_GUEST_FN_LIST_OPEN)
        *phHandle = VbglR3HgcmMsgGetU64(&Msg, 4);
    else if (RT_SUCCESS(rc) && uFunction == VBOX_SHCL_GUEST_FN_OBJ_OPEN)
        *phHandle = VbglR3HgcmMsgGetU64(&Msg, 1);
    VbglR3HgcmMsgFree(&Msg);
    return rc;
}

/**
 * Gets the number of entries of the root list (ROOT_LIST_HDR_READ) or of a
 * list (LIST_HDR_READ).
 */
static int vboxClipFileReadHdr(VBOXCLIPFILEOBJ *pObj, BOOL fRoot, UINT64 hList, UINT64 *pcEntries)
{
    VBGLR3HGCMMSG Msg;
    int rc;

    if (fRoot)
    {
        /* VBoxShClRootListHdrMsg: context, root flags, entries. */
        if (!VbglR3HgcmMsgInit(&Msg, pObj->idClient, VBOX_SHCL_GUEST_FN_ROOT_LIST_HDR_READ, 3,
                               VBOXCLIPFILE_CALL_TIMEOUT_MS, 0))
            return VERR_NO_MEMORY;
        VbglR3HgcmMsgSetU64(&Msg, 0, pObj->idContext);
        VbglR3HgcmMsgSetU32(&Msg, 1, 0);
        VbglR3HgcmMsgSetU64(&Msg, 2, 0);
    }
    else
    {
        /* VBoxShClListHdrMsg: context, handle, flags, features, objects, size. */
        if (!VbglR3HgcmMsgInit(&Msg, pObj->idClient, VBOX_SHCL_GUEST_FN_LIST_HDR_READ, 6,
                               VBOXCLIPFILE_CALL_TIMEOUT_MS, 0))
            return VERR_NO_MEMORY;
        VbglR3HgcmMsgSetU64(&Msg, 0, pObj->idContext);
        VbglR3HgcmMsgSetU64(&Msg, 1, hList);
        VbglR3HgcmMsgSetU32(&Msg, 2, 0);
        VbglR3HgcmMsgSetU32(&Msg, 3, 0);
        VbglR3HgcmMsgSetU64(&Msg, 4, 0);
        VbglR3HgcmMsgSetU64(&Msg, 5, 0);
    }
    rc = vboxClipFileCall(pObj->hDevice, &Msg);
    if (RT_SUCCESS(rc))
        *pcEntries = VbglR3HgcmMsgGetU64(&Msg, fRoot ? 2 : 4);
    VbglR3HgcmMsgFree(&Msg);
    return rc;
}

/**
 * Reads the next chunk of an open object (OBJ_READ).
 *
 * @param   pvDst       Where to put the data, cbToRead bytes.
 * @param   pcbRead     Where to return the bytes read, 0 at the end.
 */
static int vboxClipFileReadObj(VBOXCLIPFILEOBJ *pObj, UINT64 hObj, DWORD cbToRead, void *pvDst, DWORD *pcbRead)
{
    VBGLR3HGCMMSG Msg;
    BYTE *pbData;
    DWORD cbRead;
    int rc;

    /* VBoxShClObjReadWriteMsg: context, handle, size, data, checksum size, checksum. */
    if (!VbglR3HgcmMsgInit(&Msg, pObj->idClient, VBOX_SHCL_GUEST_FN_OBJ_READ, 6, VBOXCLIPFILE_CALL_TIMEOUT_MS,
                           cbToRead))
        return VERR_NO_MEMORY;
    VbglR3HgcmMsgSetU64(&Msg, 0, pObj->idContext);
    VbglR3HgcmMsgSetU64(&Msg, 1, hObj);
    VbglR3HgcmMsgSetU32(&Msg, 2, cbToRead);
    pbData = VbglR3HgcmMsgSetBuf(&Msg, 3, VBOXGUEST98_HGCM_PARM_BUFFER_OUT, cbToRead, NULL);
    VbglR3HgcmMsgSetU32(&Msg, 4, 0);
    VbglR3HgcmMsgSetBuf(&Msg, 5, VBOXGUEST98_HGCM_PARM_BUFFER_IN, 0, NULL);
    rc = vboxClipFileCall(pObj->hDevice, &Msg);
    if (RT_SUCCESS(rc))
    {
        cbRead = VbglR3HgcmMsgGetU32(&Msg, 2);
        if (cbRead > cbToRead)
            cbRead = cbToRead;
        memcpy(pvDst, pbData, cbRead);
        *pcbRead = cbRead;
    }
    VbglR3HgcmMsgFree(&Msg);
    return rc;
}


/*********************************************************************************************************************************
*   Listing                                                                                                                      *
*********************************************************************************************************************************/

/**
 * Adds a file or directory to the entries; anything else (symbolic links)
 * is skipped, like upstream.
 *
 * @param   pszPath     The path in the transfer (UTF-8, '/' separated).
 */
static int vboxClipFileAddEntry(VBOXCLIPFILEOBJ *pObj, const char *pszPath, UINT32 fMode, UINT64 cbObject,
                                INT64 nsModified)
{
    VBOXCLIPFILEENTRY *pEntry;
    int rc;

    if (fMode != VBOXCLIPFILE_TYPE_DIRECTORY && fMode != VBOXCLIPFILE_TYPE_FILE)
        return VINF_SUCCESS;
    if (pObj->cEntries == pObj->cAlloc)
    {
        UINT32 cNew = pObj->cAlloc ? pObj->cAlloc * 2 : 16;
        VBOXCLIPFILEENTRY *paNew = (VBOXCLIPFILEENTRY *)realloc(pObj->paEntries, cNew * sizeof(*paNew));
        if (!paNew)
            return VERR_NO_MEMORY;
        pObj->paEntries = paNew;
        pObj->cAlloc    = cNew;
    }
    pEntry = &pObj->paEntries[pObj->cEntries];
    rc = VBoxDnDBuildPath("", pszPath, (DWORD)strlen(pszPath), pEntry->szName);
    if (RT_FAILURE(rc))
        return rc;
    pEntry->pszPath = (char *)malloc(strlen(pszPath) + 1);
    if (!pEntry->pszPath)
        return VERR_NO_MEMORY;
    lstrcpy(pEntry->pszPath, pszPath);
    pEntry->fDir       = fMode == VBOXCLIPFILE_TYPE_DIRECTORY;
    pEntry->cbObject   = pEntry->fDir ? 0 : cbObject;
    pEntry->nsModified = nsModified;
    pObj->cEntries++;
    return VINF_SUCCESS;
}

/**
 * Adds everything in a host directory to the entries, subdirectories too.
 *
 * @param   pszPath     The directory's path in the transfer.
 */
static int vboxClipFileListDir(VBOXCLIPFILEOBJ *pObj, const char *pszPath)
{
    char *pszName;
    char *pszChild;
    UINT64 hList = 0;
    UINT64 cEntries = 0;
    UINT64 i;
    UINT64 cbObject;
    INT64 nsModified;
    UINT32 fMode;
    size_t cchPath = strlen(pszPath);
    int rc;

    pszName  = (char *)malloc(VBOXCLIPFILE_MAX_NAME);
    pszChild = (char *)malloc(cchPath + 1 + VBOXCLIPFILE_MAX_NAME);
    if (!pszName || !pszChild)
    {
        free(pszName);
        free(pszChild);
        return VERR_NO_MEMORY;
    }

    rc = vboxClipFileHandleCall(pObj, VBOX_SHCL_GUEST_FN_LIST_OPEN, pszPath, &hList);
    if (RT_SUCCESS(rc))
        rc = vboxClipFileReadHdr(pObj, FALSE, hList, &cEntries);
    for (i = 0; RT_SUCCESS(rc) && i < cEntries; i++)
    {
        rc = vboxClipFileReadEntry(pObj, FALSE, hList, pszName, &fMode, &cbObject, &nsModified);
        if (RT_FAILURE(rc))
            break;
        memcpy(pszChild, pszPath, cchPath);
        pszChild[cchPath] = '/';
        lstrcpy(&pszChild[cchPath + 1], pszName);
        rc = vboxClipFileAddEntry(pObj, pszChild, fMode, cbObject, nsModified);
        if (RT_SUCCESS(rc) && fMode == VBOXCLIPFILE_TYPE_DIRECTORY)
            rc = vboxClipFileListDir(pObj, pszChild);
    }
    if (hList)
        vboxClipFileHandleCall(pObj, VBOX_SHCL_GUEST_FN_LIST_CLOSE, NULL, &hList);
    free(pszName);
    free(pszChild);
    return rc;
}

/**
 * Lists everything the host put on its clipboard (the root list).
 */
static int vboxClipFileListRoots(VBOXCLIPFILEOBJ *pObj)
{
    char *pszName;
    UINT64 cRoots = 0;
    UINT64 i;
    UINT64 cbObject;
    INT64 nsModified;
    UINT32 fMode;
    int rc;

    pszName = (char *)malloc(VBOXCLIPFILE_MAX_NAME);
    if (!pszName)
        return VERR_NO_MEMORY;
    rc = vboxClipFileReadHdr(pObj, TRUE, 0, &cRoots);
    for (i = 0; RT_SUCCESS(rc) && i < cRoots; i++)
    {
        rc = vboxClipFileReadEntry(pObj, TRUE, i, pszName, &fMode, &cbObject, &nsModified);
        if (RT_SUCCESS(rc))
            rc = vboxClipFileAddEntry(pObj, pszName, fMode, cbObject, nsModified);
        if (RT_SUCCESS(rc) && fMode == VBOXCLIPFILE_TYPE_DIRECTORY)
            rc = vboxClipFileListDir(pObj, pszName);
    }
    free(pszName);
    return rc;
}

/**
 * Asks the host for the transfer and waits until the host and the worker
 * thread set it up.  Sets pObj->idContext once the host made it.
 */
static int vboxClipFileStart(VBOXCLIPFILEOBJ *pObj)
{
    int rc;

    EnterCriticalSection(&g_ClipFile.CritSect);
    g_ClipFile.fWaiting   = TRUE;
    g_ClipFile.fRequested = FALSE;
    g_ClipFile.rcReady    = VERR_TIMEOUT;
    ResetEvent(g_ClipFile.hEvtReady);
    LeaveCriticalSection(&g_ClipFile.CritSect);

    rc = vboxClipFileSendStatus(pObj->hDevice, pObj->idClient, 0, SHCLTRANSFERSTATUS_REQUESTED, VINF_SUCCESS);
    if (RT_SUCCESS(rc))
        WaitForSingleObject(g_ClipFile.hEvtReady, VBOXCLIPFILE_SETUP_WAIT_MS);

    EnterCriticalSection(&g_ClipFile.CritSect);
    if (RT_SUCCESS(rc))
        rc = g_ClipFile.rcReady;
    if (g_ClipFile.fRequested)
        pObj->idContext = g_ClipFile.idContext;
    g_ClipFile.fWaiting = FALSE;
    LeaveCriticalSection(&g_ClipFile.CritSect);
    return rc;
}

/**
 * Starts the transfer and lists the entries, the first time something asks.
 */
static HRESULT vboxClipFileObjPrepare(VBOXCLIPFILEOBJ *pThis)
{
    UINT32 cDirs = 0;
    UINT64 cbTotal = 0;
    UINT32 i;
    int rc;

    if (pThis->enmState == VBOXCLIPFILEOBJSTATE_READY)
        return S_OK;
    if (pThis->enmState == VBOXCLIPFILEOBJSTATE_FAILED)
        return E_FAIL;

    rc = vboxClipFileStart(pThis);
    if (RT_FAILURE(rc))
        VBoxTrayLogRel("Clipboard: the host didn't start the file transfer, rc=%d\n", rc);
    else
    {
        rc = vboxClipFileListRoots(pThis);
        if (RT_SUCCESS(rc) && !pThis->cEntries)
            rc = VERR_NO_DATA;
        if (RT_FAILURE(rc))
            VBoxTrayLogRel("Clipboard: listing the host files failed, rc=%d\n", rc);
    }
    if (RT_FAILURE(rc))
    {
        if (pThis->idContext)
            vboxClipFileSendStatus(pThis->hDevice, pThis->idClient, pThis->idContext, SHCLTRANSFERSTATUS_ERROR, rc);
        pThis->enmState = VBOXCLIPFILEOBJSTATE_FAILED;
        return E_FAIL;
    }

    for (i = 0; i < pThis->cEntries; i++)
    {
        if (pThis->paEntries[i].fDir)
            cDirs++;
        cbTotal += pThis->paEntries[i].cbObject;
    }
    VBoxTrayLogRel("Clipboard: the host offers %lu file(s) and %lu folder(s), %lu KB\n", pThis->cEntries - cDirs,
                   cDirs, (UINT32)(cbTotal / 1024));
    pThis->enmState = VBOXCLIPFILEOBJSTATE_READY;
    return S_OK;
}


/*********************************************************************************************************************************
*   IStream                                                                                                                      *
*********************************************************************************************************************************/

static HRESULT STDMETHODCALLTYPE vboxClipFileStreamQueryInterface(IStream *pIface, REFIID riid, void **ppv)
{
    if (   IsEqualIID(riid, &IID_IUnknown)
        || IsEqualIID(riid, &IID_ISequentialStream)
        || IsEqualIID(riid, &IID_IStream))
    {
        *ppv = pIface;
        pIface->lpVtbl->AddRef(pIface);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE vboxClipFileStreamAddRef(IStream *pIface)
{
    return (ULONG)InterlockedIncrement(&((VBOXCLIPFILESTREAM *)pIface)->cRefs);
}

static ULONG STDMETHODCALLTYPE vboxClipFileStreamRelease(IStream *pIface)
{
    VBOXCLIPFILESTREAM *pThis = (VBOXCLIPFILESTREAM *)pIface;
    LONG cRefs = InterlockedDecrement(&pThis->cRefs);
    if (cRefs == 0)
    {
        VBOXCLIPFILEOBJ *pObj = pThis->pObj;
        vboxClipFileHandleCall(pObj, VBOX_SHCL_GUEST_FN_OBJ_CLOSE, NULL, &pThis->hObj);
        if (pThis->off == pObj->paEntries[pThis->iEntry].cbObject)
            pObj->cFilesRead++;
        pObj->Iface.lpVtbl->Release(&pObj->Iface);
        free(pThis);
    }
    return (ULONG)cRefs;
}

/**
 * Reads from the host file until cb bytes or the end.
 */
static int vboxClipFileStreamReadIt(VBOXCLIPFILESTREAM *pThis, void *pv, ULONG cb, ULONG *pcbRead)
{
    VBOXCLIPFILEOBJ *pObj = pThis->pObj;
    UINT64 cbObject = pObj->paEntries[pThis->iEntry].cbObject;
    ULONG cbDone = 0;
    int rc = VINF_SUCCESS;

    while (cbDone < cb && pThis->off < cbObject)
    {
        DWORD cbChunk = cb - cbDone;
        DWORD cbRead = 0;
        if (cbChunk > g_ClipFile.cbChunk)
            cbChunk = g_ClipFile.cbChunk;
        if ((UINT64)cbChunk > cbObject - pThis->off)
            cbChunk = (DWORD)(cbObject - pThis->off);
        rc = vboxClipFileReadObj(pObj, pThis->hObj, cbChunk, (BYTE *)pv + cbDone, &cbRead);
        if (RT_FAILURE(rc))
            break;
        if (!cbRead)
            break; /* Shorter than listed. */
        cbDone     += cbRead;
        pThis->off += cbRead;
    }
    pObj->cbRead += cbDone;
    *pcbRead = cbDone;
    if (RT_FAILURE(rc))
    {
        VBoxTrayLogRel("Clipboard: reading the host file \"%s\" failed, rc=%d\n", pObj->paEntries[pThis->iEntry].szName, rc);
        if (RT_SUCCESS(pObj->rcRead))
            pObj->rcRead = rc;
    }
    return rc;
}

static HRESULT STDMETHODCALLTYPE vboxClipFileStreamRead(IStream *pIface, void *pv, ULONG cb, ULONG *pcbRead)
{
    ULONG cbRead = 0;
    int rc;

    if (!pv)
        return STG_E_INVALIDPOINTER;
    rc = vboxClipFileStreamReadIt((VBOXCLIPFILESTREAM *)pIface, pv, cb, &cbRead);
    if (pcbRead)
        *pcbRead = cbRead;
    return RT_SUCCESS(rc) ? S_OK : STG_E_READFAULT;
}

static HRESULT STDMETHODCALLTYPE vboxClipFileStreamWrite(IStream *pIface, const void *pv, ULONG cb, ULONG *pcbWritten)
{
    return STG_E_ACCESSDENIED;
}

static HRESULT STDMETHODCALLTYPE vboxClipFileStreamSeek(IStream *pIface, LARGE_INTEGER Move, DWORD dwOrigin,
                                                        ULARGE_INTEGER *pNewPos)
{
    VBOXCLIPFILESTREAM *pThis = (VBOXCLIPFILESTREAM *)pIface;

    /* The host files are read in order: only telling the position works. */
    if (   (dwOrigin == STREAM_SEEK_CUR && Move.QuadPart == 0)
        || (dwOrigin == STREAM_SEEK_SET && (UINT64)Move.QuadPart == pThis->off))
    {
        if (pNewPos)
            pNewPos->QuadPart = pThis->off;
        return S_OK;
    }
    return STG_E_INVALIDFUNCTION;
}

static HRESULT STDMETHODCALLTYPE vboxClipFileStreamSetSize(IStream *pIface, ULARGE_INTEGER cbNew)
{
    return E_NOTIMPL;
}

/**
 * Copies to another stream: the Windows 98 shell writes the pasted file this
 * way.
 */
static HRESULT STDMETHODCALLTYPE vboxClipFileStreamCopyTo(IStream *pIface, IStream *pDst, ULARGE_INTEGER cb,
                                                          ULARGE_INTEGER *pcbRead, ULARGE_INTEGER *pcbWritten)
{
    VBOXCLIPFILESTREAM *pThis = (VBOXCLIPFILESTREAM *)pIface;
    UINT64 cbLeft = cb.QuadPart;
    UINT64 cbReadAll = 0;
    UINT64 cbWrittenAll = 0;
    BYTE *pbBuf;
    HRESULT hr = S_OK;

    if (!pDst)
        return STG_E_INVALIDPOINTER;
    pbBuf = (BYTE *)malloc(VBOXCLIPFILE_CHUNK_SIZE);
    if (!pbBuf)
        return E_OUTOFMEMORY;
    while (cbLeft)
    {
        ULONG cbChunk = cbLeft > VBOXCLIPFILE_CHUNK_SIZE ? VBOXCLIPFILE_CHUNK_SIZE : (ULONG)cbLeft;
        ULONG cbRead = 0;
        ULONG cbWritten = 0;
        if (RT_FAILURE(vboxClipFileStreamReadIt(pThis, pbBuf, cbChunk, &cbRead)))
        {
            hr = STG_E_READFAULT;
            break;
        }
        if (!cbRead)
            break; /* The end. */
        cbReadAll += cbRead;
        hr = pDst->lpVtbl->Write(pDst, pbBuf, cbRead, &cbWritten);
        cbWrittenAll += cbWritten;
        if (FAILED(hr))
            break;
        if (cbWritten != cbRead)
        {
            hr = STG_E_MEDIUMFULL;
            break;
        }
        cbLeft -= cbRead;
    }
    free(pbBuf);
    if (pcbRead)
        pcbRead->QuadPart = cbReadAll;
    if (pcbWritten)
        pcbWritten->QuadPart = cbWrittenAll;
    return hr;
}

static HRESULT STDMETHODCALLTYPE vboxClipFileStreamCommit(IStream *pIface, DWORD grfCommitFlags)
{
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE vboxClipFileStreamRevert(IStream *pIface)
{
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE vboxClipFileStreamLockRegion(IStream *pIface, ULARGE_INTEGER off, ULARGE_INTEGER cb,
                                                              DWORD dwLockType)
{
    return STG_E_INVALIDFUNCTION;
}

static HRESULT STDMETHODCALLTYPE vboxClipFileStreamUnlockRegion(IStream *pIface, ULARGE_INTEGER off, ULARGE_INTEGER cb,
                                                                DWORD dwLockType)
{
    return STG_E_INVALIDFUNCTION;
}

static HRESULT STDMETHODCALLTYPE vboxClipFileStreamStat(IStream *pIface, STATSTG *pStat, DWORD grfStatFlag)
{
    VBOXCLIPFILESTREAM *pThis = (VBOXCLIPFILESTREAM *)pIface;

    if (!pStat)
        return STG_E_INVALIDPOINTER;
    memset(pStat, 0, sizeof(*pStat));
    pStat->type            = STGTY_STREAM;
    pStat->cbSize.QuadPart = pThis->pObj->paEntries[pThis->iEntry].cbObject;
    pStat->grfMode         = STGM_READ;
    return S_OK; /* No name (pwcsName NULL); the descriptor has it. */
}

static HRESULT STDMETHODCALLTYPE vboxClipFileStreamClone(IStream *pIface, IStream **ppStream)
{
    if (ppStream)
        *ppStream = NULL;
    return E_NOTIMPL;
}

static IStreamVtbl g_vboxClipFileStreamVtbl =
{
    vboxClipFileStreamQueryInterface,
    vboxClipFileStreamAddRef,
    vboxClipFileStreamRelease,
    vboxClipFileStreamRead,
    vboxClipFileStreamWrite,
    vboxClipFileStreamSeek,
    vboxClipFileStreamSetSize,
    vboxClipFileStreamCopyTo,
    vboxClipFileStreamCommit,
    vboxClipFileStreamRevert,
    vboxClipFileStreamLockRegion,
    vboxClipFileStreamUnlockRegion,
    vboxClipFileStreamStat,
    vboxClipFileStreamClone
};

/**
 * Opens a host file and creates its stream, with a reference held for the
 * caller.
 */
static HRESULT vboxClipFileStreamCreate(VBOXCLIPFILEOBJ *pObj, UINT32 iEntry, VBOXCLIPFILESTREAM **ppStream)
{
    VBOXCLIPFILESTREAM *pThis;
    int rc;

    pThis = (VBOXCLIPFILESTREAM *)calloc(1, sizeof(*pThis));
    if (!pThis)
        return E_OUTOFMEMORY;
    rc = vboxClipFileHandleCall(pObj, VBOX_SHCL_GUEST_FN_OBJ_OPEN, pObj->paEntries[iEntry].pszPath, &pThis->hObj);
    if (RT_FAILURE(rc))
    {
        VBoxTrayLogRel("Clipboard: opening the host file \"%s\" failed, rc=%d\n", pObj->paEntries[iEntry].szName, rc);
        if (RT_SUCCESS(pObj->rcRead))
            pObj->rcRead = rc;
        free(pThis);
        return STG_E_FILENOTFOUND;
    }
    pThis->Iface.lpVtbl = &g_vboxClipFileStreamVtbl;
    pThis->cRefs        = 1;
    pThis->pObj         = pObj;
    pThis->iEntry       = iEntry;
    pObj->Iface.lpVtbl->AddRef(&pObj->Iface);
    *ppStream = pThis;
    return S_OK;
}


/*********************************************************************************************************************************
*   IEnumFORMATETC                                                                                                               *
*********************************************************************************************************************************/

static IEnumFORMATETC *vboxClipFileEnumFmtCreate(ULONG iCur);

static HRESULT STDMETHODCALLTYPE vboxClipFileEnumFmtQueryInterface(IEnumFORMATETC *pIface, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IEnumFORMATETC))
    {
        *ppv = pIface;
        pIface->lpVtbl->AddRef(pIface);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE vboxClipFileEnumFmtAddRef(IEnumFORMATETC *pIface)
{
    return (ULONG)InterlockedIncrement(&((VBOXCLIPFILEENUMFMT *)pIface)->cRefs);
}

static ULONG STDMETHODCALLTYPE vboxClipFileEnumFmtRelease(IEnumFORMATETC *pIface)
{
    VBOXCLIPFILEENUMFMT *pThis = (VBOXCLIPFILEENUMFMT *)pIface;
    LONG cRefs = InterlockedDecrement(&pThis->cRefs);
    if (cRefs == 0)
        free(pThis);
    return (ULONG)cRefs;
}

static HRESULT STDMETHODCALLTYPE vboxClipFileEnumFmtNext(IEnumFORMATETC *pIface, ULONG celt, FORMATETC *rgelt,
                                                         ULONG *pceltFetched)
{
    VBOXCLIPFILEENUMFMT *pThis = (VBOXCLIPFILEENUMFMT *)pIface;
    ULONG cFetched = 0;

    if (!rgelt || (celt != 1 && !pceltFetched))
        return E_INVALIDARG;
    while (cFetched < celt && pThis->iCur < VBOXCLIPFILE_FMT_COUNT)
        rgelt[cFetched++] = g_aClipFileFmts[pThis->iCur++];
    if (pceltFetched)
        *pceltFetched = cFetched;
    return cFetched == celt ? S_OK : S_FALSE;
}

static HRESULT STDMETHODCALLTYPE vboxClipFileEnumFmtSkip(IEnumFORMATETC *pIface, ULONG celt)
{
    VBOXCLIPFILEENUMFMT *pThis = (VBOXCLIPFILEENUMFMT *)pIface;
    while (celt && pThis->iCur < VBOXCLIPFILE_FMT_COUNT)
    {
        pThis->iCur++;
        celt--;
    }
    return celt ? S_FALSE : S_OK;
}

static HRESULT STDMETHODCALLTYPE vboxClipFileEnumFmtReset(IEnumFORMATETC *pIface)
{
    ((VBOXCLIPFILEENUMFMT *)pIface)->iCur = 0;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE vboxClipFileEnumFmtClone(IEnumFORMATETC *pIface, IEnumFORMATETC **ppEnum)
{
    if (!ppEnum)
        return E_INVALIDARG;
    *ppEnum = vboxClipFileEnumFmtCreate(((VBOXCLIPFILEENUMFMT *)pIface)->iCur);
    return *ppEnum ? S_OK : E_OUTOFMEMORY;
}

static IEnumFORMATETCVtbl g_vboxClipFileEnumFmtVtbl =
{
    vboxClipFileEnumFmtQueryInterface,
    vboxClipFileEnumFmtAddRef,
    vboxClipFileEnumFmtRelease,
    vboxClipFileEnumFmtNext,
    vboxClipFileEnumFmtSkip,
    vboxClipFileEnumFmtReset,
    vboxClipFileEnumFmtClone
};

/**
 * Creates an enumerator of the formats, with a reference held for the caller.
 */
static IEnumFORMATETC *vboxClipFileEnumFmtCreate(ULONG iCur)
{
    VBOXCLIPFILEENUMFMT *pThis = (VBOXCLIPFILEENUMFMT *)calloc(1, sizeof(*pThis));
    if (!pThis)
        return NULL;
    pThis->Iface.lpVtbl = &g_vboxClipFileEnumFmtVtbl;
    pThis->cRefs        = 1;
    pThis->iCur         = iCur;
    return &pThis->Iface;
}


/*********************************************************************************************************************************
*   IDataObject                                                                                                                  *
*********************************************************************************************************************************/

static HRESULT STDMETHODCALLTYPE vboxClipFileObjQueryInterface(IDataObject *pIface, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDataObject))
    {
        *ppv = pIface;
        pIface->lpVtbl->AddRef(pIface);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE vboxClipFileObjAddRef(IDataObject *pIface)
{
    return (ULONG)InterlockedIncrement(&((VBOXCLIPFILEOBJ *)pIface)->cRefs);
}

static ULONG STDMETHODCALLTYPE vboxClipFileObjRelease(IDataObject *pIface)
{
    VBOXCLIPFILEOBJ *pThis = (VBOXCLIPFILEOBJ *)pIface;
    LONG cRefs = InterlockedDecrement(&pThis->cRefs);
    UINT32 i;

    if (cRefs == 0)
    {
        /* Off the clipboard and nobody reads any more: the transfer is over. */
        if (pThis->enmState == VBOXCLIPFILEOBJSTATE_READY)
        {
            vboxClipFileSendStatus(pThis->hDevice, pThis->idClient, pThis->idContext,
                                   RT_SUCCESS(pThis->rcRead) ? SHCLTRANSFERSTATUS_COMPLETED : SHCLTRANSFERSTATUS_ERROR,
                                   pThis->rcRead);
            VBoxTrayLogRel("Clipboard: file transfer done, %lu file(s), %lu KB read\n", pThis->cFilesRead,
                           (UINT32)(pThis->cbRead / 1024));
        }
        for (i = 0; i < pThis->cEntries; i++)
            free(pThis->paEntries[i].pszPath);
        free(pThis->paEntries);
        free(pThis);
    }
    return (ULONG)cRefs;
}

/**
 * Checks whether a FORMATETC asks for one of our formats.
 */
static HRESULT vboxClipFileObjCheckFormat(const FORMATETC *pFormatEtc)
{
    if (!pFormatEtc)
        return E_INVALIDARG;
    if (pFormatEtc->dwAspect != DVASPECT_CONTENT)
        return DV_E_DVASPECT;
    if (pFormatEtc->cfFormat == g_cfFileContents)
        return pFormatEtc->tymed & (TYMED_ISTREAM | TYMED_HGLOBAL) ? S_OK : DV_E_TYMED;
    if (pFormatEtc->cfFormat == g_cfFileDescriptor || pFormatEtc->cfFormat == g_cfDropEffect)
        return pFormatEtc->tymed & TYMED_HGLOBAL ? S_OK : DV_E_TYMED;
    return DV_E_FORMATETC;
}

/**
 * Makes the FileGroupDescriptor of the entries.
 */
static HGLOBAL vboxClipFileObjMakeDescriptor(VBOXCLIPFILEOBJ *pThis)
{
    FILEGROUPDESCRIPTORA *pGroup;
    HGLOBAL hMem;
    UINT32 i;

    hMem = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT,
                       sizeof(FILEGROUPDESCRIPTORA) + (pThis->cEntries - 1) * sizeof(FILEDESCRIPTORA));
    if (!hMem)
        return NULL;
    pGroup = (FILEGROUPDESCRIPTORA *)GlobalLock(hMem);
    if (!pGroup)
    {
        GlobalFree(hMem);
        return NULL;
    }
    pGroup->cItems = pThis->cEntries;
    for (i = 0; i < pThis->cEntries; i++)
    {
        const VBOXCLIPFILEENTRY *pEntry = &pThis->paEntries[i];
        FILEDESCRIPTORA *pDesc = &pGroup->fgd[i];
        pDesc->dwFlags          = FD_ATTRIBUTES | FD_FILESIZE;
        pDesc->dwFileAttributes = pEntry->fDir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
        pDesc->nFileSizeLow     = (DWORD)pEntry->cbObject;
        pDesc->nFileSizeHigh    = (DWORD)(pEntry->cbObject >> 32);
        if (pEntry->nsModified > 0)
        {
            UINT64 uTime = (UINT64)(pEntry->nsModified / 100) + VBOXCLIPFILE_FILETIME_1970;
            pDesc->ftLastWriteTime.dwLowDateTime  = (DWORD)uTime;
            pDesc->ftLastWriteTime.dwHighDateTime = (DWORD)(uTime >> 32);
            pDesc->dwFlags |= FD_WRITESTIME;
        }
        lstrcpyn(pDesc->cFileName, pEntry->szName, MAX_PATH);
    }
    GlobalUnlock(hMem);
    return hMem;
}

/**
 * Reads a whole host file into memory (FileContents asked as TYMED_HGLOBAL).
 */
static HRESULT vboxClipFileObjReadWhole(VBOXCLIPFILEOBJ *pThis, UINT32 iEntry, HGLOBAL *phMem)
{
    VBOXCLIPFILESTREAM *pStream;
    UINT64 cbObject = pThis->paEntries[iEntry].cbObject;
    HGLOBAL hMem;
    void *pv;
    ULONG cbRead = 0;
    HRESULT hr;
    int rc;

    if (cbObject > 0x7fffffff)
        return E_OUTOFMEMORY;
    hMem = GlobalAlloc(GMEM_MOVEABLE, cbObject ? (DWORD)cbObject : 1);
    if (!hMem)
        return E_OUTOFMEMORY;
    hr = vboxClipFileStreamCreate(pThis, iEntry, &pStream);
    if (SUCCEEDED(hr))
    {
        pv = GlobalLock(hMem);
        rc = pv ? vboxClipFileStreamReadIt(pStream, pv, (ULONG)cbObject, &cbRead) : VERR_NO_MEMORY;
        if (pv)
            GlobalUnlock(hMem);
        if (RT_FAILURE(rc))
            hr = STG_E_READFAULT;
        pStream->Iface.lpVtbl->Release(&pStream->Iface);
    }
    if (FAILED(hr))
    {
        GlobalFree(hMem);
        return hr;
    }
    *phMem = hMem;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE vboxClipFileObjGetData(IDataObject *pIface, FORMATETC *pFormatEtc, STGMEDIUM *pMedium)
{
    VBOXCLIPFILEOBJ *pThis = (VBOXCLIPFILEOBJ *)pIface;
    HGLOBAL hMem = NULL;
    HRESULT hr;

    if (!pMedium)
        return E_INVALIDARG;
    hr = vboxClipFileObjCheckFormat(pFormatEtc);
    if (FAILED(hr))
        return hr;
    memset(pMedium, 0, sizeof(*pMedium));

    if (pFormatEtc->cfFormat == g_cfDropEffect)
    {
        hMem = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
        if (!hMem)
            return E_OUTOFMEMORY;
        *(DWORD *)GlobalLock(hMem) = DROPEFFECT_COPY;
        GlobalUnlock(hMem);
    }
    else if (pFormatEtc->cfFormat == g_cfFileDescriptor)
    {
        hr = vboxClipFileObjPrepare(pThis);
        if (FAILED(hr))
            return hr;
        hMem = vboxClipFileObjMakeDescriptor(pThis);
        if (!hMem)
            return E_OUTOFMEMORY;
    }
    else
    {
        /* FileContents: lindex is the descriptor's index. */
        UINT32 iEntry = (UINT32)pFormatEtc->lindex;
        hr = vboxClipFileObjPrepare(pThis);
        if (FAILED(hr))
            return hr;
        if (pFormatEtc->lindex < 0 || iEntry >= pThis->cEntries || pThis->paEntries[iEntry].fDir)
            return DV_E_LINDEX;
        if (pFormatEtc->tymed & TYMED_ISTREAM)
        {
            VBOXCLIPFILESTREAM *pStream;
            hr = vboxClipFileStreamCreate(pThis, iEntry, &pStream);
            if (FAILED(hr))
                return hr;
            pMedium->tymed   = TYMED_ISTREAM;
            pMedium->pstm    = &pStream->Iface;
            return S_OK;
        }
        hr = vboxClipFileObjReadWhole(pThis, iEntry, &hMem);
        if (FAILED(hr))
            return hr;
    }
    pMedium->tymed   = TYMED_HGLOBAL;
    pMedium->hGlobal = hMem;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE vboxClipFileObjGetDataHere(IDataObject *pIface, FORMATETC *pFormatEtc,
                                                            STGMEDIUM *pMedium)
{
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE vboxClipFileObjQueryGetData(IDataObject *pIface, FORMATETC *pFormatEtc)
{
    return vboxClipFileObjCheckFormat(pFormatEtc);
}

static HRESULT STDMETHODCALLTYPE vboxClipFileObjGetCanonicalFormatEtc(IDataObject *pIface, FORMATETC *pFormatEtcIn,
                                                                      FORMATETC *pFormatEtcOut)
{
    if (pFormatEtcOut)
        pFormatEtcOut->ptd = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE vboxClipFileObjSetData(IDataObject *pIface, FORMATETC *pFormatEtc, STGMEDIUM *pMedium,
                                                        BOOL fRelease)
{
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE vboxClipFileObjEnumFormatEtc(IDataObject *pIface, DWORD dwDirection,
                                                              IEnumFORMATETC **ppEnum)
{
    if (!ppEnum)
        return E_INVALIDARG;
    *ppEnum = NULL;
    if (dwDirection != DATADIR_GET)
        return E_NOTIMPL;
    *ppEnum = vboxClipFileEnumFmtCreate(0);
    return *ppEnum ? S_OK : E_OUTOFMEMORY;
}

static HRESULT STDMETHODCALLTYPE vboxClipFileObjDAdvise(IDataObject *pIface, FORMATETC *pFormatEtc, DWORD fAdvf,
                                                        IAdviseSink *pAdvSink, DWORD *pdwConnection)
{
    return OLE_E_ADVISENOTSUPPORTED;
}

static HRESULT STDMETHODCALLTYPE vboxClipFileObjDUnadvise(IDataObject *pIface, DWORD dwConnection)
{
    return OLE_E_ADVISENOTSUPPORTED;
}

static HRESULT STDMETHODCALLTYPE vboxClipFileObjEnumDAdvise(IDataObject *pIface, IEnumSTATDATA **ppEnumAdvise)
{
    return OLE_E_ADVISENOTSUPPORTED;
}

static IDataObjectVtbl g_vboxClipFileObjVtbl =
{
    vboxClipFileObjQueryInterface,
    vboxClipFileObjAddRef,
    vboxClipFileObjRelease,
    vboxClipFileObjGetData,
    vboxClipFileObjGetDataHere,
    vboxClipFileObjQueryGetData,
    vboxClipFileObjGetCanonicalFormatEtc,
    vboxClipFileObjSetData,
    vboxClipFileObjEnumFormatEtc,
    vboxClipFileObjDAdvise,
    vboxClipFileObjDUnadvise,
    vboxClipFileObjEnumDAdvise
};


/*********************************************************************************************************************************
*   Guest to host                                                                                                                *
*********************************************************************************************************************************/

/**
 * Turns a Win32 error from a file operation into a VBox status code.
 */
static int vboxClipFileWinRc(DWORD dwErr)
{
    switch (dwErr)
    {
        case ERROR_FILE_NOT_FOUND:      return VERR_FILE_NOT_FOUND;
        case ERROR_PATH_NOT_FOUND:      return VERR_PATH_NOT_FOUND;
        case ERROR_ACCESS_DENIED:       return VERR_ACCESS_DENIED;
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:      return VERR_SHARING_VIOLATION;
        case ERROR_TOO_MANY_OPEN_FILES: return VERR_TOO_MANY_OPEN_FILES;
        case ERROR_INVALID_NAME:        return VERR_INVALID_NAME;
        default:                        return VERR_READ_ERROR;
    }
}

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
    rc = vboxClipFileCall(hDevice, &Msg);
    pReq->idContext = VbglR3HgcmMsgGetU64(&Msg, 0);
    pReq->u64       = iU64 ? VbglR3HgcmMsgGetU64(&Msg, iU64) : 0;
    pReq->u32       = iU32 ? VbglR3HgcmMsgGetU32(&Msg, iU32) : 0;
    pReq->szPath[0] = '\0';
    if (RT_SUCCESS(rc) && pbPath)
    {
        cch = VBoxDnDStrNLen(pbPath, VbglR3HgcmMsgGetBufSize(&Msg, iPath));
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
    rc = vboxClipFileCall(hDevice, &Msg);
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
    rc = vboxClipFileCall(hDevice, &Msg);
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
    rc = vboxClipFileCall(hDevice, &Msg);
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
    rc = vboxClipFileCall(hDevice, &Msg);
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
            rc = vboxClipFileWinRc(GetLastError());
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
                rc = vboxClipFileWinRc(dwErr);
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
        return vboxClipFileWinRc(GetLastError());
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
            rc = vboxClipFileWinRc(GetLastError());
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
        rc = vboxClipFileWinRc(GetLastError());
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
 * Prepares the file transfers, before any thread runs.
 */
void VBoxClipFileInit(void)
{
    static const DWORD s_afTymed[VBOXCLIPFILE_FMT_COUNT] = { TYMED_HGLOBAL, TYMED_ISTREAM, TYMED_HGLOBAL };
    unsigned i;

    InitializeCriticalSection(&g_ClipFile.CritSect);
    g_ClipFile.hEvtReady = CreateEvent(NULL, TRUE, FALSE, NULL);
    g_ClipFile.cbChunk   = VBOXCLIPFILE_CHUNK_SIZE;

    g_cfFileDescriptor = RegisterClipboardFormat(CFSTR_FILEDESCRIPTORA);
    g_cfFileContents   = RegisterClipboardFormat(CFSTR_FILECONTENTS);
    g_cfDropEffect     = RegisterClipboardFormat(CFSTR_PREFERREDDROPEFFECT);
    g_aClipFileFmts[0].cfFormat = (CLIPFORMAT)g_cfFileDescriptor;
    g_aClipFileFmts[1].cfFormat = (CLIPFORMAT)g_cfFileContents;
    g_aClipFileFmts[2].cfFormat = (CLIPFORMAT)g_cfDropEffect;
    for (i = 0; i < VBOXCLIPFILE_FMT_COUNT; i++)
    {
        g_aClipFileFmts[i].ptd      = NULL;
        g_aClipFileFmts[i].dwAspect = DVASPECT_CONTENT;
        g_aClipFileFmts[i].lindex   = -1;
        g_aClipFileFmts[i].tymed    = s_afTymed[i];
    }
}

/**
 * Tells whether the host does file transfers; called by the worker thread
 * after reporting the features.  Agrees on the chunk size if it does
 * (VBOX_SHCL_GUEST_FN_NEGOTIATE_CHUNK_SIZE).
 */
void VBoxClipFileSetEnabled(HANDLE hDevice, UINT32 idClient, BOOL fEnabled)
{
    VBGLR3HGCMMSG Msg;
    UINT32 cbChunk = VBOXCLIPFILE_CHUNK_SIZE;

    if (   fEnabled
        && VbglR3HgcmMsgInit(&Msg, idClient, VBOX_SHCL_GUEST_FN_NEGOTIATE_CHUNK_SIZE, 2, VBOXCLIPFILE_CALL_TIMEOUT_MS, 0))
    {
        VbglR3HgcmMsgSetU32(&Msg, 0, VBOXCLIPFILE_CHUNK_SIZE);
        VbglR3HgcmMsgSetU32(&Msg, 1, 0); /* Let the host choose. */
        if (RT_SUCCESS(vboxClipFileCall(hDevice, &Msg)))
        {
            cbChunk = VbglR3HgcmMsgGetU32(&Msg, 1);
            if (!cbChunk || cbChunk > VBOXCLIPFILE_CHUNK_SIZE)
                cbChunk = VBOXCLIPFILE_CHUNK_SIZE;
        }
        VbglR3HgcmMsgFree(&Msg);
    }

    EnterCriticalSection(&g_ClipFile.CritSect);
    g_ClipFile.fEnabled = fEnabled;
    g_ClipFile.cbChunk  = cbChunk;
    LeaveCriticalSection(&g_ClipFile.CritSect);
}

/**
 * Whether host files can be pasted.
 */
BOOL VBoxClipFileEnabled(void)
{
    BOOL fEnabled;
    EnterCriticalSection(&g_ClipFile.CritSect);
    fEnabled = g_ClipFile.fEnabled;
    LeaveCriticalSection(&g_ClipFile.CritSect);
    return fEnabled;
}

/**
 * Handles VBOX_SHCL_HOST_MSG_TRANSFER_STATUS (worker thread): fetches it and
 * moves the transfer being set up along.
 */
void VBoxClipFileOnStatus(HANDLE hDevice, UINT32 idClient)
{
    VBGLR3HGCMMSG Msg;
    UINT64 idContext;
    UINT32 enmDir;
    UINT32 uStatus;
    int rcHost;
    int rc;

    /* VBoxShClTransferStatusMsg: context, direction, status, rc, flags. */
    if (!VbglR3HgcmMsgInit(&Msg, idClient, VBOX_SHCL_GUEST_FN_MSG_GET, 5, VBOXCLIPFILE_CALL_TIMEOUT_MS, 0))
        return;
    VbglR3HgcmMsgSetU64(&Msg, 0, VBOX_SHCL_HOST_MSG_TRANSFER_STATUS);
    VbglR3HgcmMsgSetU32(&Msg, 1, 0);
    VbglR3HgcmMsgSetU32(&Msg, 2, 0);
    VbglR3HgcmMsgSetU32(&Msg, 3, 0);
    VbglR3HgcmMsgSetU32(&Msg, 4, 0);
    rc = vboxClipFileCall(hDevice, &Msg);
    idContext = VbglR3HgcmMsgGetU64(&Msg, 0);
    enmDir    = VbglR3HgcmMsgGetU32(&Msg, 1);
    uStatus   = VbglR3HgcmMsgGetU32(&Msg, 2);
    rcHost    = (int)VbglR3HgcmMsgGetU32(&Msg, 3);
    VbglR3HgcmMsgFree(&Msg);
    if (RT_FAILURE(rc))
    {
        VBoxTrayLogRel("Clipboard: fetching a transfer status failed, rc=%d\n", rc);
        return;
    }

    EnterCriticalSection(&g_ClipFile.CritSect);
    if (uStatus == SHCLTRANSFERSTATUS_REQUESTED && g_ClipFile.fWaiting && !g_ClipFile.fRequested)
    {
        /* The host made the transfer we asked for; it waits for INITIALIZED. */
        g_ClipFile.fRequested = TRUE;
        g_ClipFile.idContext  = idContext;
        LeaveCriticalSection(&g_ClipFile.CritSect);
        rc = vboxClipFileSendStatus(hDevice, idClient, idContext, SHCLTRANSFERSTATUS_INITIALIZED, VINF_SUCCESS);
        if (RT_FAILURE(rc))
        {
            EnterCriticalSection(&g_ClipFile.CritSect);
            g_ClipFile.rcReady = rc;
            SetEvent(g_ClipFile.hEvtReady);
            LeaveCriticalSection(&g_ClipFile.CritSect);
        }
        return;
    }
    if (   g_ClipFile.fWaiting
        && g_ClipFile.fRequested
        && VBOX_SHCL_CONTEXTID_GET_TRANSFER(idContext) == VBOX_SHCL_CONTEXTID_GET_TRANSFER(g_ClipFile.idContext))
    {
        if (uStatus == SHCLTRANSFERSTATUS_INITIALIZED || uStatus == SHCLTRANSFERSTATUS_STARTED)
            g_ClipFile.rcReady = VINF_SUCCESS;
        else
            g_ClipFile.rcReady = RT_FAILURE(rcHost) ? rcHost : VERR_CANCELLED;
        SetEvent(g_ClipFile.hEvtReady);
        LeaveCriticalSection(&g_ClipFile.CritSect);
        return;
    }
    LeaveCriticalSection(&g_ClipFile.CritSect);

    /* A host application pastes guest files (direction from the host's point
       of view): set the transfer up and tell the host, which then asks for
       the roots. */
    if (uStatus == SHCLTRANSFERSTATUS_INITIALIZED && enmDir == SHCLTRANSFERDIR_FROM_REMOTE)
    {
        rc = vboxClipFileSendBegin(idContext);
        if (RT_FAILURE(rc))
            VBoxTrayLogRel("Clipboard: cannot give the guest files to the host, rc=%d\n", rc);
        vboxClipFileSendStatus(hDevice, idClient, idContext,
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
            vboxClipFileSendStatus(hDevice, idClient, g_ClipFileSend.idContext, SHCLTRANSFERSTATUS_ERROR, rc);
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
        vboxClipFileSendStatus(hDevice, idClient, pReq->idContext, SHCLTRANSFERSTATUS_ERROR, rc);
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

/**
 * The host announced files (window thread): puts a new data object for them
 * on the guest clipboard, replacing whatever is there.
 *
 * @returns TRUE if the clipboard has it.
 */
BOOL VBoxClipFileAnnounce(HANDLE hDevice, UINT32 idClient)
{
    VBOXCLIPFILEOBJ *pThis;
    IDataObject *pOld = g_pClipFileObj;
    HRESULT hr = E_FAIL;
    unsigned i;

    pThis = (VBOXCLIPFILEOBJ *)calloc(1, sizeof(*pThis));
    if (!pThis)
        return FALSE;
    pThis->Iface.lpVtbl = &g_vboxClipFileObjVtbl;
    pThis->cRefs        = 1;
    pThis->hDevice      = hDevice;
    pThis->idClient     = idClient;
    pThis->enmState     = VBOXCLIPFILEOBJSTATE_IDLE;
    pThis->rcRead       = VINF_SUCCESS;

    /* OleSetClipboard tells the clipboard viewers, us too, before returning:
       VBoxClipFileIsCurrent must say yes meanwhile. */
    g_pClipFileObj     = &pThis->Iface;
    g_fClipFileSetting = TRUE;
    for (i = 0; i < VBOXCLIPFILE_SET_TRIES; i++)
    {
        hr = OleSetClipboard(&pThis->Iface);
        if (hr != CLIPBRD_E_CANT_OPEN)
            break;
        Sleep(VBOXCLIPFILE_SET_WAIT_MS);
    }
    g_fClipFileSetting = FALSE;
    if (FAILED(hr))
    {
        VBoxTrayLogRel("Clipboard: cannot put the host files on the clipboard, hr=%#lx\n", hr);
        g_pClipFileObj = pOld;
        pThis->Iface.lpVtbl->Release(&pThis->Iface);
        return FALSE;
    }
    if (pOld)
        pOld->lpVtbl->Release(pOld);
    return TRUE;
}

/**
 * Whether the guest clipboard holds our data object (window thread).
 */
BOOL VBoxClipFileIsCurrent(void)
{
    return g_fClipFileSetting || (g_pClipFileObj && OleIsCurrentClipboard(g_pClipFileObj) == S_OK);
}

/**
 * The guest clipboard has something else (window thread): drops our
 * reference, which ends the transfer once nobody reads any more.
 */
void VBoxClipFileForget(void)
{
    IDataObject *pObj = g_pClipFileObj;
    g_pClipFileObj = NULL;
    if (pObj)
        pObj->lpVtbl->Release(pObj);
}

/**
 * VBoxTray is going away (window thread): takes the data object off the
 * clipboard, so OLE doesn't render the files for keeps (OleFlushClipboard),
 * like upstream empties the clipboard.
 */
void VBoxClipFileTerm(void)
{
    if (VBoxClipFileIsCurrent())
        OleSetClipboard(NULL);
    VBoxClipFileForget();
}
