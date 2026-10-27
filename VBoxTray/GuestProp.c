/** @file
 * VBoxTray - Guest property HGCM calls (VBoxGuestPropSvc), as upstream
 * VbglR3GuestPropWrite, VbglR3GuestPropWriteValue and VbglR3GuestPropRead.
 */

#include "VBoxTray.h"
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
    VBGLR3HGCMMSG Msg;
    DWORD cbName = (DWORD)strlen(pszName) + 1;
    DWORD cbBuf  = GUEST_PROP_MAX_VALUE_LEN + GUEST_PROP_MAX_NAME_LEN; /* The value, then the flags. */
    BYTE *pbBuf;
    int rc;

    if (!VbglR3HgcmMsgInit(&Msg, idClient, GUEST_PROP_FN_GET_PROP, GUEST_PROP_FN_GET_PROP_CPARMS,
                           VBOXGUESTPROP_CALL_TIMEOUT_MS, cbName + cbBuf))
        return VERR_NO_MEMORY;
    VbglR3HgcmMsgSetBuf(&Msg, 0, VBOXGUEST98_HGCM_PARM_BUFFER_IN, cbName, pszName);
    pbBuf = VbglR3HgcmMsgSetBuf(&Msg, 1, VBOXGUEST98_HGCM_PARM_BUFFER_OUT, cbBuf, NULL);
    VbglR3HgcmMsgSetU64(&Msg, 2, 0);
    VbglR3HgcmMsgSetU32(&Msg, 3, 0);

    rc = VbglR3HgcmMsgCall(hDevice, &Msg);
    if (RT_SUCCESS(rc))
    {
        DWORD cch = 0;
        while (cch < cbBuf && pbBuf[cch])
            cch++;
        if (cch < cbValue)
        {
            memcpy(pszValue, pbBuf, cch);
            pszValue[cch] = '\0';
        }
        else
            rc = VERR_BUFFER_OVERFLOW;
    }
    VbglR3HgcmMsgFree(&Msg);
    return rc;
}
