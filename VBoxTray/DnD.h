/** @file
 * VBoxTray - Drag and drop, internal header shared by the DnD*.c files.
 */

#ifndef VBOXTRAY_DND_H
#define VBOXTRAY_DND_H

#include "VBoxTray.h"
#include <ole2.h>
#include <shellapi.h>


/** Timeout for fetching queued host messages and for answers (ms). */
#define VBOXDND_CALL_TIMEOUT_MS     30000

/** A host drag entering the guest (WM_VBOXDND_ENTER data). */
typedef struct VBOXDNDENTER
{
    UINT32 x;
    UINT32 y;
    UINT32 fAllActions;
    /** Formats offered by the host, VBOX_DND_FORMATS_SEPARATOR separated. */
    char   szFormats[1];
} VBOXDNDENTER;


/*********************************************************************************************************************************
*   HGCM calls (DnDHgcm.c)                                                                                                       *
*********************************************************************************************************************************/

int VBoxDnDConnect(HANDLE hDevice, UINT32 *pidClient);
int VBoxDnDHandshake(HANDLE hDevice, UINT32 idClient);
int VBoxDnDGetNextMsg(HANDLE hDevice, UINT32 idClient, UINT32 *puMsg, UINT32 *pcParms);
int VBoxDnDRecvSimple(HANDLE hDevice, UINT32 idClient, UINT32 uMsg);
int VBoxDnDRecvAction(HANDLE hDevice, UINT32 idClient, UINT32 uMsg, UINT32 *px, UINT32 *py, VBOXDNDENTER **ppEnter);
int VBoxDnDSendAck(HANDLE hDevice, UINT32 idClient, UINT32 uAction);
int VBoxDnDSendReqData(HANDLE hDevice, UINT32 idClient, const char *pszFormat);
int VBoxDnDSendProgress(HANDLE hDevice, UINT32 idClient, UINT32 uStatus, UINT32 uPercent, int rcErr);
int VBoxDnDRecvGhPending(HANDLE hDevice, UINT32 idClient);
int VBoxDnDSendAckPending(HANDLE hDevice, UINT32 idClient, UINT32 uAction, const char *pszFormats);
int VBoxDnDRecvGhDropped(HANDLE hDevice, UINT32 idClient, char *pszFormat, DWORD cbFormat);
int VBoxDnDSendError(HANDLE hDevice, UINT32 idClient, int rcErr);
int VBoxDnDGhSendMeta(HANDLE hDevice, UINT32 idClient, const char *pszMetaFmt, const void *pvMeta, DWORD cbMeta,
                      UINT64 cbTotal, UINT64 cObjects);
int VBoxDnDGhSendDir(HANDLE hDevice, UINT32 idClient, const char *pszRel);
int VBoxDnDGhSendFile(HANDLE hDevice, UINT32 idClient, const char *pszAbs, const char *pszRel, UINT64 cbFile);


/*********************************************************************************************************************************
*   Transfers (DnDTransfer.c, worker thread)                                                                                     *
*********************************************************************************************************************************/

int VBoxDnDRecvData(HANDLE hDevice, UINT32 idClient, void **ppvData, DWORD *pcbData);
int VBoxDnDGhSendData(HANDLE hDevice, UINT32 idClient, const char *pszData, BOOL fFiles);


/*********************************************************************************************************************************
*   Text and names (DnDUtil.c)                                                                                                   *
*********************************************************************************************************************************/

int   VBoxDnDBuildPath(const char *pszDropDir, const char *pchRel, DWORD cchRel, char *pszPath);
DWORD VBoxDnDStrNLen(const BYTE *pb, DWORD cb);
int   VBoxDnDBuildHDrop(char *pszUriList, const char *pszDropDir, void **ppv, DWORD *pcb);
int   VBoxDnDBuildText(const char *pch, DWORD cch, void **ppv, DWORD *pcb);
char *VBoxDnDCopyHDrop(HDROP hDrop);


/*********************************************************************************************************************************
*   OLE objects (DnDObj.c)                                                                                                       *
*********************************************************************************************************************************/

IDataObject *VBoxDnDDataObjectCreate(CLIPFORMAT cfFormat);
void         VBoxDnDDataObjectSetDropping(IDataObject *pIface);
void         VBoxDnDDataObjectSetData(IDataObject *pIface, void *pvData, DWORD cbData);
void         VBoxDnDDataObjectAbort(IDataObject *pIface);

IDropSource *VBoxDnDDropSourceCreate(void);
void         VBoxDnDDropSourceAbort(IDropSource *pIface);
DWORD        VBoxDnDDropSourceGetEffect(IDropSource *pIface);

IDropTarget *VBoxDnDDropTargetCreate(void);


/*********************************************************************************************************************************
*   Drop target callbacks (DnD.c, window thread)                                                                                 *
*********************************************************************************************************************************/

/** A guest drag offering cfFormat (CF_HDROP, CF_TEXT or 0 for none we take)
 *  reached the proxy window. */
void VBoxDnDGhOnDragEnter(CLIPFORMAT cfFormat, IDataObject *pDataObj);
/** The guest drag was dropped on the proxy window.  @returns TRUE if its data was taken. */
BOOL VBoxDnDGhOnDrop(void);
/** The guest drag left the proxy window (cancelled). */
void VBoxDnDGhOnDragLeave(void);

#endif /* !VBOXTRAY_DND_H */
