/** @file
 * VBoxTray - Drag and drop, internal header shared by DnD.c and DnDObj.c.
 */

#ifndef VBOXTRAY_DND_H
#define VBOXTRAY_DND_H

#include "VBoxTray.h"
#include <ole2.h>


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
