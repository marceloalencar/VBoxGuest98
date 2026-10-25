/** @file
 * VBoxGst - VBGL ring-0 request layer.
 *
 * VMMDev requests are handed to the host by writing their physical address
 * to the request port, so they must live in physically contiguous memory
 * below 4GB.  Windows 98 provides that through DMA adapter common buffers.
 *
 * Each allocation is prefixed by a VBGLR0REQPRIV block that remembers the
 * physical address, so VbglR0GRPerform needs no lookups or locks and can be
 * used at any IRQL, including from the ISR (it never logs).
 */

#include "VBoxGst.h"


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/

/** Private header preceding every request allocated by VbglR0GRAlloc. */
typedef struct VBGLR0REQPRIV
{
    /** Physical address of this header. */
    PHYSICAL_ADDRESS PhysAddr;
    /** Size of the common buffer including this header. */
    ULONG cbAlloc;
    /** VBGLR0REQPRIV_MAGIC. */
    ULONG u32Magic;
} VBGLR0REQPRIV;

/** VBGLR0REQPRIV::u32Magic value ('VBGL'). */
#define VBGLR0REQPRIV_MAGIC     0x5642474cU


/*********************************************************************************************************************************
*   Global Variables                                                                                                             *
*********************************************************************************************************************************/

VBGLDATA g_vbgldata;


/**
 * Returns the private header of a request.
 */
static VBGLR0REQPRIV *vbglR0GRGetPriv(VMMDevRequestHeader *pReq)
{
    return (VBGLR0REQPRIV *)((PUCHAR)pReq - sizeof(VBGLR0REQPRIV));
}

/**
 * Used by VbglR0InitPrimary to get the host feature mask and version
 * information (g_vbgldata::hostVersion).
 *
 * This was first implemented by the host in 3.1 and we quietly ignore failures
 * for that reason.
 */
static void vbglR0QueryHostVersion(void)
{
    VMMDevReqHostVersion *pReq;
    int rc = VbglR0GRAlloc((VMMDevRequestHeader **)&pReq, sizeof(*pReq), VMMDevReq_GetHostVersion);
    if (RT_SUCCESS(rc))
    {
        rc = VbglR0GRPerform(&pReq->header);
        if (RT_SUCCESS(rc))
            g_vbgldata.hostVersion = *pReq;
        VbglR0GRFree(&pReq->header);
    }
}

/**
 * Initializes the request layer.
 *
 * @returns VBox status code.
 * @param   pPdo            The physical device object of the VMMDev (needed
 *                          for the DMA adapter).
 * @param   portVMMDev      Base of the VMMDev I/O port range.
 * @param   pVMMDevMemory   The VMMDev memory mapping, optional.
 * @param   pfFeatures      Where to return the host feature flags.
 */
int VbglR0InitPrimary(PDEVICE_OBJECT pPdo, USHORT portVMMDev, VMMDevMemory volatile *pVMMDevMemory, UINT32 *pfFeatures)
{
    DEVICE_DESCRIPTION DevDesc;
    ULONG cMapRegisters = 0;

    RtlZeroMemory(&g_vbgldata, sizeof(g_vbgldata));
    g_vbgldata.portVMMDev    = portVMMDev;
    g_vbgldata.pVMMDevMemory = pVMMDevMemory;

    RtlZeroMemory(&DevDesc, sizeof(DevDesc));
    DevDesc.Version       = DEVICE_DESCRIPTION_VERSION;
    DevDesc.Master        = FALSE;
    DevDesc.ScatterGather = FALSE;
    DevDesc.InterfaceType = PCIBus;
    DevDesc.MaximumLength = VBOXGUEST98_MAX_VMMDEV_REQUEST_SIZE + PAGE_SIZE;
    g_vbgldata.pDmaAdapter = IoGetDmaAdapter(pPdo, &DevDesc, &cMapRegisters);
    if (!g_vbgldata.pDmaAdapter)
        return VERR_NO_MEMORY;

    vbglR0QueryHostVersion();
    *pfFeatures = g_vbgldata.hostVersion.features;
    return VINF_SUCCESS;
}

/**
 * Counter part to VbglR0InitPrimary.  All requests must have been freed.
 */
void VbglR0TerminatePrimary(void)
{
    if (g_vbgldata.pDmaAdapter)
    {
        g_vbgldata.pDmaAdapter->DmaOperations->PutDmaAdapter(g_vbgldata.pDmaAdapter);
        g_vbgldata.pDmaAdapter = NULL;
    }
    g_vbgldata.pVMMDevMemory = NULL;
    g_vbgldata.portVMMDev    = 0;
}

/**
 * Allocates and initializes a request.
 *
 * Must be called at PASSIVE_LEVEL.  The body is zeroed, the header is filled
 * in for a kernel mode requestor.
 *
 * @returns VBox status code.
 * @param   ppReq       Where to return the request.
 * @param   cbReq       Size of the request including the header.
 * @param   enmReqType  The request type.
 */
int VbglR0GRAlloc(VMMDevRequestHeader **ppReq, ULONG cbReq, VMMDevRequestType enmReqType)
{
    PHYSICAL_ADDRESS PhysAddr;
    VBGLR0REQPRIV *pPriv;
    VMMDevRequestHeader *pReq;
    ULONG cbAlloc;

    *ppReq = NULL;
    if (!g_vbgldata.pDmaAdapter)
        return VERR_INTERNAL_ERROR;
    if (   cbReq < sizeof(VMMDevRequestHeader)
        || cbReq > VBOXGUEST98_MAX_VMMDEV_REQUEST_SIZE)
        return VERR_INVALID_PARAMETER;

    cbAlloc = sizeof(VBGLR0REQPRIV) + cbReq;
    pPriv = (VBGLR0REQPRIV *)g_vbgldata.pDmaAdapter->DmaOperations->AllocateCommonBuffer(g_vbgldata.pDmaAdapter,
                                                                                         cbAlloc, &PhysAddr, FALSE);
    if (!pPriv)
        return VERR_NO_MEMORY;
    if (PhysAddr.HighPart != 0)
    {
        /* VMMDev only takes 32-bit physical addresses. */
        g_vbgldata.pDmaAdapter->DmaOperations->FreeCommonBuffer(g_vbgldata.pDmaAdapter, cbAlloc, PhysAddr, pPriv, FALSE);
        return VERR_OUT_OF_RANGE;
    }

    RtlZeroMemory(pPriv, cbAlloc);
    pPriv->PhysAddr = PhysAddr;
    pPriv->cbAlloc  = cbAlloc;
    pPriv->u32Magic = VBGLR0REQPRIV_MAGIC;

    pReq = (VMMDevRequestHeader *)(pPriv + 1);
    pReq->size        = cbReq;
    pReq->version     = VMMDEV_REQUEST_HEADER_VERSION;
    pReq->requestType = enmReqType;
    pReq->rc          = VERR_GENERAL_FAILURE;
    pReq->reserved1   = 0;
    pReq->fRequestor  = VMMDEV_REQUESTOR_KERNEL        | VMMDEV_REQUESTOR_USR_DRV
                      | VMMDEV_REQUESTOR_CON_DONT_KNOW | VMMDEV_REQUESTOR_TRUST_NOT_GIVEN;

    *ppReq = pReq;
    return VINF_SUCCESS;
}

/**
 * Returns the physical address of a request allocated by VbglR0GRAlloc.
 */
UINT32 VbglR0GRPhysAddr(VMMDevRequestHeader *pReq)
{
    return vbglR0GRGetPriv(pReq)->PhysAddr.LowPart + sizeof(VBGLR0REQPRIV);
}

/**
 * Hands a request to the host.
 *
 * The host processes the request synchronously while handling the port
 * write, except for HGCM requests which may complete asynchronously
 * (VINF_HGCM_ASYNC_EXECUTE).  Callable at any IRQL; never logs.
 *
 * @returns VBox status code, the request status on success.
 * @param   pReq    A request allocated by VbglR0GRAlloc.
 */
int VbglR0GRPerform(VMMDevRequestHeader *pReq)
{
    VBGLR0REQPRIV *pPriv;

    if (!pReq || !g_vbgldata.portVMMDev)
        return VERR_INVALID_PARAMETER;
    pPriv = vbglR0GRGetPriv(pReq);
    if (pPriv->u32Magic != VBGLR0REQPRIV_MAGIC)
        return VERR_INVALID_POINTER;

    vgdrvMemoryBarrier();
    WRITE_PORT_ULONG((PULONG)(ULONG_PTR)(g_vbgldata.portVMMDev + VMMDEV_PORT_OFF_REQUEST),
                     pPriv->PhysAddr.LowPart + sizeof(VBGLR0REQPRIV));
    vgdrvMemoryBarrier();

    return pReq->rc;
}

/**
 * Frees a request allocated by VbglR0GRAlloc.  Must be called at PASSIVE_LEVEL.
 */
void VbglR0GRFree(VMMDevRequestHeader *pReq)
{
    VBGLR0REQPRIV *pPriv;

    if (!pReq || !g_vbgldata.pDmaAdapter)
        return;
    pPriv = vbglR0GRGetPriv(pReq);
    if (pPriv->u32Magic != VBGLR0REQPRIV_MAGIC)
        return;
    pPriv->u32Magic = ~VBGLR0REQPRIV_MAGIC;
    g_vbgldata.pDmaAdapter->DmaOperations->FreeCommonBuffer(g_vbgldata.pDmaAdapter, pPriv->cbAlloc,
                                                           pPriv->PhysAddr, pPriv, FALSE);
}
