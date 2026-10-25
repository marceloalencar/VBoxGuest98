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

#endif /* !VBOXTRAY_DND_H */
