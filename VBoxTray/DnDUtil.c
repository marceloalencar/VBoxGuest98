/** @file
 * VBoxTray - Drag and drop, CF_TEXT data from the host's text (the paths and
 * CF_HDROP data are in Util.c).
 */

#include "DnD.h"
#include <stdlib.h>
#include <string.h>


/*********************************************************************************************************************************
*   Text and names                                                                                                               *
*********************************************************************************************************************************/

/**
 * Builds the CF_TEXT data from UTF-8 text: ANSI, CRLF line ends.
 */
int VBoxDnDBuildText(const char *pch, DWORD cch, void **ppv, DWORD *pcb)
{
    char *pszAnsi;
    char *pszRet;
    DWORD cLf = 0;
    DWORD offSrc;
    DWORD offDst = 0;

    pszAnsi = VBoxTrayUtf8ToAnsi(pch, cch, '?');
    if (!pszAnsi)
        return VERR_NO_MEMORY;
    for (offSrc = 0; pszAnsi[offSrc]; offSrc++)
        if (pszAnsi[offSrc] == '\n' && (offSrc == 0 || pszAnsi[offSrc - 1] != '\r'))
            cLf++;

    pszRet = (char *)malloc(offSrc + cLf + 1);
    if (!pszRet)
    {
        free(pszAnsi);
        return VERR_NO_MEMORY;
    }
    for (offSrc = 0; pszAnsi[offSrc]; offSrc++)
    {
        if (pszAnsi[offSrc] == '\n' && (offSrc == 0 || pszAnsi[offSrc - 1] != '\r'))
            pszRet[offDst++] = '\r';
        pszRet[offDst++] = pszAnsi[offSrc];
    }
    pszRet[offDst++] = '\0';
    free(pszAnsi);

    *ppv = pszRet;
    *pcb = offDst;
    return VINF_SUCCESS;
}
