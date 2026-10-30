/** @file
 * VBoxTray - Guest property HGCM calls (VBoxGuestPropSvc), as upstream
 * VbglR3GuestPropWrite, VbglR3GuestPropWriteValue, VbglR3GuestPropRead,
 * VbglR3GuestPropEnum and VbglR3GuestPropWait.  Also built into VBoxControl.
 */

#include "VBoxTray.h"
#include <stdlib.h>
#include <string.h>


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/

/** Timeout for the calls (ms); the host answers them right away. */
#define VBOXGUESTPROP_CALL_TIMEOUT_MS   30000


/**
 * Writes or deletes a property.
 *
 * @returns VBox status code.
 * @param   hDevice     The device handle.
 * @param   idClient    The VBoxGuestPropSvc client.
 * @param   pszName     The property name.
 * @param   pszValue    The value (UTF-8), NULL to delete the property.
 * @param   pszFlags    The flags ("TRANSRESET" etc.), NULL to keep the
 *                      property's flags (none for a new one).
 */
int VBoxGuestPropWrite(HANDLE hDevice, UINT32 idClient, const char *pszName, const char *pszValue, const char *pszFlags)
{
    VBGLR3HGCMMSG Msg;
    DWORD cbName  = (DWORD)strlen(pszName) + 1;
    DWORD cbValue = pszValue ? (DWORD)strlen(pszValue) + 1 : 0;
    DWORD cbFlags = pszFlags ? (DWORD)strlen(pszFlags) + 1 : 0;
    int rc;

    if (!pszValue)
    {
        if (!VbglR3HgcmMsgInit(&Msg, idClient, GUEST_PROP_FN_DEL_PROP, GUEST_PROP_FN_DEL_PROP_CPARMS,
                               VBOXGUESTPROP_CALL_TIMEOUT_MS, cbName))
            return VERR_NO_MEMORY;
        VbglR3HgcmMsgSetBuf(&Msg, 0, VBOXGUEST98_HGCM_PARM_BUFFER_IN, cbName, pszName);
    }
    else if (!pszFlags)
    {
        if (!VbglR3HgcmMsgInit(&Msg, idClient, GUEST_PROP_FN_SET_PROP_VALUE, GUEST_PROP_FN_SET_PROP_VALUE_CPARMS,
                               VBOXGUESTPROP_CALL_TIMEOUT_MS, cbName + cbValue))
            return VERR_NO_MEMORY;
        VbglR3HgcmMsgSetBuf(&Msg, 0, VBOXGUEST98_HGCM_PARM_BUFFER_IN, cbName, pszName);
        VbglR3HgcmMsgSetBuf(&Msg, 1, VBOXGUEST98_HGCM_PARM_BUFFER_IN, cbValue, pszValue);
    }
    else
    {
        if (!VbglR3HgcmMsgInit(&Msg, idClient, GUEST_PROP_FN_SET_PROP, GUEST_PROP_FN_SET_PROP_CPARMS,
                               VBOXGUESTPROP_CALL_TIMEOUT_MS, cbName + cbValue + cbFlags))
            return VERR_NO_MEMORY;
        VbglR3HgcmMsgSetBuf(&Msg, 0, VBOXGUEST98_HGCM_PARM_BUFFER_IN, cbName, pszName);
        VbglR3HgcmMsgSetBuf(&Msg, 1, VBOXGUEST98_HGCM_PARM_BUFFER_IN, cbValue, pszValue);
        VbglR3HgcmMsgSetBuf(&Msg, 2, VBOXGUEST98_HGCM_PARM_BUFFER_IN, cbFlags, pszFlags);
    }

    rc = VbglR3HgcmMsgCall(hDevice, &Msg);
    VbglR3HgcmMsgFree(&Msg);
    return rc;
}

/**
 * Copies a string the host returned in a buffer, which it may have left
 * unterminated.
 *
 * @returns VINF_SUCCESS or VERR_BUFFER_OVERFLOW.
 * @param   ppch        The string; advanced past its terminator.
 * @param   pchEnd      The end of the buffer.
 * @param   psz         Where to copy it, NULL to skip it.
 */
static int vboxGuestPropGetString(const char **ppch, const char *pchEnd, char *psz, DWORD cb)
{
    const char *pch = *ppch;
    DWORD cch = 0;

    while (pch + cch < pchEnd && pch[cch])
        cch++;
    *ppch = pch + cch < pchEnd ? pch + cch + 1 : pchEnd;
    if (!psz)
        return VINF_SUCCESS;
    if (cch >= cb)
        return VERR_BUFFER_OVERFLOW;
    memcpy(psz, pch, cch);
    psz[cch] = '\0';
    return VINF_SUCCESS;
}

/**
 * Reads the value of a property.
 *
 * @returns VBox status code, VERR_NOT_FOUND if the property isn't set,
 *          VERR_BUFFER_OVERFLOW if the value doesn't fit.
 * @param   hDevice     The device handle.
 * @param   idClient    The VBoxGuestPropSvc client.
 * @param   pszName     The property name.
 * @param   pszValue    Where to return the value (UTF-8, zero terminated).
 * @param   cbValue     The size of the pszValue buffer.
 */
int VBoxGuestPropRead(HANDLE hDevice, UINT32 idClient, const char *pszName, char *pszValue, DWORD cbValue)
{
    return VBoxGuestPropReadEx(hDevice, idClient, pszName, pszValue, cbValue, NULL, NULL, 0);
}

/**
 * Reads the value, timestamp and flags of a property (as upstream
 * VbglR3GuestPropRead).
 *
 * @returns VBox status code, VERR_NOT_FOUND if the property isn't set,
 *          VERR_BUFFER_OVERFLOW if the value or the flags don't fit.
 * @param   puTimestamp Where to return the timestamp (ns since 1970), optional.
 * @param   pszFlags    Where to return the flags, optional.
 */
int VBoxGuestPropReadEx(HANDLE hDevice, UINT32 idClient, const char *pszName, char *pszValue, DWORD cbValue,
                        UINT64 *puTimestamp, char *pszFlags, DWORD cbFlags)
{
    VBGLR3HGCMMSG Msg;
    DWORD cbName = (DWORD)strlen(pszName) + 1;
    DWORD cbBuf  = GUEST_PROP_MAX_VALUE_LEN + GUEST_PROP_MAX_FLAGS_LEN; /* The value, then the flags. */
    const char *pch;
    int rc;

    if (!VbglR3HgcmMsgInit(&Msg, idClient, GUEST_PROP_FN_GET_PROP, GUEST_PROP_FN_GET_PROP_CPARMS,
                           VBOXGUESTPROP_CALL_TIMEOUT_MS, cbName + cbBuf))
        return VERR_NO_MEMORY;
    VbglR3HgcmMsgSetBuf(&Msg, 0, VBOXGUEST98_HGCM_PARM_BUFFER_IN, cbName, pszName);
    pch = (const char *)VbglR3HgcmMsgSetBuf(&Msg, 1, VBOXGUEST98_HGCM_PARM_BUFFER_OUT, cbBuf, NULL);
    VbglR3HgcmMsgSetU64(&Msg, 2, 0);
    VbglR3HgcmMsgSetU32(&Msg, 3, 0);

    rc = VbglR3HgcmMsgCall(hDevice, &Msg);
    if (RT_SUCCESS(rc))
    {
        const char *pchEnd = pch + cbBuf;
        rc = vboxGuestPropGetString(&pch, pchEnd, pszValue, cbValue);
        if (RT_SUCCESS(rc) && pszFlags)
            rc = vboxGuestPropGetString(&pch, pchEnd, pszFlags, cbFlags);
        if (puTimestamp)
            *puTimestamp = VbglR3HgcmMsgGetU64(&Msg, 2);
    }
    VbglR3HgcmMsgFree(&Msg);
    return rc;
}

/**
 * Lists the properties matching the patterns (as upstream VbglR3GuestPropEnum).
 *
 * @returns VBox status code.
 * @param   pszPatterns Patterns separated by '|', "" for all.
 * @param   ppszList    Where to return the list (malloc'ed): name, value,
 *                      timestamp (decimal) and flags strings for each
 *                      property, ending with an empty name.
 */
int VBoxGuestPropEnum(HANDLE hDevice, UINT32 idClient, const char *pszPatterns, char **ppszList)
{
    DWORD cbPatterns = (DWORD)strlen(pszPatterns) + 1;
    DWORD cbBuf = 4096;
    int rc;

    *ppszList = NULL;
    for (;;)
    {
        VBGLR3HGCMMSG Msg;
        BYTE *pb;
        DWORD cbNeeded;

        if (!VbglR3HgcmMsgInit(&Msg, idClient, GUEST_PROP_FN_ENUM_PROPS, GUEST_PROP_FN_ENUM_PROPS_CPARMS,
                               VBOXGUESTPROP_CALL_TIMEOUT_MS, cbPatterns + cbBuf))
            return VERR_NO_MEMORY;
        VbglR3HgcmMsgSetBuf(&Msg, 0, VBOXGUEST98_HGCM_PARM_BUFFER_IN, cbPatterns, pszPatterns);
        pb = VbglR3HgcmMsgSetBuf(&Msg, 1, VBOXGUEST98_HGCM_PARM_BUFFER_OUT, cbBuf, NULL);
        VbglR3HgcmMsgSetU32(&Msg, 2, 0);
        rc = VbglR3HgcmMsgCall(hDevice, &Msg);
        cbNeeded = VbglR3HgcmMsgGetU32(&Msg, 2);
        if (RT_SUCCESS(rc))
        {
            /* Make sure the list ends even if the host's doesn't: four
             * terminators past the data. */
            *ppszList = (char *)malloc(cbBuf + 4);
            if (*ppszList)
            {
                memcpy(*ppszList, pb, cbBuf);
                memset(*ppszList + cbBuf, 0, 4);
            }
            else
                rc = VERR_NO_MEMORY;
        }
        VbglR3HgcmMsgFree(&Msg);
        /* Properties may be added between the calls, so ask for some more. */
        if (rc != VERR_BUFFER_OVERFLOW || cbBuf >= 1024 * 1024)
            return rc;
        cbBuf = cbNeeded > cbBuf ? cbNeeded + 1024 : cbBuf * 2;
    }
}

/**
 * Waits for a property matching the patterns to change (as upstream
 * VbglR3GuestPropWait).
 *
 * @returns VBox status code: VERR_TIMEOUT, VWRN_NOT_FOUND if changes were
 *          missed (the oldest one known is returned), VERR_BUFFER_OVERFLOW.
 * @param   pszPatterns Patterns separated by '|', "" for all.
 * @param   cMsTimeout  How long to wait (ms).
 * @param   puTimestamp In: the timestamp of the last change seen, 0 for the
 *                      next change.  Out: the timestamp of this change.
 * @param   pszValue    Where to return the value, empty if the property was
 *                      deleted.
 */
int VBoxGuestPropWait(HANDLE hDevice, UINT32 idClient, const char *pszPatterns, UINT32 cMsTimeout, UINT64 *puTimestamp,
                      char *pszName, DWORD cbName, char *pszValue, DWORD cbValue, char *pszFlags, DWORD cbFlags)
{
    VBGLR3HGCMMSG Msg;
    DWORD cbPatterns = (DWORD)strlen(pszPatterns) + 1;
    DWORD cbBuf = GUEST_PROP_MAX_NAME_LEN + GUEST_PROP_MAX_VALUE_LEN + GUEST_PROP_MAX_FLAGS_LEN;
    const char *pch;
    int rc;

    if (!VbglR3HgcmMsgInit(&Msg, idClient, GUEST_PROP_FN_GET_NOTIFICATION, GUEST_PROP_FN_GET_NOTIFICATION_CPARMS,
                           cMsTimeout, cbPatterns + cbBuf))
        return VERR_NO_MEMORY;
    VbglR3HgcmMsgSetBuf(&Msg, 0, VBOXGUEST98_HGCM_PARM_BUFFER_IN, cbPatterns, pszPatterns);
    VbglR3HgcmMsgSetU64(&Msg, 1, *puTimestamp);
    pch = (const char *)VbglR3HgcmMsgSetBuf(&Msg, 2, VBOXGUEST98_HGCM_PARM_BUFFER_OUT, cbBuf, NULL);
    VbglR3HgcmMsgSetU32(&Msg, 3, 0);

    rc = VbglR3HgcmMsgCall(hDevice, &Msg);
    if (RT_SUCCESS(rc))
    {
        const char *pchEnd = pch + cbBuf;
        int rc2 = vboxGuestPropGetString(&pch, pchEnd, pszName, cbName);
        if (RT_SUCCESS(rc2))
            rc2 = vboxGuestPropGetString(&pch, pchEnd, pszValue, cbValue);
        if (RT_SUCCESS(rc2))
            rc2 = vboxGuestPropGetString(&pch, pchEnd, pszFlags, cbFlags);
        if (RT_FAILURE(rc2))
            rc = rc2;
        *puTimestamp = VbglR3HgcmMsgGetU64(&Msg, 1);
    }
    VbglR3HgcmMsgFree(&Msg);
    return rc;
}

