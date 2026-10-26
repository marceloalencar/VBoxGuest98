/** @file
 * VBoxTray - Drag and drop, the VBoxDragAndDropSvc HGCM calls (protocol v3),
 * as upstream VbglR3DnD.cpp.
 */

#include "DnD.h"
#include <stdlib.h>
#include <string.h>


/*********************************************************************************************************************************
*   HGCM calls                                                                                                                   *
*********************************************************************************************************************************/

/**
 * Connects to the service and does the protocol handshake (VbglR3DnDConnect).
 */
int VBoxDnDConnect(HANDLE hDevice, UINT32 *pidClient)
{
    VBGLR3HGCMMSG Msg;
    int rc;

    rc = VbglR3HgcmConnect(hDevice, VBOX_DND_SERVICE_NAME, pidClient);
    if (RT_FAILURE(rc))
        return rc;

    if (!VbglR3HgcmMsgInit(&Msg, *pidClient, GUEST_DND_FN_CONNECT, 3, VBOXDND_CALL_TIMEOUT_MS, 0))
        rc = VERR_NO_MEMORY;
    else
    {
        VbglR3HgcmMsgSetU32(&Msg, 0, 0);                           /* uContext */
        VbglR3HgcmMsgSetU32(&Msg, 1, VBOX_DND_PROTOCOL_VERSION);   /* uProtocol */
        VbglR3HgcmMsgSetU32(&Msg, 2, 0);                           /* uFlags */
        rc = VbglR3HgcmMsgCall(hDevice, &Msg);
        VbglR3HgcmMsgFree(&Msg);
    }

    /* Reporting the features fails on older hosts, which is fine. */
    if (RT_SUCCESS(rc) && VbglR3HgcmMsgInit(&Msg, *pidClient, GUEST_DND_FN_REPORT_FEATURES, 2, VBOXDND_CALL_TIMEOUT_MS, 0))
    {
        VbglR3HgcmMsgSetU64(&Msg, 0, VBOX_DND_GF_NONE);
        VbglR3HgcmMsgSetU64(&Msg, 1, VBOX_DND_GF_1_MUST_BE_ONE);
        VbglR3HgcmMsgCall(hDevice, &Msg);
        VbglR3HgcmMsgFree(&Msg);
    }

    if (RT_FAILURE(rc))
    {
        VbglR3HgcmDisconnect(hDevice, *pidClient);
        *pidClient = 0;
    }
    return rc;
}

/**
 * Waits for the next host message.
 *
 * @returns VBox status code; VERR_CANCELLED if the host cancelled the
 *          current operation (HOST_DND_FN_CANCEL).
 */
int VBoxDnDGetNextMsg(HANDLE hDevice, UINT32 idClient, UINT32 *puMsg, UINT32 *pcParms)
{
    VBGLR3HGCMMSG Msg;
    int rc;

    do
    {
        if (!VbglR3HgcmMsgInit(&Msg, idClient, GUEST_DND_FN_GET_NEXT_HOST_MSG, 3, VBOXGUEST98_WAIT_INFINITE, 0))
            return VERR_NO_MEMORY;
        VbglR3HgcmMsgSetU32(&Msg, 0, 0);   /* uMsg */
        VbglR3HgcmMsgSetU32(&Msg, 1, 0);   /* cParms */
        VbglR3HgcmMsgSetU32(&Msg, 2, 1);   /* fBlock */
        rc = VbglR3HgcmMsgCall(hDevice, &Msg);
        if (RT_SUCCESS(rc))
        {
            *puMsg   = VbglR3HgcmMsgGetU32(&Msg, 0);
            *pcParms = VbglR3HgcmMsgGetU32(&Msg, 1);
        }
        VbglR3HgcmMsgFree(&Msg);
    } while (rc == VERR_INTERRUPTED && !g_fVBoxTrayShutdown);
    return rc;
}

/**
 * Fetches a message that only has the context ID (leave, cancel).
 */
int VBoxDnDRecvSimple(HANDLE hDevice, UINT32 idClient, UINT32 uMsg)
{
    VBGLR3HGCMMSG Msg;
    int rc;

    if (!VbglR3HgcmMsgInit(&Msg, idClient, uMsg, 1, VBOXDND_CALL_TIMEOUT_MS, 0))
        return VERR_NO_MEMORY;
    VbglR3HgcmMsgSetU32(&Msg, 0, 0);
    rc = VbglR3HgcmMsgCall(hDevice, &Msg);
    VbglR3HgcmMsgFree(&Msg);
    return rc;
}

/**
 * Fetches an "action" message: enter, move or dropped (vbglR3DnDHGRecvAction).
 *
 * @param   ppEnter     Where to return the enter data (malloc'ed), NULL if not wanted.
 */
int VBoxDnDRecvAction(HANDLE hDevice, UINT32 idClient, UINT32 uMsg, UINT32 *px, UINT32 *py,
                      VBOXDNDENTER **ppEnter)
{
    VBGLR3HGCMMSG Msg;
    BYTE *pbFormats;
    UINT32 cbFormats;
    int rc;

    if (!VbglR3HgcmMsgInit(&Msg, idClient, uMsg, 8, VBOXDND_CALL_TIMEOUT_MS, VBOX_DND_CHUNK_SIZE))
        return VERR_NO_MEMORY;
    VbglR3HgcmMsgSetU32(&Msg, 0, 0);   /* uContext */
    VbglR3HgcmMsgSetU32(&Msg, 1, 0);   /* uScreenId */
    VbglR3HgcmMsgSetU32(&Msg, 2, 0);   /* uX */
    VbglR3HgcmMsgSetU32(&Msg, 3, 0);   /* uY */
    VbglR3HgcmMsgSetU32(&Msg, 4, 0);   /* uDefAction */
    VbglR3HgcmMsgSetU32(&Msg, 5, 0);   /* uAllActions */
    pbFormats = VbglR3HgcmMsgSetBuf(&Msg, 6, VBOXGUEST98_HGCM_PARM_BUFFER_OUT, VBOX_DND_CHUNK_SIZE, NULL);
    VbglR3HgcmMsgSetU32(&Msg, 7, 0);   /* cbFormats */

    rc = VbglR3HgcmMsgCall(hDevice, &Msg);
    if (RT_SUCCESS(rc))
    {
        *px = VbglR3HgcmMsgGetU32(&Msg, 2);
        *py = VbglR3HgcmMsgGetU32(&Msg, 3);
        if (ppEnter)
        {
            cbFormats = VbglR3HgcmMsgGetBufSize(&Msg, 6);
            if (cbFormats > VBOX_DND_CHUNK_SIZE)
                cbFormats = VBOX_DND_CHUNK_SIZE;
            *ppEnter = (VBOXDNDENTER *)malloc(sizeof(VBOXDNDENTER) + cbFormats);
            if (*ppEnter)
            {
                (*ppEnter)->x           = *px;
                (*ppEnter)->y           = *py;
                (*ppEnter)->fAllActions = VbglR3HgcmMsgGetU32(&Msg, 5);
                memcpy((*ppEnter)->szFormats, pbFormats, cbFormats);
                (*ppEnter)->szFormats[cbFormats] = '\0';
            }
            else
                rc = VERR_NO_MEMORY;
        }
    }
    VbglR3HgcmMsgFree(&Msg);
    return rc;
}

/**
 * Tells the host the action the guest would do at the current position.
 */
int VBoxDnDSendAck(HANDLE hDevice, UINT32 idClient, UINT32 uAction)
{
    VBGLR3HGCMMSG Msg;
    int rc;

    if (!VbglR3HgcmMsgInit(&Msg, idClient, GUEST_DND_FN_HG_ACK_OP, 2, VBOXDND_CALL_TIMEOUT_MS, 0))
        return VERR_NO_MEMORY;
    VbglR3HgcmMsgSetU32(&Msg, 0, 0);
    VbglR3HgcmMsgSetU32(&Msg, 1, uAction);
    rc = VbglR3HgcmMsgCall(hDevice, &Msg);
    VbglR3HgcmMsgFree(&Msg);
    return rc;
}

/**
 * Asks the host for the dropped data in a format.
 */
int VBoxDnDSendReqData(HANDLE hDevice, UINT32 idClient, const char *pszFormat)
{
    DWORD cbFormat = lstrlen(pszFormat) + 1;
    VBGLR3HGCMMSG Msg;
    int rc;

    if (!VbglR3HgcmMsgInit(&Msg, idClient, GUEST_DND_FN_HG_REQ_DATA, 3, VBOXDND_CALL_TIMEOUT_MS, cbFormat))
        return VERR_NO_MEMORY;
    VbglR3HgcmMsgSetU32(&Msg, 0, 0);
    VbglR3HgcmMsgSetBuf(&Msg, 1, VBOXGUEST98_HGCM_PARM_BUFFER_IN, cbFormat, pszFormat);
    VbglR3HgcmMsgSetU32(&Msg, 2, cbFormat);
    rc = VbglR3HgcmMsgCall(hDevice, &Msg);
    VbglR3HgcmMsgFree(&Msg);
    return rc;
}

/**
 * Reports the transfer status to the host.
 */
int VBoxDnDSendProgress(HANDLE hDevice, UINT32 idClient, UINT32 uStatus, UINT32 uPercent, int rcErr)
{
    VBGLR3HGCMMSG Msg;
    int rc;

    if (!VbglR3HgcmMsgInit(&Msg, idClient, GUEST_DND_FN_HG_EVT_PROGRESS, 4, VBOXDND_CALL_TIMEOUT_MS, 0))
        return VERR_NO_MEMORY;
    VbglR3HgcmMsgSetU32(&Msg, 0, 0);
    VbglR3HgcmMsgSetU32(&Msg, 1, uStatus);
    VbglR3HgcmMsgSetU32(&Msg, 2, uPercent);
    VbglR3HgcmMsgSetU32(&Msg, 3, (UINT32)rcErr);
    rc = VbglR3HgcmMsgCall(hDevice, &Msg);
    VbglR3HgcmMsgFree(&Msg);
    return rc;
}

/**
 * Fetches the host's "is a guest drag pending" query (uContext, uScreenId).
 */
int VBoxDnDRecvGhPending(HANDLE hDevice, UINT32 idClient)
{
    VBGLR3HGCMMSG Msg;
    int rc;

    if (!VbglR3HgcmMsgInit(&Msg, idClient, HOST_DND_FN_GH_REQ_PENDING, 2, VBOXDND_CALL_TIMEOUT_MS, 0))
        return VERR_NO_MEMORY;
    VbglR3HgcmMsgSetU32(&Msg, 0, 0);
    VbglR3HgcmMsgSetU32(&Msg, 1, 0);
    rc = VbglR3HgcmMsgCall(hDevice, &Msg);
    VbglR3HgcmMsgFree(&Msg);
    return rc;
}

/**
 * Answers the "is a guest drag pending" query.
 *
 * @param   pszFormats  The formats of the guest drag, "" (with
 *                      VBOX_DND_ACTION_IGNORE) if there is none.
 */
int VBoxDnDSendAckPending(HANDLE hDevice, UINT32 idClient, UINT32 uAction, const char *pszFormats)
{
    DWORD cbFormats = lstrlen(pszFormats) + 1;
    VBGLR3HGCMMSG Msg;
    int rc;

    if (!VbglR3HgcmMsgInit(&Msg, idClient, GUEST_DND_FN_GH_ACK_PENDING, 5, VBOXDND_CALL_TIMEOUT_MS, cbFormats))
        return VERR_NO_MEMORY;
    VbglR3HgcmMsgSetU32(&Msg, 0, 0);                           /* uContext */
    VbglR3HgcmMsgSetU32(&Msg, 1, uAction);                     /* uDefAction */
    VbglR3HgcmMsgSetU32(&Msg, 2, uAction);                     /* uAllActions */
    VbglR3HgcmMsgSetBuf(&Msg, 3, VBOXGUEST98_HGCM_PARM_BUFFER_IN, cbFormats, pszFormats);
    VbglR3HgcmMsgSetU32(&Msg, 4, cbFormats);                   /* cbFormats */
    rc = VbglR3HgcmMsgCall(hDevice, &Msg);
    VbglR3HgcmMsgFree(&Msg);
    return rc;
}

/**
 * Fetches HOST_DND_FN_GH_EVT_DROPPED: the guest drag was dropped on the host.
 *
 * @param   pszFormat   Where to return the format the host wants.
 * @param   cbFormat    Size of pszFormat.
 */
int VBoxDnDRecvGhDropped(HANDLE hDevice, UINT32 idClient, char *pszFormat, DWORD cbFormat)
{
    VBGLR3HGCMMSG Msg;
    BYTE *pb;
    int rc;

    if (!VbglR3HgcmMsgInit(&Msg, idClient, HOST_DND_FN_GH_EVT_DROPPED, 4, VBOXDND_CALL_TIMEOUT_MS, VBOX_DND_CHUNK_SIZE))
        return VERR_NO_MEMORY;
    VbglR3HgcmMsgSetU32(&Msg, 0, 0);                                                               /* uContext */
    pb = VbglR3HgcmMsgSetBuf(&Msg, 1, VBOXGUEST98_HGCM_PARM_BUFFER_OUT, VBOX_DND_CHUNK_SIZE, NULL);    /* pvFormat */
    VbglR3HgcmMsgSetU32(&Msg, 2, 0);                                                               /* cbFormat */
    VbglR3HgcmMsgSetU32(&Msg, 3, 0);                                                               /* uAction */
    rc = VbglR3HgcmMsgCall(hDevice, &Msg);
    if (RT_SUCCESS(rc))
        lstrcpyn(pszFormat, (const char *)pb, cbFormat < VBOX_DND_CHUNK_SIZE ? cbFormat : VBOX_DND_CHUNK_SIZE);
    VbglR3HgcmMsgFree(&Msg);
    return rc;
}

/**
 * Tells the host the guest data can't be sent, so it doesn't wait for it.
 */
int VBoxDnDSendError(HANDLE hDevice, UINT32 idClient, int rcErr)
{
    VBGLR3HGCMMSG Msg;
    int rc;

    if (!VbglR3HgcmMsgInit(&Msg, idClient, GUEST_DND_FN_EVT_ERROR, 2, VBOXDND_CALL_TIMEOUT_MS, 0))
        return VERR_NO_MEMORY;
    VbglR3HgcmMsgSetU32(&Msg, 0, 0);
    VbglR3HgcmMsgSetU32(&Msg, 1, (UINT32)rcErr);
    rc = VbglR3HgcmMsgCall(hDevice, &Msg);
    VbglR3HgcmMsgFree(&Msg);
    return rc;
}

/**
 * Sends the data header and the meta data of a guest to host transfer
 * (vbglR3DnDGHSendDataInternal).
 *
 * @param   pszMetaFmt  "text/uri-list" for files, NULL for raw data.
 * @param   cbTotal     Meta data plus file bytes.
 */
int VBoxDnDGhSendMeta(HANDLE hDevice, UINT32 idClient, const char *pszMetaFmt, const void *pvMeta, DWORD cbMeta,
                      UINT64 cbTotal, UINT64 cObjects)
{
    DWORD cbMetaFmt = pszMetaFmt ? lstrlen(pszMetaFmt) + 1 : 0;
    DWORD cbSent = 0;
    VBGLR3HGCMMSG Msg;
    int rc;

    if (!VbglR3HgcmMsgInit(&Msg, idClient, GUEST_DND_FN_GH_SND_DATA_HDR, 12, VBOXDND_CALL_TIMEOUT_MS, cbMetaFmt))
        return VERR_NO_MEMORY;
    VbglR3HgcmMsgSetU32(&Msg, 0, 0);                                                           /* uContext */
    VbglR3HgcmMsgSetU32(&Msg, 1, 0);                                                           /* uFlags */
    VbglR3HgcmMsgSetU32(&Msg, 2, 0);                                                           /* uScreenId */
    VbglR3HgcmMsgSetU64(&Msg, 3, cbTotal);                                                     /* cbTotal */
    VbglR3HgcmMsgSetU32(&Msg, 4, cbMeta);                                                      /* cbMeta */
    VbglR3HgcmMsgSetBuf(&Msg, 5, VBOXGUEST98_HGCM_PARM_BUFFER_IN, cbMetaFmt, pszMetaFmt);      /* pvMetaFmt */
    VbglR3HgcmMsgSetU32(&Msg, 6, cbMetaFmt);                                                   /* cbMetaFmt */
    VbglR3HgcmMsgSetU64(&Msg, 7, cObjects);                                                    /* cObjects */
    VbglR3HgcmMsgSetU32(&Msg, 8, 0);                                                           /* enmCompression */
    VbglR3HgcmMsgSetU32(&Msg, 9, 0);                                                           /* enmChecksumType */
    VbglR3HgcmMsgSetBuf(&Msg, 10, VBOXGUEST98_HGCM_PARM_BUFFER_IN, 0, NULL);                   /* pvChecksum */
    VbglR3HgcmMsgSetU32(&Msg, 11, 0);                                                          /* cbChecksum */
    rc = VbglR3HgcmMsgCall(hDevice, &Msg);
    VbglR3HgcmMsgFree(&Msg);

    while (RT_SUCCESS(rc) && cbSent < cbMeta)
    {
        DWORD cbChunk = cbMeta - cbSent < VBOX_DND_CHUNK_SIZE ? cbMeta - cbSent : VBOX_DND_CHUNK_SIZE;
        if (!VbglR3HgcmMsgInit(&Msg, idClient, GUEST_DND_FN_GH_SND_DATA, 5, VBOXDND_CALL_TIMEOUT_MS, cbChunk))
            return VERR_NO_MEMORY;
        VbglR3HgcmMsgSetU32(&Msg, 0, 0);                                                               /* uContext */
        VbglR3HgcmMsgSetBuf(&Msg, 1, VBOXGUEST98_HGCM_PARM_BUFFER_IN, cbChunk, (const BYTE *)pvMeta + cbSent); /* pvData */
        VbglR3HgcmMsgSetU32(&Msg, 2, cbChunk);                                                         /* cbData */
        VbglR3HgcmMsgSetBuf(&Msg, 3, VBOXGUEST98_HGCM_PARM_BUFFER_IN, 0, NULL);                        /* pvChecksum */
        VbglR3HgcmMsgSetU32(&Msg, 4, 0);                                                               /* cbChecksum */
        rc = VbglR3HgcmMsgCall(hDevice, &Msg);
        VbglR3HgcmMsgFree(&Msg);
        cbSent += cbChunk;
    }
    return rc;
}

/**
 * Sends a directory of a guest to host transfer.
 *
 * @param   pszRel      Its path in the transfer (UTF-8, '/' separated).
 */
int VBoxDnDGhSendDir(HANDLE hDevice, UINT32 idClient, const char *pszRel)
{
    DWORD cbRel = lstrlen(pszRel) + 1;
    VBGLR3HGCMMSG Msg;
    int rc;

    if (!VbglR3HgcmMsgInit(&Msg, idClient, GUEST_DND_FN_GH_SND_DIR, 4, VBOXDND_CALL_TIMEOUT_MS, cbRel))
        return VERR_NO_MEMORY;
    VbglR3HgcmMsgSetU32(&Msg, 0, 0);                                                   /* uContext */
    VbglR3HgcmMsgSetBuf(&Msg, 1, VBOXGUEST98_HGCM_PARM_BUFFER_IN, cbRel, pszRel);      /* pvName */
    VbglR3HgcmMsgSetU32(&Msg, 2, cbRel);                                               /* cbName */
    VbglR3HgcmMsgSetU32(&Msg, 3, VBOX_DND_MODE_DIR);                                   /* fMode */
    rc = VbglR3HgcmMsgCall(hDevice, &Msg);
    VbglR3HgcmMsgFree(&Msg);
    return rc;
}

/**
 * Sends a file of a guest to host transfer: the header, then the data in chunks.
 *
 * @param   pszAbs      The guest file.
 * @param   pszRel      Its path in the transfer (UTF-8, '/' separated).
 * @param   cbFile      Its size, as announced in the data header.
 */
int VBoxDnDGhSendFile(HANDLE hDevice, UINT32 idClient, const char *pszAbs, const char *pszRel, UINT64 cbFile)
{
    DWORD cbRel = lstrlen(pszRel) + 1;
    UINT64 cbLeft = cbFile;
    VBGLR3HGCMMSG Msg;
    HANDLE hFile;
    int rc;

    hFile = CreateFile(pszAbs, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return VERR_OPEN_FAILED;

    if (!VbglR3HgcmMsgInit(&Msg, idClient, GUEST_DND_FN_GH_SND_FILE_HDR, 6, VBOXDND_CALL_TIMEOUT_MS, cbRel))
    {
        CloseHandle(hFile);
        return VERR_NO_MEMORY;
    }
    VbglR3HgcmMsgSetU32(&Msg, 0, 0);                                                   /* uContext */
    VbglR3HgcmMsgSetBuf(&Msg, 1, VBOXGUEST98_HGCM_PARM_BUFFER_IN, cbRel, pszRel);      /* pvName */
    VbglR3HgcmMsgSetU32(&Msg, 2, cbRel);                                               /* cbName */
    VbglR3HgcmMsgSetU32(&Msg, 3, 0);                                                   /* uFlags */
    VbglR3HgcmMsgSetU32(&Msg, 4, VBOX_DND_MODE_FILE);                                  /* fMode */
    VbglR3HgcmMsgSetU64(&Msg, 5, cbFile);                                              /* cbTotal */
    rc = VbglR3HgcmMsgCall(hDevice, &Msg);
    VbglR3HgcmMsgFree(&Msg);

    while (RT_SUCCESS(rc) && cbLeft)
    {
        DWORD cbChunk = cbLeft < VBOX_DND_CHUNK_SIZE ? (DWORD)cbLeft : VBOX_DND_CHUNK_SIZE;
        DWORD cbRead = 0;
        BYTE *pb;

        if (!VbglR3HgcmMsgInit(&Msg, idClient, GUEST_DND_FN_GH_SND_FILE_DATA, 5, VBOXDND_CALL_TIMEOUT_MS, cbChunk))
        {
            rc = VERR_NO_MEMORY;
            break;
        }
        VbglR3HgcmMsgSetU32(&Msg, 0, 0);                                                       /* uContext */
        pb = VbglR3HgcmMsgSetBuf(&Msg, 1, VBOXGUEST98_HGCM_PARM_BUFFER_IN, cbChunk, NULL);     /* pvData */
        VbglR3HgcmMsgSetU32(&Msg, 2, cbChunk);                                                 /* cbData */
        VbglR3HgcmMsgSetBuf(&Msg, 3, VBOXGUEST98_HGCM_PARM_BUFFER_IN, 0, NULL);                /* pvChecksum */
        VbglR3HgcmMsgSetU32(&Msg, 4, 0);                                                       /* cbChecksum */
        if (!ReadFile(hFile, pb, cbChunk, &cbRead, NULL) || cbRead != cbChunk)
            rc = VERR_READ_ERROR; /* The file changed size since we looked: the host counts on the announced size. */
        else
            rc = VbglR3HgcmMsgCall(hDevice, &Msg);
        VbglR3HgcmMsgFree(&Msg);
        cbLeft -= cbChunk;
    }

    CloseHandle(hFile);
    return rc;
}
