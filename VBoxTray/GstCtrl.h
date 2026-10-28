/** @file
 * VBoxTray - Guest control, internal header (GstCtrl.c: the root, sessions
 * and programs; GstCtrlFile.c: files, directories and file systems).
 */

#ifndef GSTCTRL_H
#define GSTCTRL_H

#include "VBoxTray.h"


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/

/** Timeout for the calls that don't wait for the host (ms). */
#define VBOXGSTCTRL_CALL_TIMEOUT_MS     30000
/** Size of the one time session key (the host accepts 64 to 16384 bytes). */
#define VBOXGSTCTRL_KEY_SIZE            64
/** First size of the string buffers of a host message; they grow when the
 *  host says they are too small. */
#define VBOXGSTCTRL_DEF_BUF_SIZE        1024
/** Largest string buffer accepted. */
#define VBOXGSTCTRL_MAX_BUF_SIZE        (256U * 1024)
/** Most data moved per message: program output, file reads and writes
 *  (upstream: 64K). */
#define VBOXGSTCTRL_OUTPUT_SIZE         (64U * 1024)


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/

struct VBOXGSTCTRLPROC;
struct VBOXGSTCTRLFILE;
struct VBOXGSTCTRLDIR;

/** A guest session. */
typedef struct VBOXGSTCTRLSESSION
{
    struct VBOXGSTCTRLSESSION *pNext;
    /** References: the root's list, the session thread and each program thread. */
    LONG        cRefs;
    UINT32      idSession;
    /** The one time key the root prepared the session with. */
    BYTE        abKey[VBOXGSTCTRL_KEY_SIZE];
    /** Overlapped handle, shared by the session thread and the program threads. */
    HANDLE      hDevice;
    /** The session's HGCM client, 0 once disconnected. */
    UINT32      idClient;
    /** Set when the session is closing; the program threads stop waiting. */
    HANDLE      hEvtStop;
    HANDLE      hThread;
    /** The programs started in the session (g_GstCtrlCritSect). */
    struct VBOXGSTCTRLPROC *pProcs;
    /** The open files and directories; only the session thread uses them. */
    struct VBOXGSTCTRLFILE *pFiles;
    struct VBOXGSTCTRLDIR  *pDirs;
} VBOXGSTCTRLSESSION;


/*********************************************************************************************************************************
*   Shared by GstCtrl.c and GstCtrlFile.c                                                                                        *
*********************************************************************************************************************************/

/** The host features (VBOX_GUESTCTRL_HF_0_XXX). */
extern UINT64 g_fVBoxGstCtrlHostFeatures;

int   VBoxGstCtrlCallMsg(HANDLE hDevice, VBGLR3HGCMMSG *pMsg);
int   VBoxGstCtrlCallU32(HANDLE hDevice, UINT32 idClient, UINT32 uFunction, UINT32 cParms, const UINT32 *pau32);
void  VBoxGstCtrlSkip(HANDLE hDevice, UINT32 idClient, int rcSkip, UINT32 idMsg);
char *VBoxGstCtrlGetStr(VBGLR3HGCMMSG *pMsg, UINT32 iParm, const BYTE *pb, char chDefault);

BOOL  VBoxGstCtrlFileHandleMsg(VBOXGSTCTRLSESSION *pSession, UINT32 idClient, UINT32 idMsg, UINT32 cParms);
void  VBoxGstCtrlFileCleanup(VBOXGSTCTRLSESSION *pSession);

#endif /* !GSTCTRL_H */
