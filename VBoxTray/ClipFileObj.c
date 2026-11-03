/** @file
 * VBoxTray - Shared clipboard file transfers, host to guest: the data object
 * offering the host files as virtual files (FileGroupDescriptor and
 * FileContents), the listing and the progress window.  See ClipFile.c for
 * the protocol.
 *
 * The objects live on the window thread; other processes call them through
 * OLE marshaling, which calls in on that thread.
 */

#include "ClipFile.h"
#include <shlobj.h>
#include <shlwapi.h>
#include <stdlib.h>
#include <string.h>


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/

/** How long the first request waits for the host to set the transfer up (ms). */
#define VBOXCLIPFILE_SETUP_WAIT_MS  30000
/** Room for an entry name (SHCLLISTENTRY_MAX_NAME). */
#define VBOXCLIPFILE_MAX_NAME       4096
/** Asks the shell for a progress window (shell 5.0 and later; newer than
 *  the Visual C++ 6.0 headers). */
#ifndef FD_PROGRESSUI
# define FD_PROGRESSUI              0x4000
#endif
/** The progress window closes when nothing was read for this long (ms), so
 *  a paste that stops early doesn't leave it open. */
#define VBOXCLIPFILE_PROGRESS_IDLE_MS 3000
/** The timer checking that, on the window thread (ms). */
#define VBOXCLIPFILE_PROGRESS_TIMER_MS 1000
/** PROGDLG_XXX: estimate the time left. */
#define VBOXCLIPFILE_PROGDLG_AUTOTIME 0x00000002
/** The shell's file copy animation (AVI resource of SHELL32.DLL). */
#define VBOXCLIPFILE_AVI_FILECOPY   161
/** Attempts and pause between them when another application has the
 *  clipboard open. */
#define VBOXCLIPFILE_SET_TRIES      10
#define VBOXCLIPFILE_SET_WAIT_MS    20
/** Number of formats the data object offers. */
#define VBOXCLIPFILE_FMT_COUNT      3


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/

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

/** IProgressDialog (shlobj.h of later SDKs; not in the Visual C++ 6.0 one). */
typedef struct VBOXPROGRESSDIALOG VBOXPROGRESSDIALOG;
typedef struct VBOXPROGRESSDIALOGVTBL
{
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(VBOXPROGRESSDIALOG *pThis, REFIID riid, void **ppv);
    ULONG   (STDMETHODCALLTYPE *AddRef)(VBOXPROGRESSDIALOG *pThis);
    ULONG   (STDMETHODCALLTYPE *Release)(VBOXPROGRESSDIALOG *pThis);
    HRESULT (STDMETHODCALLTYPE *StartProgressDialog)(VBOXPROGRESSDIALOG *pThis, HWND hwndParent, IUnknown *punkEnableModless,
                                                     DWORD dwFlags, LPCVOID pvReserved);
    HRESULT (STDMETHODCALLTYPE *StopProgressDialog)(VBOXPROGRESSDIALOG *pThis);
    HRESULT (STDMETHODCALLTYPE *SetTitle)(VBOXPROGRESSDIALOG *pThis, LPCWSTR pwzTitle);
    HRESULT (STDMETHODCALLTYPE *SetAnimation)(VBOXPROGRESSDIALOG *pThis, HINSTANCE hInstAnimation, UINT idAnimation);
    BOOL    (STDMETHODCALLTYPE *HasUserCancelled)(VBOXPROGRESSDIALOG *pThis);
    HRESULT (STDMETHODCALLTYPE *SetProgress)(VBOXPROGRESSDIALOG *pThis, DWORD dwCompleted, DWORD dwTotal);
    HRESULT (STDMETHODCALLTYPE *SetProgress64)(VBOXPROGRESSDIALOG *pThis, ULONGLONG ullCompleted, ULONGLONG ullTotal);
    HRESULT (STDMETHODCALLTYPE *SetLine)(VBOXPROGRESSDIALOG *pThis, DWORD dwLineNum, LPCWSTR pwzString, BOOL fCompactPath,
                                         LPCVOID pvReserved);
    HRESULT (STDMETHODCALLTYPE *SetCancelMsg)(VBOXPROGRESSDIALOG *pThis, LPCWSTR pwzCancelMsg, LPCVOID pvReserved);
    HRESULT (STDMETHODCALLTYPE *Timer)(VBOXPROGRESSDIALOG *pThis, DWORD dwTimerAction, LPCVOID pvReserved);
} VBOXPROGRESSDIALOGVTBL;
struct VBOXPROGRESSDIALOG
{
    const VBOXPROGRESSDIALOGVTBL *lpVtbl;
};

/** Our progress window for shells without one (window thread only). */
typedef struct VBOXCLIPFILEPROGRESS
{
    /** Whether the shell version was checked, and the outcome: the shell
     *  shows no progress for virtual files. */
    BOOL                 fChecked;
    BOOL                 fNeeded;
    /** The window while it is open. */
    VBOXPROGRESSDIALOG  *pDlg;
    /** The user canceled: reads fail until the paste is over (idle). */
    BOOL                 fCanceled;
    /** Bytes of all the files and read so far in this paste. */
    UINT64               cbTotal;
    UINT64               cbDone;
    /** GetTickCount of the last read. */
    DWORD                msLast;
    UINT                 idTimer;
} VBOXCLIPFILEPROGRESS;


/*********************************************************************************************************************************
*   Global Variables                                                                                                             *
*********************************************************************************************************************************/

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
/** The progress window for host files pasted in the guest. */
static VBOXCLIPFILEPROGRESS g_ClipFileProgress;
static const CLSID g_vboxClsidProgressDialog =
    { 0xf8383852, 0xfcd3, 0x11d1, { 0xa6, 0xb9, 0x00, 0x60, 0x97, 0xdf, 0x5b, 0xd4 } };
static const IID g_vboxIidProgressDialog =
    { 0xebbc7c04, 0x315e, 0x11d2, { 0xb6, 0x2f, 0x00, 0x60, 0x97, 0xdf, 0x5b, 0xd4 } };


/*********************************************************************************************************************************
*   HGCM calls                                                                                                                   *
*********************************************************************************************************************************/

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
    rc = VBoxClipFileCall(pObj->hDevice, &Msg);
    if (RT_SUCCESS(rc))
    {
        cchName = VBoxTrayStrNLen(pbName, VbglR3HgcmMsgGetBufSize(&Msg, 3));
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
    rc = VBoxClipFileCall(pObj->hDevice, &Msg);
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
    rc = VBoxClipFileCall(pObj->hDevice, &Msg);
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
    rc = VBoxClipFileCall(pObj->hDevice, &Msg);
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
    rc = VBoxTrayBuildPath("", pszPath, (DWORD)strlen(pszPath), pEntry->szName);
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

    rc = VBoxClipFileReportStatus(pObj->hDevice, pObj->idClient, 0, SHCLTRANSFERSTATUS_REQUESTED, VINF_SUCCESS);
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
            VBoxClipFileReportStatus(pThis->hDevice, pThis->idClient, pThis->idContext, SHCLTRANSFERSTATUS_ERROR, rc);
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
*   Progress window                                                                                                              *
*********************************************************************************************************************************/

/**
 * Whether the shell shows no progress of its own (older than 5.0, which
 * introduced FD_PROGRESSUI).  Checked once.
 */
static BOOL vboxClipFileProgressNeeded(void)
{
    if (!g_ClipFileProgress.fChecked)
    {
        DLLGETVERSIONPROC pfnDllGetVersion;
        DLLVERSIONINFO Info;
        HMODULE hShell = GetModuleHandle("SHELL32.DLL");

        memset(&Info, 0, sizeof(Info));
        Info.cbSize = sizeof(Info);
        pfnDllGetVersion = hShell ? (DLLGETVERSIONPROC)GetProcAddress(hShell, "DllGetVersion") : NULL;
        if (!pfnDllGetVersion || FAILED(pfnDllGetVersion(&Info)))
            Info.dwMajorVersion = 4;
        g_ClipFileProgress.fChecked = TRUE;
        g_ClipFileProgress.fNeeded  = Info.dwMajorVersion < 5;
        VBoxTrayLogRel("Clipboard: shell %lu.%lu, %s progress window for pasted host files\n", Info.dwMajorVersion,
                       Info.dwMinorVersion, g_ClipFileProgress.fNeeded ? "our" : "the shell's");
    }
    return g_ClipFileProgress.fNeeded;
}

/**
 * Closes the progress window.
 */
static void vboxClipFileProgressClose(void)
{
    VBOXPROGRESSDIALOG *pDlg = g_ClipFileProgress.pDlg;

    g_ClipFileProgress.pDlg = NULL;
    if (pDlg)
    {
        pDlg->lpVtbl->StopProgressDialog(pDlg);
        pDlg->lpVtbl->Release(pDlg);
    }
}

/**
 * Ends the paste when nothing was read for a while (timer on the window
 * thread).
 */
static VOID CALLBACK vboxClipFileProgressTimer(HWND hwnd, UINT uMsg, UINT idEvent, DWORD msTime)
{
    if (GetTickCount() - g_ClipFileProgress.msLast < VBOXCLIPFILE_PROGRESS_IDLE_MS)
        return;
    vboxClipFileProgressClose();
    KillTimer(NULL, g_ClipFileProgress.idTimer);
    g_ClipFileProgress.idTimer   = 0;
    g_ClipFileProgress.fCanceled = FALSE;
}

/**
 * An application opens a host file (FileContents): opens the progress
 * window for the whole paste if the shell has none, and shows the file.
 */
static void vboxClipFileProgressFile(VBOXCLIPFILEOBJ *pObj, UINT32 iEntry)
{
    WCHAR wszName[MAX_PATH];
    UINT32 i;

    if (!vboxClipFileProgressNeeded())
        return;
    g_ClipFileProgress.msLast = GetTickCount();
    if (!g_ClipFileProgress.idTimer)
        g_ClipFileProgress.idTimer = SetTimer(NULL, 0, VBOXCLIPFILE_PROGRESS_TIMER_MS, vboxClipFileProgressTimer);
    if (g_ClipFileProgress.fCanceled)
        return;
    if (!g_ClipFileProgress.pDlg)
    {
        VBOXPROGRESSDIALOG *pDlg = NULL;
        HRESULT hr = CoCreateInstance(&g_vboxClsidProgressDialog, NULL, CLSCTX_INPROC_SERVER, &g_vboxIidProgressDialog,
                                      (void **)&pDlg);
        if (FAILED(hr))
        {
            VBoxTrayLogRel("Clipboard: no progress window, hr=%#lx\n", hr);
            g_ClipFileProgress.fNeeded = FALSE;
            return;
        }
        pDlg->lpVtbl->SetTitle(pDlg, L"Pasting files from the host");
        pDlg->lpVtbl->SetAnimation(pDlg, GetModuleHandle("SHELL32.DLL"), VBOXCLIPFILE_AVI_FILECOPY);
        hr = pDlg->lpVtbl->StartProgressDialog(pDlg, NULL, NULL, VBOXCLIPFILE_PROGDLG_AUTOTIME, NULL);
        if (FAILED(hr))
        {
            VBoxTrayLogRel("Clipboard: the progress window didn't open, hr=%#lx\n", hr);
            pDlg->lpVtbl->Release(pDlg);
            g_ClipFileProgress.fNeeded = FALSE;
            return;
        }
        g_ClipFileProgress.pDlg    = pDlg;
        g_ClipFileProgress.cbDone  = 0;
        g_ClipFileProgress.cbTotal = 0;
        for (i = 0; i < pObj->cEntries; i++)
            g_ClipFileProgress.cbTotal += pObj->paEntries[i].cbObject;
        pDlg->lpVtbl->SetProgress64(pDlg, 0, g_ClipFileProgress.cbTotal);
    }
    if (!MultiByteToWideChar(CP_ACP, 0, pObj->paEntries[iEntry].szName, -1, wszName, MAX_PATH))
        wszName[0] = 0;
    g_ClipFileProgress.pDlg->lpVtbl->SetLine(g_ClipFileProgress.pDlg, 1, wszName, TRUE, NULL);
}

/**
 * Bytes of a host file were read.
 *
 * @returns FALSE if the user canceled the paste.
 */
static BOOL vboxClipFileProgressRead(ULONG cb)
{
    VBOXPROGRESSDIALOG *pDlg = g_ClipFileProgress.pDlg;

    if (g_ClipFileProgress.fCanceled)
        return FALSE;
    if (!pDlg)
        return TRUE;
    g_ClipFileProgress.msLast  = GetTickCount();
    g_ClipFileProgress.cbDone += cb;
    if (pDlg->lpVtbl->HasUserCancelled(pDlg))
    {
        VBoxTrayLogRel("Clipboard: the user canceled the paste\n");
        g_ClipFileProgress.fCanceled = TRUE;
        vboxClipFileProgressClose();
        return FALSE;
    }
    pDlg->lpVtbl->SetProgress64(pDlg, g_ClipFileProgress.cbDone, g_ClipFileProgress.cbTotal);
    if (g_ClipFileProgress.cbDone >= g_ClipFileProgress.cbTotal)
        vboxClipFileProgressClose();
    return TRUE;
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
    int rc = g_ClipFileProgress.fCanceled ? VERR_CANCELLED : VINF_SUCCESS;

    while (RT_SUCCESS(rc) && cbDone < cb && pThis->off < cbObject)
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
        if (!vboxClipFileProgressRead(cbRead))
            rc = VERR_CANCELLED;
    }
    pObj->cbRead += cbDone;
    *pcbRead = cbDone;
    if (RT_FAILURE(rc) && rc != VERR_CANCELLED)
    {
        VBoxTrayLogRel("Clipboard: reading the host file \"%s\" failed, rc=%d\n", pObj->paEntries[pThis->iEntry].szName, rc);
        if (RT_SUCCESS(pObj->rcRead))
            pObj->rcRead = rc;
    }
    return rc;
}

/**
 * The HRESULT of a failed read: a canceled paste is no error to show.
 */
static HRESULT vboxClipFileStreamReadHr(int rc)
{
    return rc == VERR_CANCELLED ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : STG_E_READFAULT;
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
    return RT_SUCCESS(rc) ? S_OK : vboxClipFileStreamReadHr(rc);
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
        int rc = vboxClipFileStreamReadIt(pThis, pbBuf, cbChunk, &cbRead);
        if (RT_FAILURE(rc))
        {
            hr = vboxClipFileStreamReadHr(rc);
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

    vboxClipFileProgressFile(pObj, iEntry);
    if (g_ClipFileProgress.fCanceled)
        return HRESULT_FROM_WIN32(ERROR_CANCELLED);
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
            VBoxClipFileReportStatus(pThis->hDevice, pThis->idClient, pThis->idContext,
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
        pDesc->dwFlags          = FD_PROGRESSUI | FD_ATTRIBUTES | FD_FILESIZE;
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
            hr = vboxClipFileStreamReadHr(rc);
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
*   Entry points                                                                                                                 *
*********************************************************************************************************************************/

/**
 * Registers the clipboard formats of the data object (VBoxClipFileInit).
 */
void VBoxClipFileObjInit(void)
{
    static const DWORD s_afTymed[VBOXCLIPFILE_FMT_COUNT] = { TYMED_HGLOBAL, TYMED_ISTREAM, TYMED_HGLOBAL };
    unsigned i;

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
    vboxClipFileProgressClose();
    if (g_ClipFileProgress.idTimer)
    {
        KillTimer(NULL, g_ClipFileProgress.idTimer);
        g_ClipFileProgress.idTimer = 0;
    }
}
