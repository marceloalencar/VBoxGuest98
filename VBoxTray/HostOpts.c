/** @file
 * VBoxTray - Service options set by the host.
 *
 * The host gives the official VBoxService options as guest properties
 * (VBoxManage guestproperty set <vm> /VirtualBox/GuestAdd/VBoxService/--name
 * value), read only for the guest.  The services read them again every
 * VBOXHOSTOPTS_INTERVAL_MS, so no restart is needed; an option that is not
 * set gets its default.
 */

#include "VBoxTray.h"
#include <stdlib.h>
#include <string.h>


/**
 * Parses a decimal number in a range.
 *
 * @returns TRUE if valid.
 */
static BOOL vboxHostOptsParseU32(const char *psz, DWORD uMin, DWORD uMax, DWORD *pu)
{
    UINT64 u = 0;

    if (!*psz)
        return FALSE;
    for (; *psz; psz++)
    {
        if (*psz < '0' || *psz > '9')
            return FALSE;
        u = u * 10 + (*psz - '0');
        if (u > uMax)
            return FALSE;
    }
    if (u < uMin)
        return FALSE;
    *pu = (DWORD)u;
    return TRUE;
}

/**
 * Returns the size of a VBoxGuestPropEnum list, up to and including the empty
 * name that ends it.
 */
static DWORD vboxHostOptsListSize(const char *pszList)
{
    const char *psz = pszList;
    int i;

    while (*psz)
        for (i = 0; i < 4; i++)
            psz += strlen(psz) + 1;
    return (DWORD)(psz - pszList) + 1;
}

/**
 * Reads the host options matching the patterns.  Unknown options and invalid
 * values are logged and ignored.
 *
 * @returns TRUE if the options changed since the last call and pvOpts was
 *          filled in, FALSE if they didn't or couldn't be read (keep the
 *          current settings).
 * @param   pszService  Service name for the log.
 * @param   pszPatterns Full property names or patterns, separated by '|'.
 * @param   paOpts      The options known.
 * @param   pvOpts      The settings (DWORD fields): the caller puts its
 *                      "not set" values in them first.
 * @param   pCache      The list last read; free it with VBoxHostOptsFree.
 */
BOOL VBoxHostOptsRead(HANDLE hDevice, UINT32 idClient, const char *pszService, const char *pszPatterns,
                      const VBOXHOSTOPT *paOpts, unsigned cOpts, void *pvOpts, VBOXHOSTOPTSCACHE *pCache)
{
    const char *psz;
    char *pszList;
    DWORD cbList;
    int rc;

    rc = VBoxGuestPropEnum(hDevice, idClient, pszPatterns, &pszList);
    if (RT_FAILURE(rc))
    {
        if (!pCache->fLoggedFailure)
            VBoxTrayLogRel("%s: reading the host options failed, rc=%d\n", pszService, rc);
        pCache->fLoggedFailure = TRUE;
        return FALSE;
    }
    pCache->fLoggedFailure = FALSE;

    cbList = vboxHostOptsListSize(pszList);
    if (   pCache->pszList
        && cbList == pCache->cbList
        && !memcmp(pszList, pCache->pszList, cbList))
    {
        free(pszList);
        return FALSE;
    }

    for (psz = pszList; *psz; )
    {
        const char *pszName  = psz;
        const char *pszValue = pszName + strlen(pszName) + 1;
        const char *pszOpt   = pszName + sizeof(VBOXHOSTOPTS_PREFIX) - 1;
        unsigned i;

        psz = pszValue + strlen(pszValue) + 1;  /* timestamp */
        psz += strlen(psz) + 1;                 /* flags */
        psz += strlen(psz) + 1;                 /* next name */

        for (i = 0; i < cOpts; i++)
            if (!strcmp(pszOpt, paOpts[i].pszName))
                break;
        if (i >= cOpts)
            VBoxTrayLogRel("%s: unknown host option %.64s ignored\n", pszService, pszOpt);
        else
        {
            DWORD *pu = (DWORD *)((BYTE *)pvOpts + paOpts[i].offField);
            if (!paOpts[i].fNumber)
                *pu = paOpts[i].uMin;
            else if (!vboxHostOptsParseU32(pszValue, paOpts[i].uMin, paOpts[i].uMax, pu))
                VBoxTrayLogRel("%s: host option %s has an invalid value (%.64s), ignored; range %lu..%lu\n",
                               pszService, pszOpt, pszValue, paOpts[i].uMin, paOpts[i].uMax);
        }
    }

    free(pCache->pszList);
    pCache->pszList = pszList;
    pCache->cbList  = cbList;
    return TRUE;
}

/**
 * Frees the list kept by VBoxHostOptsRead.
 */
void VBoxHostOptsFree(VBOXHOSTOPTSCACHE *pCache)
{
    free(pCache->pszList);
    pCache->pszList = NULL;
    pCache->cbList  = 0;
}
