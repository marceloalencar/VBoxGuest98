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
 *    (VBoxTrayBuildPath).
 *  - The directories are read recursively when the transfer starts.
 *  - Guest to host, the host's paths are matched with the roots without
 *    regard to case, like the FAT file system does.
 *  - The Windows 98 shell (4.72) ignores FD_PROGRESSUI and shows no progress
 *    while it copies virtual files, so with a shell older than 5.0 VBoxTray
 *    shows the standard progress window (CLSID_ProgressDialog, Internet
 *    Explorer 5 and later) itself while the files are read.
 *
 * ClipFile.c has what both directions share and the entry points for
 * Clipboard.c; ClipFileObj.c puts the host files on the guest clipboard,
 * ClipFileSend.c gives the guest files to the host.
 */

#include "ClipFile.h"


/*********************************************************************************************************************************
*   Global Variables                                                                                                             *
*********************************************************************************************************************************/

VBOXCLIPFILESTATE g_ClipFile;


/*********************************************************************************************************************************
*   HGCM calls                                                                                                                   *
*********************************************************************************************************************************/

/**
 * Makes a call, repeating it when interrupted.
 */
int VBoxClipFileCall(HANDLE hDevice, VBGLR3HGCMMSG *pMsg)
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
int VBoxClipFileReportStatus(HANDLE hDevice, UINT32 idClient, UINT64 idContext, UINT32 uStatus, int rcTransfer)
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
    rc = VBoxClipFileCall(hDevice, &Msg);
    VbglR3HgcmMsgFree(&Msg);
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
    InitializeCriticalSection(&g_ClipFile.CritSect);
    g_ClipFile.hEvtReady = CreateEvent(NULL, TRUE, FALSE, NULL);
    g_ClipFile.cbChunk   = VBOXCLIPFILE_CHUNK_SIZE;

    VBoxClipFileObjInit();
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
        if (RT_SUCCESS(VBoxClipFileCall(hDevice, &Msg)))
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
    rc = VBoxClipFileCall(hDevice, &Msg);
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
        rc = VBoxClipFileReportStatus(hDevice, idClient, idContext, SHCLTRANSFERSTATUS_INITIALIZED, VINF_SUCCESS);
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

    VBoxClipFileSendOnStatus(hDevice, idClient, idContext, enmDir, uStatus, rcHost);
}
