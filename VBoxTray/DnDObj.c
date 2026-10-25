/** @file
 * VBoxTray - Drag and drop OLE objects (IDataObject, IEnumFORMATETC,
 * IDropSource), written in C with hand made virtual tables.
 *
 * The data object offers a single format.  It is created when the host
 * drag enters the guest, but the data only arrives after the drop, from the
 * DnD worker thread (VBoxDnDDataObjectSetData).  The proxy window releases
 * the mouse button only then, so drop targets normally find the data ready;
 * GetData waits for it otherwise.  The objects are used by the proxy window
 * thread (and by other processes through OLE marshaling, which calls in on
 * that thread); only VBoxDnDDataObjectSetData/Abort come from the worker.
 */

#include "DnD.h"

#include <stdlib.h>
#include <string.h>


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/

/** How long GetData waits for the data after the drop (ms). */
#define VBOXDND_DATA_WAIT_MS        30000


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/

/** Data object state. */
typedef enum VBOXDNDDATASTATE
{
    /** Being dragged, no data yet. */
    VBOXDNDDATASTATE_DRAGGING = 0,
    /** Dropped, the data was requested from the host. */
    VBOXDNDDATASTATE_DROPPING,
    /** The data is here. */
    VBOXDNDDATASTATE_DROPPED,
    /** Cancelled or failed. */
    VBOXDNDDATASTATE_ABORTED
} VBOXDNDDATASTATE;

/** IDataObject implementation. */
typedef struct VBOXDNDDATAOBJ
{
    /** Must be first: the interface pointer is the object pointer. */
    IDataObject      Iface;
    LONG             cRefs;
    FORMATETC        FormatEtc;
    /** Protects enmState, pvData and cbData. */
    CRITICAL_SECTION CritSect;
    VBOXDNDDATASTATE enmState;
    /** Signalled when leaving VBOXDNDDATASTATE_DROPPING. */
    HANDLE           hEvtDone;
    /** The data in the HGLOBAL layout of the format (malloc'ed). */
    void            *pvData;
    DWORD            cbData;
} VBOXDNDDATAOBJ;

/** IEnumFORMATETC implementation (one format). */
typedef struct VBOXDNDENUMFMT
{
    IEnumFORMATETC   Iface;
    LONG             cRefs;
    FORMATETC        FormatEtc;
    /** Index of the next format to return (0 or 1). */
    ULONG            iCur;
} VBOXDNDENUMFMT;

/** IDropSource implementation. */
typedef struct VBOXDNDDROPSOURCE
{
    IDropSource      Iface;
    LONG             cRefs;
    /** Set to cancel the drag at the next QueryContinueDrag. */
    LONG volatile    fAbort;
    /** The effect the drop target last offered (DROPEFFECT_XXX). */
    DWORD volatile   dwEffect;
} VBOXDNDDROPSOURCE;


/*********************************************************************************************************************************
*   IEnumFORMATETC                                                                                                               *
*********************************************************************************************************************************/

static IEnumFORMATETC *vboxDnDEnumFmtCreate(const FORMATETC *pFormatEtc, ULONG iCur);

static HRESULT STDMETHODCALLTYPE vboxDnDEnumFmtQueryInterface(IEnumFORMATETC *pIface, REFIID riid, void **ppv)
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

static ULONG STDMETHODCALLTYPE vboxDnDEnumFmtAddRef(IEnumFORMATETC *pIface)
{
    return (ULONG)InterlockedIncrement(&((VBOXDNDENUMFMT *)pIface)->cRefs);
}

static ULONG STDMETHODCALLTYPE vboxDnDEnumFmtRelease(IEnumFORMATETC *pIface)
{
    VBOXDNDENUMFMT *pThis = (VBOXDNDENUMFMT *)pIface;
    LONG cRefs = InterlockedDecrement(&pThis->cRefs);
    if (cRefs == 0)
        free(pThis);
    return (ULONG)cRefs;
}

static HRESULT STDMETHODCALLTYPE vboxDnDEnumFmtNext(IEnumFORMATETC *pIface, ULONG celt, FORMATETC *rgelt,
                                                    ULONG *pceltFetched)
{
    VBOXDNDENUMFMT *pThis = (VBOXDNDENUMFMT *)pIface;
    ULONG cFetched = 0;

    if (!rgelt || (celt != 1 && !pceltFetched))
        return E_INVALIDARG;
    if (celt && pThis->iCur == 0)
    {
        rgelt[0] = pThis->FormatEtc;
        pThis->iCur = 1;
        cFetched = 1;
    }
    if (pceltFetched)
        *pceltFetched = cFetched;
    return cFetched == celt ? S_OK : S_FALSE;
}

static HRESULT STDMETHODCALLTYPE vboxDnDEnumFmtSkip(IEnumFORMATETC *pIface, ULONG celt)
{
    VBOXDNDENUMFMT *pThis = (VBOXDNDENUMFMT *)pIface;
    if (celt && pThis->iCur == 0)
    {
        pThis->iCur = 1;
        celt--;
    }
    return celt ? S_FALSE : S_OK;
}

static HRESULT STDMETHODCALLTYPE vboxDnDEnumFmtReset(IEnumFORMATETC *pIface)
{
    ((VBOXDNDENUMFMT *)pIface)->iCur = 0;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE vboxDnDEnumFmtClone(IEnumFORMATETC *pIface, IEnumFORMATETC **ppEnum)
{
    VBOXDNDENUMFMT *pThis = (VBOXDNDENUMFMT *)pIface;

    if (!ppEnum)
        return E_INVALIDARG;
    *ppEnum = vboxDnDEnumFmtCreate(&pThis->FormatEtc, pThis->iCur);
    return *ppEnum ? S_OK : E_OUTOFMEMORY;
}

static IEnumFORMATETCVtbl g_vboxDnDEnumFmtVtbl =
{
    vboxDnDEnumFmtQueryInterface,
    vboxDnDEnumFmtAddRef,
    vboxDnDEnumFmtRelease,
    vboxDnDEnumFmtNext,
    vboxDnDEnumFmtSkip,
    vboxDnDEnumFmtReset,
    vboxDnDEnumFmtClone
};

/**
 * Creates an enumerator for one format, with a reference held for the caller.
 */
static IEnumFORMATETC *vboxDnDEnumFmtCreate(const FORMATETC *pFormatEtc, ULONG iCur)
{
    VBOXDNDENUMFMT *pThis = (VBOXDNDENUMFMT *)calloc(1, sizeof(*pThis));
    if (!pThis)
        return NULL;
    pThis->Iface.lpVtbl = &g_vboxDnDEnumFmtVtbl;
    pThis->cRefs        = 1;
    pThis->FormatEtc    = *pFormatEtc;
    pThis->iCur         = iCur;
    return &pThis->Iface;
}


/*********************************************************************************************************************************
*   IDataObject                                                                                                                  *
*********************************************************************************************************************************/

static HRESULT STDMETHODCALLTYPE vboxDnDDataObjQueryInterface(IDataObject *pIface, REFIID riid, void **ppv)
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

static ULONG STDMETHODCALLTYPE vboxDnDDataObjAddRef(IDataObject *pIface)
{
    return (ULONG)InterlockedIncrement(&((VBOXDNDDATAOBJ *)pIface)->cRefs);
}

static ULONG STDMETHODCALLTYPE vboxDnDDataObjRelease(IDataObject *pIface)
{
    VBOXDNDDATAOBJ *pThis = (VBOXDNDDATAOBJ *)pIface;
    LONG cRefs = InterlockedDecrement(&pThis->cRefs);
    if (cRefs == 0)
    {
        DeleteCriticalSection(&pThis->CritSect);
        CloseHandle(pThis->hEvtDone);
        free(pThis->pvData);
        free(pThis);
    }
    return (ULONG)cRefs;
}

/**
 * Checks whether a FORMATETC asks for our format.
 */
static HRESULT vboxDnDDataObjCheckFormat(VBOXDNDDATAOBJ *pThis, const FORMATETC *pFormatEtc)
{
    if (!pFormatEtc)
        return E_INVALIDARG;
    if (   pFormatEtc->cfFormat != pThis->FormatEtc.cfFormat
        || pFormatEtc->dwAspect != DVASPECT_CONTENT
        || pFormatEtc->lindex   != -1)
        return DV_E_FORMATETC;
    if (!(pFormatEtc->tymed & TYMED_HGLOBAL))
        return DV_E_TYMED;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE vboxDnDDataObjGetData(IDataObject *pIface, FORMATETC *pFormatEtc, STGMEDIUM *pMedium)
{
    VBOXDNDDATAOBJ *pThis = (VBOXDNDDATAOBJ *)pIface;
    HGLOBAL hGlobal;
    void *pv;
    HRESULT hr;

    if (!pMedium)
        return E_INVALIDARG;
    hr = vboxDnDDataObjCheckFormat(pThis, pFormatEtc);
    if (FAILED(hr))
        return hr;

    /* Dropped, but the data may still be on its way. */
    if (pThis->enmState == VBOXDNDDATASTATE_DROPPING)
        WaitForSingleObject(pThis->hEvtDone, VBOXDND_DATA_WAIT_MS);

    EnterCriticalSection(&pThis->CritSect);
    if (pThis->enmState == VBOXDNDDATASTATE_DROPPED)
    {
        hr = E_OUTOFMEMORY;
        hGlobal = GlobalAlloc(GMEM_MOVEABLE, pThis->cbData);
        if (hGlobal)
        {
            pv = GlobalLock(hGlobal);
            if (pv)
            {
                memcpy(pv, pThis->pvData, pThis->cbData);
                GlobalUnlock(hGlobal);
                pMedium->tymed          = TYMED_HGLOBAL;
                pMedium->hGlobal        = hGlobal;
                pMedium->pUnkForRelease = NULL;
                hr = S_OK;
            }
            else
                GlobalFree(hGlobal);
        }
    }
    else
        hr = E_UNEXPECTED; /* Not dropped yet (a target peeking while dragging), or failed. */
    LeaveCriticalSection(&pThis->CritSect);
    return hr;
}

static HRESULT STDMETHODCALLTYPE vboxDnDDataObjGetDataHere(IDataObject *pIface, FORMATETC *pFormatEtc, STGMEDIUM *pMedium)
{
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE vboxDnDDataObjQueryGetData(IDataObject *pIface, FORMATETC *pFormatEtc)
{
    return vboxDnDDataObjCheckFormat((VBOXDNDDATAOBJ *)pIface, pFormatEtc);
}

static HRESULT STDMETHODCALLTYPE vboxDnDDataObjGetCanonicalFormatEtc(IDataObject *pIface, FORMATETC *pFormatEtcIn,
                                                                     FORMATETC *pFormatEtcOut)
{
    if (pFormatEtcOut)
        pFormatEtcOut->ptd = NULL;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE vboxDnDDataObjSetData(IDataObject *pIface, FORMATETC *pFormatEtc, STGMEDIUM *pMedium,
                                                       BOOL fRelease)
{
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE vboxDnDDataObjEnumFormatEtc(IDataObject *pIface, DWORD dwDirection,
                                                             IEnumFORMATETC **ppEnum)
{
    if (!ppEnum)
        return E_INVALIDARG;
    *ppEnum = NULL;
    if (dwDirection != DATADIR_GET)
        return E_NOTIMPL;
    *ppEnum = vboxDnDEnumFmtCreate(&((VBOXDNDDATAOBJ *)pIface)->FormatEtc, 0);
    return *ppEnum ? S_OK : E_OUTOFMEMORY;
}

static HRESULT STDMETHODCALLTYPE vboxDnDDataObjDAdvise(IDataObject *pIface, FORMATETC *pFormatEtc, DWORD fAdvf,
                                                       IAdviseSink *pAdvSink, DWORD *pdwConnection)
{
    return OLE_E_ADVISENOTSUPPORTED;
}

static HRESULT STDMETHODCALLTYPE vboxDnDDataObjDUnadvise(IDataObject *pIface, DWORD dwConnection)
{
    return OLE_E_ADVISENOTSUPPORTED;
}

static HRESULT STDMETHODCALLTYPE vboxDnDDataObjEnumDAdvise(IDataObject *pIface, IEnumSTATDATA **ppEnumAdvise)
{
    return OLE_E_ADVISENOTSUPPORTED;
}

static IDataObjectVtbl g_vboxDnDDataObjVtbl =
{
    vboxDnDDataObjQueryInterface,
    vboxDnDDataObjAddRef,
    vboxDnDDataObjRelease,
    vboxDnDDataObjGetData,
    vboxDnDDataObjGetDataHere,
    vboxDnDDataObjQueryGetData,
    vboxDnDDataObjGetCanonicalFormatEtc,
    vboxDnDDataObjSetData,
    vboxDnDDataObjEnumFormatEtc,
    vboxDnDDataObjDAdvise,
    vboxDnDDataObjDUnadvise,
    vboxDnDDataObjEnumDAdvise
};

/**
 * Creates the data object for a format (TYMED_HGLOBAL), with a reference
 * held for the caller.
 *
 * @returns The object, NULL if out of memory.
 */
IDataObject *VBoxDnDDataObjectCreate(CLIPFORMAT cfFormat)
{
    VBOXDNDDATAOBJ *pThis = (VBOXDNDDATAOBJ *)calloc(1, sizeof(*pThis));
    if (!pThis)
        return NULL;
    pThis->hEvtDone = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!pThis->hEvtDone)
    {
        free(pThis);
        return NULL;
    }
    InitializeCriticalSection(&pThis->CritSect);
    pThis->Iface.lpVtbl       = &g_vboxDnDDataObjVtbl;
    pThis->cRefs              = 1;
    pThis->FormatEtc.cfFormat = cfFormat;
    pThis->FormatEtc.ptd      = NULL;
    pThis->FormatEtc.dwAspect = DVASPECT_CONTENT;
    pThis->FormatEtc.lindex   = -1;
    pThis->FormatEtc.tymed    = TYMED_HGLOBAL;
    pThis->enmState           = VBOXDNDDATASTATE_DRAGGING;
    return &pThis->Iface;
}

/**
 * Marks the object dropped: the data was requested from the host.
 */
void VBoxDnDDataObjectSetDropping(IDataObject *pIface)
{
    VBOXDNDDATAOBJ *pThis = (VBOXDNDDATAOBJ *)pIface;

    EnterCriticalSection(&pThis->CritSect);
    if (pThis->enmState == VBOXDNDDATASTATE_DRAGGING)
        pThis->enmState = VBOXDNDDATASTATE_DROPPING;
    LeaveCriticalSection(&pThis->CritSect);
}

/**
 * Hands the received data to the object.  Any thread.
 *
 * @param   pIface      The data object.
 * @param   pvData      The data in the HGLOBAL layout of the format
 *                      (malloc'ed, the object takes it over).
 * @param   cbData      Its size.
 */
void VBoxDnDDataObjectSetData(IDataObject *pIface, void *pvData, DWORD cbData)
{
    VBOXDNDDATAOBJ *pThis = (VBOXDNDDATAOBJ *)pIface;

    EnterCriticalSection(&pThis->CritSect);
    if (pThis->enmState != VBOXDNDDATASTATE_ABORTED && !pThis->pvData)
    {
        pThis->pvData   = pvData;
        pThis->cbData   = cbData;
        pThis->enmState = VBOXDNDDATASTATE_DROPPED;
        pvData = NULL;
    }
    LeaveCriticalSection(&pThis->CritSect);
    SetEvent(pThis->hEvtDone);
    free(pvData);
}

/**
 * Marks the object failed, waking up GetData.  Any thread.
 */
void VBoxDnDDataObjectAbort(IDataObject *pIface)
{
    VBOXDNDDATAOBJ *pThis = (VBOXDNDDATAOBJ *)pIface;

    EnterCriticalSection(&pThis->CritSect);
    if (pThis->enmState != VBOXDNDDATASTATE_DROPPED)
        pThis->enmState = VBOXDNDDATASTATE_ABORTED;
    LeaveCriticalSection(&pThis->CritSect);
    SetEvent(pThis->hEvtDone);
}


/*********************************************************************************************************************************
*   IDropSource                                                                                                                  *
*********************************************************************************************************************************/

static HRESULT STDMETHODCALLTYPE vboxDnDDropSrcQueryInterface(IDropSource *pIface, REFIID riid, void **ppv)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDropSource))
    {
        *ppv = pIface;
        pIface->lpVtbl->AddRef(pIface);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE vboxDnDDropSrcAddRef(IDropSource *pIface)
{
    return (ULONG)InterlockedIncrement(&((VBOXDNDDROPSOURCE *)pIface)->cRefs);
}

static ULONG STDMETHODCALLTYPE vboxDnDDropSrcRelease(IDropSource *pIface)
{
    VBOXDNDDROPSOURCE *pThis = (VBOXDNDDROPSOURCE *)pIface;
    LONG cRefs = InterlockedDecrement(&pThis->cRefs);
    if (cRefs == 0)
        free(pThis);
    return (ULONG)cRefs;
}

static HRESULT STDMETHODCALLTYPE vboxDnDDropSrcQueryContinueDrag(IDropSource *pIface, BOOL fEscapePressed, DWORD grfKeyState)
{
    VBOXDNDDROPSOURCE *pThis = (VBOXDNDDROPSOURCE *)pIface;

    if (fEscapePressed || pThis->fAbort)
    {
        pThis->dwEffect = DROPEFFECT_NONE;
        return DRAGDROP_S_CANCEL;
    }
    /* The proxy window releases the button once the host data is here. */
    if (!(grfKeyState & MK_LBUTTON))
        return DRAGDROP_S_DROP;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE vboxDnDDropSrcGiveFeedback(IDropSource *pIface, DWORD dwEffect)
{
    ((VBOXDNDDROPSOURCE *)pIface)->dwEffect = dwEffect;
    return DRAGDROP_S_USEDEFAULTCURSORS;
}

static IDropSourceVtbl g_vboxDnDDropSrcVtbl =
{
    vboxDnDDropSrcQueryInterface,
    vboxDnDDropSrcAddRef,
    vboxDnDDropSrcRelease,
    vboxDnDDropSrcQueryContinueDrag,
    vboxDnDDropSrcGiveFeedback
};

/**
 * Creates the drop source, with a reference held for the caller.
 */
IDropSource *VBoxDnDDropSourceCreate(void)
{
    VBOXDNDDROPSOURCE *pThis = (VBOXDNDDROPSOURCE *)calloc(1, sizeof(*pThis));
    if (!pThis)
        return NULL;
    pThis->Iface.lpVtbl = &g_vboxDnDDropSrcVtbl;
    pThis->cRefs        = 1;
    pThis->dwEffect     = DROPEFFECT_NONE;
    return &pThis->Iface;
}

/**
 * Makes the drag end with DRAGDROP_S_CANCEL.
 */
void VBoxDnDDropSourceAbort(IDropSource *pIface)
{
    InterlockedExchange((LONG *)&((VBOXDNDDROPSOURCE *)pIface)->fAbort, TRUE);
}

/**
 * Returns the drop effect (DROPEFFECT_XXX) the target under the cursor offers.
 */
DWORD VBoxDnDDropSourceGetEffect(IDropSource *pIface)
{
    return ((VBOXDNDDROPSOURCE *)pIface)->dwEffect;
}
