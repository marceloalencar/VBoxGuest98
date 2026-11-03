/** @file
 * VBoxTray - Shared clipboard file transfers, internal header shared by the
 * ClipFile*.c files.
 */

#ifndef VBOXTRAY_CLIPFILE_H
#define VBOXTRAY_CLIPFILE_H

#include "VBoxTray.h"


/** Timeout of each transfer call; the host may have to read a slow disk (ms). */
#define VBOXCLIPFILE_CALL_TIMEOUT_MS 60000
/** Largest chunk read at once (upstream's default chunk size). */
#define VBOXCLIPFILE_CHUNK_SIZE     (64U * 1024)
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
/** 100 ns intervals from 1601 (FILETIME) to 1970 (RTTIMESPEC). */
#define VBOXCLIPFILE_FILETIME_1970  (11644473600i64 * 10000000)


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

/** The transfer being set up (ClipFile.c). */
extern VBOXCLIPFILESTATE g_ClipFile;


/*********************************************************************************************************************************
*   Shared calls (ClipFile.c)                                                                                                    *
*********************************************************************************************************************************/

int  VBoxClipFileCall(HANDLE hDevice, VBGLR3HGCMMSG *pMsg);
int  VBoxClipFileReportStatus(HANDLE hDevice, UINT32 idClient, UINT64 idContext, UINT32 uStatus, int rcTransfer);


/*********************************************************************************************************************************
*   Host to guest (ClipFileObj.c, window thread)                                                                                 *
*********************************************************************************************************************************/

void VBoxClipFileObjInit(void);


/*********************************************************************************************************************************
*   Guest to host (ClipFileSend.c, worker thread)                                                                                *
*********************************************************************************************************************************/

void VBoxClipFileSendOnStatus(HANDLE hDevice, UINT32 idClient, UINT64 idContext, UINT32 enmDir, UINT32 uStatus,
                              int rcHost);

#endif
