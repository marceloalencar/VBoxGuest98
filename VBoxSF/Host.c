/** @file
 * VBoxSF - The VBoxSharedFolders HGCM client.
 *
 * VBoxSF opens VBoxGst through NTKERN and uses its HGCM I/O controls like
 * VBoxTray does, so it is a session of its own on the driver with a single
 * client of the host service.  Names are UTF-16 (no SHFL_FN_SET_UTF8): the
 * parsed paths IFSMgr hands over are Unicode already.
 */
#include "VBoxSF.h"

#pragma VxD_LOCKED_CODE_SEG
#pragma VxD_LOCKED_DATA_SEG


/*********************************************************************************************************************************
*   Global Variables                                                                                                             *
*********************************************************************************************************************************/

/** The VBoxGst handle, NULL until the first successful SfHostConnect. */
static HANDLE g_hDevice;
/** Our VBoxSharedFolders client. */
static UINT32 g_idClient;
/** The driver's direct HGCM call entry point, for paging I/O. */
static VBOXGUEST98DIRECTHGCM g_Direct;

/** Names VBoxGst can be opened by, tried in order. */
static WCHAR g_wszDevNameNt[]  = VBOXGUEST98_DEVICE_NAME_NT;
static WCHAR g_wszDevNameDos[] = VBOXGUEST98_DEVICE_NAME_DOS;


/*********************************************************************************************************************************
*   VBoxGst access                                                                                                               *
*********************************************************************************************************************************/

static ULONG sfWcsLen(const WCHAR *pwsz)
{
    ULONG cwc = 0;
    while (pwsz[cwc])
        cwc++;
    return cwc;
}

static NTSTATUS sfOpenDevice(WCHAR *pwszName, HANDLE *phDevice)
{
    SFUNICODESTRING Name;
    SFOBJECTATTRIBUTES ObjAttr;
    SFIOSTATUSBLOCK Ios;

    Name.Length        = (USHORT)(sfWcsLen(pwszName) * sizeof(WCHAR));
    Name.MaximumLength = (USHORT)(Name.Length + sizeof(WCHAR));
    Name.Buffer        = pwszName;
    SfMemZero(&ObjAttr, sizeof(ObjAttr));
    ObjAttr.Length     = sizeof(ObjAttr);
    ObjAttr.ObjectName = &Name;
    ObjAttr.Attributes = SF_OBJ_CASE_INSENSITIVE;
    SfMemZero(&Ios, sizeof(Ios));
    return SfNtCreateFile(phDevice, SF_SYNCHRONIZE | SF_GENERIC_READ | SF_GENERIC_WRITE, &ObjAttr, &Ios, NULL, 0,
                          SF_FILE_SHARE_READ | SF_FILE_SHARE_WRITE, SF_FILE_OPEN, SF_FILE_SYNCHRONOUS_IO_NONALERT,
                          NULL, 0);
}

/** An open of VBoxGst done by an NTKERN worker thread. */
typedef struct SFOPENWORK
{
    SFWORKQUEUEITEM Item;
    WCHAR          *pwszName;
    HANDLE          hDevice;
    NTSTATUS        rcNt;
    PVOID           hSemDone;
} SFOPENWORK;

static void __stdcall sfOpenDeviceWorker(PVOID pvParameter)
{
    SFOPENWORK *pWork = (SFOPENWORK *)pvParameter;
    pWork->rcNt = sfOpenDevice(pWork->pwszName, &pWork->hDevice);
    SfSignalSemaphore(pWork->hSemDone);
}

/**
 * Opens VBoxGst in the system's context.  NTKERN handles belong to the
 * process they are opened in, and the first use of a share (which opens the
 * driver if it started after Init_Complete) can come from any program; an
 * NTKERN worker thread runs in the system's context, as Init_Complete.
 */
static NTSTATUS sfOpenDeviceInSystem(WCHAR *pwszName, HANDLE *phDevice, BOOL fSystemContext)
{
    SFOPENWORK Work;

    if (fSystemContext)
        return sfOpenDevice(pwszName, phDevice);
    SfMemZero(&Work, sizeof(Work));
    Work.hSemDone = SfCreateSemaphore(0);
    if (!Work.hSemDone)
        return (NTSTATUS)0xc000009a; /* STATUS_INSUFFICIENT_RESOURCES */
    Work.Item.pfnWorker   = sfOpenDeviceWorker;
    Work.Item.pvParameter = &Work;
    Work.pwszName         = pwszName;
    Work.rcNt             = (NTSTATUS)0xc0000001; /* STATUS_UNSUCCESSFUL */
    SfNtQueueWorkItem(&Work.Item, SF_DELAYED_WORK_QUEUE);
    SfWaitSemaphore(Work.hSemDone, BLOCK_SVC_INTS);
    SfDestroySemaphore(Work.hSemDone);
    if (NT_SUCCESS(Work.rcNt))
        *phDevice = Work.hDevice;
    return Work.rcNt;
}

/**
 * Sends an I/O control to VBoxGst; the buffer is both input and output.
 *
 * @returns VBox status code (of the I/O control itself).
 */
static int sfHostIoCtl(ULONG uCode, void *pvBuf, ULONG cbBuf)
{
    SFIOSTATUSBLOCK Ios;
    NTSTATUS rcNt;

    SfMemZero(&Ios, sizeof(Ios));
    Ios.Status = -1;
    rcNt = SfNtDeviceIoControl(g_hDevice, NULL, NULL, NULL, &Ios, uCode, pvBuf, cbBuf, pvBuf, cbBuf);
#ifdef VBOXSF_LOG_CALLS
    if (rcNt != 0 || Ios.Status != 0 || Ios.Information != cbBuf)
        SfLogRel("I/O control %#x: %#x, status %#x, information %u of %u\n", uCode, rcNt, Ios.Status,
                 Ios.Information, cbBuf);
#endif
    if (NT_SUCCESS(rcNt))
        rcNt = Ios.Status;
    if (!NT_SUCCESS(rcNt))
    {
        SfLogRel("I/O control %#x failed: %#x\n", uCode, rcNt);
        return VERR_GENERAL_FAILURE;
    }
    return VINF_SUCCESS;
}

/**
 * Opens VBoxGst and connects to VBoxSharedFolders, unless that's done.
 *
 * @returns VBox status code.
 * @param   fSystemContext  Called in the system's context (Init_Complete).
 */
int SfHostConnect(BOOL fSystemContext)
{
    VBOXGUEST98HGCMCONNECT Connect;
    WCHAR *pwszName = g_wszDevNameNt;
    NTSTATUS rcNt;
    int rc;

    if (g_idClient)
        return VINF_SUCCESS;

    if (!g_hDevice)
    {
        rcNt = sfOpenDeviceInSystem(pwszName, &g_hDevice, fSystemContext);
        if (!NT_SUCCESS(rcNt))
        {
            SfLogRel("opening \\Device\\VBoxGuest failed: %#x\n", rcNt);
            pwszName = g_wszDevNameDos;
            rcNt = sfOpenDeviceInSystem(pwszName, &g_hDevice, fSystemContext);
        }
        if (!NT_SUCCESS(rcNt))
        {
            SfLogRel("opening \\DosDevices\\VBoxGuest failed: %#x\n", rcNt);
            g_hDevice = NULL;
            return VERR_FILE_NOT_FOUND;
        }
        SfLogRel("opened %s\n", pwszName == g_wszDevNameNt ? "\\Device\\VBoxGuest" : "\\DosDevices\\VBoxGuest");
    }

    if (!g_Direct.pfnCall)
    {
        SFIOSTATUSBLOCK Ios;
        SfMemZero(&Ios, sizeof(Ios));
        rcNt = SfNtInternalDeviceIoControl(g_hDevice, NULL, NULL, NULL, &Ios, IOCTL_VBOXGUEST98_INTERNAL_GET_DIRECT_HGCM,
                                           NULL, 0, &g_Direct, sizeof(g_Direct));
        if (!NT_SUCCESS(rcNt) || !NT_SUCCESS(Ios.Status) || !g_Direct.pfnCall)
        {
            /* Older VBoxGst: everything works but running programs from a share. */
            SfLogRel("no direct HGCM calls (%#x, %#x)\n", rcNt, Ios.Status);
            g_Direct.pfnCall = NULL;
        }
    }

    SfMemZero(&Connect, sizeof(Connect));
    SfMemCopy(Connect.szServiceName, SHFL_SERVICE_NAME, sizeof(SHFL_SERVICE_NAME));
    rc = sfHostIoCtl(IOCTL_VBOXGUEST98_HGCM_CONNECT, &Connect, sizeof(Connect));
    if (rc >= 0)
        rc = Connect.rc;
    if (rc < 0)
    {
        SfLogRel("connecting to %s failed: %d\n", SHFL_SERVICE_NAME, rc);
        return rc;
    }
    g_idClient = Connect.u32ClientID;
    SfLogRel("connected to %s, client ID %u\n", SHFL_SERVICE_NAME, g_idClient);
    return VINF_SUCCESS;
}


/*********************************************************************************************************************************
*   HGCM call builder                                                                                                            *
*********************************************************************************************************************************/

/**
 * Starts a call with cParms parameters and room for cbData bytes of buffers.
 *
 * @returns VBox status code.
 */
static int sfMsgInit(SFHGCMMSG *pMsg, UINT32 uFunction, UINT32 cParms, ULONG cbData)
{
    ULONG offData = sizeof(VBOXGUEST98HGCMCALL) + cParms * sizeof(VBOXGUEST98HGCMPARM);

    pMsg->cbCall  = offData + cbData;
    pMsg->offNext = offData;
    pMsg->fDirect = FALSE;
    pMsg->pCall   = (VBOXGUEST98HGCMCALL *)SfHeapAllocate(pMsg->cbCall, HEAPZEROINIT);
    if (!pMsg->pCall)
        return VERR_NO_MEMORY;
    pMsg->pCall->u32ClientID = g_idClient;
    pMsg->pCall->u32Function = uFunction;
    pMsg->pCall->cMsTimeout  = VBOXGUEST98_WAIT_INFINITE;
    pMsg->pCall->cParms      = cParms;
    pMsg->pCall->rc          = VERR_INTERNAL_ERROR;    /* Until the driver says otherwise. */
    return VINF_SUCCESS;
}

static void sfMsgSetU32(SFHGCMMSG *pMsg, UINT32 iParm, UINT32 u32)
{
    VBOXGUEST98HGCMPARM *pParm = &VBOXGUEST98_HGCM_CALL_PARMS(pMsg->pCall)[iParm];
    pParm->enmType  = VBOXGUEST98_HGCM_PARM_32BIT;
    pParm->u64Value = u32;
}

/**
 * Adds a buffer parameter, copying pvIn into it if given.
 *
 * @returns The buffer inside the call.
 */
static void *sfMsgSetBuf(SFHGCMMSG *pMsg, UINT32 iParm, UINT32 enmType, ULONG cb, const void *pvIn)
{
    VBOXGUEST98HGCMPARM *pParm = &VBOXGUEST98_HGCM_CALL_PARMS(pMsg->pCall)[iParm];
    BYTE *pb = (BYTE *)pMsg->pCall + pMsg->offNext;

    pParm->enmType = enmType;
    pParm->cb      = cb;
    pParm->off     = pMsg->offNext;
    if (pvIn)
        SfMemCopy(pb, pvIn, cb);
    pMsg->offNext += (cb + 7) & ~7UL;
    return pb;
}

static void sfMsgSetU64(SFHGCMMSG *pMsg, UINT32 iParm, UINT64 u64)
{
    VBOXGUEST98HGCMPARM *pParm = &VBOXGUEST98_HGCM_CALL_PARMS(pMsg->pCall)[iParm];
    pParm->enmType  = VBOXGUEST98_HGCM_PARM_64BIT;
    pParm->u64Value = u64;
}

static UINT32 sfMsgGetU32(SFHGCMMSG *pMsg, UINT32 iParm)
{
    return (UINT32)VBOXGUEST98_HGCM_CALL_PARMS(pMsg->pCall)[iParm].u64Value;
}

/**
 * Makes the call.
 *
 * @returns VBox status code: the host's if the call got there.
 */
static int sfMsgCall(SFHGCMMSG *pMsg)
{
    int rc;
    if (!pMsg->fDirect)
        rc = sfHostIoCtl(IOCTL_VBOXGUEST98_HGCM_CALL, pMsg->pCall, pMsg->cbCall);
    else if (g_Direct.pfnCall)
        rc = g_Direct.pfnCall(g_Direct.pvSession, pMsg->pCall, pMsg->cbCall);
    else
        rc = VERR_NOT_SUPPORTED;
    return rc >= 0 ? pMsg->pCall->rc : rc;
}

static void sfMsgFree(SFHGCMMSG *pMsg)
{
    if (pMsg->pCall)
        SfHeapFree(pMsg->pCall, 0);
    pMsg->pCall = NULL;
}


/*********************************************************************************************************************************
*   Shared folders functions                                                                                                     *
*********************************************************************************************************************************/

/**
 * Logs a UTF-16 name, '?' for what isn't ASCII.
 */
static void sfLogName(const char *pszWhat, UINT32 u32, const WCHAR *pwsz, ULONG cwc)
{
    char szName[SHFL_MAX_LEN + 1];
    ULONG i;

    if (cwc > SHFL_MAX_LEN)
        cwc = SHFL_MAX_LEN;
    for (i = 0; i < cwc; i++)
        szName[i] = pwsz[i] >= 0x20 && pwsz[i] < 0x7f ? (char)pwsz[i] : '?';
    szName[i] = '\0';
    SfLogRel("%s %u: '%s'\n", pszWhat, u32, szName);
}

/**
 * Logs the host's shared folders (name and root of each).
 */
void SfHostLogMappings(void)
{
    SFHGCMMSG Msg;
    SHFLMAPPING *paMappings;
    UINT32 cMappings;
    UINT32 i;
    int rc;

    rc = sfMsgInit(&Msg, SHFL_FN_QUERY_MAPPINGS, SHFL_FN_QUERY_MAPPINGS_CPARMS, SHFL_MAX_MAPPINGS * sizeof(SHFLMAPPING));
    if (rc < 0)
        return;
    sfMsgSetU32(&Msg, 0, SHFL_MF_UCS2);
    sfMsgSetU32(&Msg, 1, SHFL_MAX_MAPPINGS);
    paMappings = (SHFLMAPPING *)sfMsgSetBuf(&Msg, 2, VBOXGUEST98_HGCM_PARM_BUFFER_OUT,
                                            SHFL_MAX_MAPPINGS * sizeof(SHFLMAPPING), NULL);
    rc = sfMsgCall(&Msg);
    cMappings = sfMsgGetU32(&Msg, 1);
    SfLogRel("SHFL_FN_QUERY_MAPPINGS: %d, %u mappings\n", rc, cMappings);
    if (rc >= 0)
    {
        if (cMappings > SHFL_MAX_MAPPINGS)
            cMappings = SHFL_MAX_MAPPINGS;
        for (i = 0; i < cMappings; i++)
        {
            SFHGCMMSG MsgName;
            SHFLSTRING *pName;

            if (sfMsgInit(&MsgName, SHFL_FN_QUERY_MAP_NAME, SHFL_FN_QUERY_MAP_NAME_CPARMS,
                          SHFLSTRING_HEADER_SIZE + (SHFL_MAX_LEN + 1) * sizeof(WCHAR)) < 0)
                break;
            sfMsgSetU32(&MsgName, 0, paMappings[i].root);
            pName = (SHFLSTRING *)sfMsgSetBuf(&MsgName, 1, VBOXGUEST98_HGCM_PARM_BUFFER_INOUT,
                                              SHFLSTRING_HEADER_SIZE + (SHFL_MAX_LEN + 1) * sizeof(WCHAR), NULL);
            pName->u16Size = (SHFL_MAX_LEN + 1) * sizeof(WCHAR);
            rc = sfMsgCall(&MsgName);
            if (rc >= 0)
                sfLogName("mapping, root", paMappings[i].root, pName->String.utf16, pName->u16Length / sizeof(WCHAR));
            else
                SfLogRel("SHFL_FN_QUERY_MAP_NAME %u: %d\n", paMappings[i].root, rc);
            sfMsgFree(&MsgName);
        }
    }
    sfMsgFree(&Msg);
}

/**
 * Gets the names of the host's shared folders, for the network provider's
 * view of \\VBOXSVR.
 *
 * @returns VBox status code.
 * @param   pwszBuf     Where to put the names: each one terminated, the list
 *                      ended by an empty one.  Names that don't fit are left
 *                      out.
 * @param   cwcBuf      Its size in characters (at least 1).
 * @param   pcwcRet     Where to return the characters used.
 */
int SfHostQueryNames(WCHAR *pwszBuf, ULONG cwcBuf, ULONG *pcwcRet)
{
    SFHGCMMSG Msg;
    SHFLMAPPING *paMappings;
    UINT32 cMappings;
    UINT32 i;
    ULONG off = 0;
    int rc;

    rc = sfMsgInit(&Msg, SHFL_FN_QUERY_MAPPINGS, SHFL_FN_QUERY_MAPPINGS_CPARMS, SHFL_MAX_MAPPINGS * sizeof(SHFLMAPPING));
    if (rc < 0)
        return rc;
    sfMsgSetU32(&Msg, 0, SHFL_MF_UCS2);
    sfMsgSetU32(&Msg, 1, SHFL_MAX_MAPPINGS);
    paMappings = (SHFLMAPPING *)sfMsgSetBuf(&Msg, 2, VBOXGUEST98_HGCM_PARM_BUFFER_OUT,
                                            SHFL_MAX_MAPPINGS * sizeof(SHFLMAPPING), NULL);
    rc = sfMsgCall(&Msg);
    cMappings = sfMsgGetU32(&Msg, 1);
    if (cMappings > SHFL_MAX_MAPPINGS)
        cMappings = SHFL_MAX_MAPPINGS;
    for (i = 0; rc >= 0 && i < cMappings; i++)
    {
        SFHGCMMSG MsgName;
        SHFLSTRING *pName;
        ULONG cwcName;

        rc = sfMsgInit(&MsgName, SHFL_FN_QUERY_MAP_NAME, SHFL_FN_QUERY_MAP_NAME_CPARMS,
                       SHFLSTRING_HEADER_SIZE + (SHFL_MAX_LEN + 1) * sizeof(WCHAR));
        if (rc < 0)
            break;
        sfMsgSetU32(&MsgName, 0, paMappings[i].root);
        pName = (SHFLSTRING *)sfMsgSetBuf(&MsgName, 1, VBOXGUEST98_HGCM_PARM_BUFFER_INOUT,
                                          SHFLSTRING_HEADER_SIZE + (SHFL_MAX_LEN + 1) * sizeof(WCHAR), NULL);
        pName->u16Size = (SHFL_MAX_LEN + 1) * sizeof(WCHAR);
        /* A folder removed meanwhile is just left out. */
        if (sfMsgCall(&MsgName) >= 0)
        {
            cwcName = pName->u16Length / sizeof(WCHAR);
            if (cwcName && cwcName <= SHFL_MAX_LEN && off + cwcName + 2 <= cwcBuf)
            {
                SfMemCopy(&pwszBuf[off], pName->String.utf16, cwcName * sizeof(WCHAR));
                off += cwcName;
                pwszBuf[off++] = 0;
            }
        }
        sfMsgFree(&MsgName);
    }
    sfMsgFree(&Msg);
    pwszBuf[off++] = 0;
    *pcwcRet = off;
    return rc;
}

/**
 * Maps a shared folder by name.
 *
 * @returns VBox status code.
 * @param   pwszName    The name (not terminated).
 * @param   cwcName     Its length in characters.
 * @param   pidRoot     Where to return the root.
 */
int SfHostMapFolder(const WCHAR *pwszName, ULONG cwcName, SHFLROOT *pidRoot)
{
    SFHGCMMSG Msg;
    SHFLSTRING *pName;
    ULONG cbName = (cwcName + 1) * sizeof(WCHAR);
    int rc;

    if (cwcName > SHFL_MAX_LEN)
        return VERR_INVALID_PARAMETER;
    rc = sfMsgInit(&Msg, SHFL_FN_MAP_FOLDER, SHFL_FN_MAP_FOLDER_CPARMS, SHFLSTRING_HEADER_SIZE + cbName);
    if (rc < 0)
        return rc;
    pName = (SHFLSTRING *)sfMsgSetBuf(&Msg, 0, VBOXGUEST98_HGCM_PARM_BUFFER_IN, SHFLSTRING_HEADER_SIZE + cbName, NULL);
    pName->u16Size   = (USHORT)cbName;
    pName->u16Length = (USHORT)(cbName - sizeof(WCHAR));
    SfMemCopy(pName->String.utf16, pwszName, cwcName * sizeof(WCHAR));
    sfMsgSetU32(&Msg, 1, 0);
    sfMsgSetU32(&Msg, 2, '\\');
    sfMsgSetU32(&Msg, 3, 0 /* case insensitive */);
    rc = sfMsgCall(&Msg);
    *pidRoot = sfMsgGetU32(&Msg, 1);
    if (rc < 0)
        SfLogRel("SHFL_FN_MAP_FOLDER: %d\n", rc);
    else
        sfLogName("mapped, root", *pidRoot, pwszName, cwcName);
    sfMsgFree(&Msg);
    return rc;
}

/**
 * Unmaps a shared folder.
 *
 * @returns VBox status code.
 */
int SfHostUnmapFolder(SHFLROOT idRoot)
{
    SFHGCMMSG Msg;
    int rc = sfMsgInit(&Msg, SHFL_FN_UNMAP_FOLDER, SHFL_FN_UNMAP_FOLDER_CPARMS, 0);
    if (rc < 0)
        return rc;
    sfMsgSetU32(&Msg, 0, idRoot);
    rc = sfMsgCall(&Msg);
    SfLogRel("unmapped root %u: %d\n", idRoot, rc);
    sfMsgFree(&Msg);
    return rc;
}

/** Size of a SHFLSTRING buffer, header included. */
#define SF_SHFLSTRING_SIZE(pStr)    (SHFLSTRING_HEADER_SIZE + (pStr)->u16Size)

/**
 * Opens, creates or looks up an object (SHFL_FN_CREATE).
 *
 * @returns VBox status code of the call; pParms->Result and Handle say what
 *          happened to the object.
 */
int SfHostCreate(SHFLROOT idRoot, const SHFLSTRING *pPath, SHFLCREATEPARMS *pParms)
{
    SFHGCMMSG Msg;
    SHFLCREATEPARMS *pParmsOut;
    int rc = sfMsgInit(&Msg, SHFL_FN_CREATE, SHFL_FN_CREATE_CPARMS,
                       ((SF_SHFLSTRING_SIZE(pPath) + 7) & ~7UL) + sizeof(*pParms));
    if (rc < 0)
        return rc;
    sfMsgSetU32(&Msg, 0, idRoot);
    sfMsgSetBuf(&Msg, 1, VBOXGUEST98_HGCM_PARM_BUFFER_IN, SF_SHFLSTRING_SIZE(pPath), pPath);
    pParmsOut = (SHFLCREATEPARMS *)sfMsgSetBuf(&Msg, 2, VBOXGUEST98_HGCM_PARM_BUFFER_INOUT, sizeof(*pParms), pParms);
    rc = sfMsgCall(&Msg);
    if (rc >= 0)
        SfMemCopy(pParms, pParmsOut, sizeof(*pParms));
    sfMsgFree(&Msg);
    return rc;
}

/**
 * Closes a host handle (SHFL_FN_CLOSE).
 */
int SfHostClose(SHFLROOT idRoot, SHFLHANDLE hHost)
{
    SFHGCMMSG Msg;
    int rc = sfMsgInit(&Msg, SHFL_FN_CLOSE, SHFL_FN_CLOSE_CPARMS, 0);
    if (rc < 0)
        return rc;
    sfMsgSetU32(&Msg, 0, idRoot);
    sfMsgSetU64(&Msg, 1, hHost);
    rc = sfMsgCall(&Msg);
    sfMsgFree(&Msg);
    return rc;
}

/**
 * Reads up to SF_MAX_IO_CHUNK bytes (SHFL_FN_READ).
 *
 * @param   pcb     IN: bytes to read, at most SF_MAX_IO_CHUNK; OUT: bytes read.
 * @param   fPaging Paging I/O (see SFHGCMMSG::fDirect).
 */
int SfHostRead(SHFLROOT idRoot, SHFLHANDLE hHost, UINT64 off, void *pvBuf, ULONG *pcb, BOOL fPaging)
{
    SFHGCMMSG Msg;
    void *pvData;
    ULONG cbRead;
    int rc = sfMsgInit(&Msg, SHFL_FN_READ, SHFL_FN_READ_CPARMS, *pcb);
    if (rc < 0)
        return rc;
    Msg.fDirect = fPaging;
    sfMsgSetU32(&Msg, 0, idRoot);
    sfMsgSetU64(&Msg, 1, hHost);
    sfMsgSetU64(&Msg, 2, off);
    sfMsgSetU32(&Msg, 3, *pcb);
    pvData = sfMsgSetBuf(&Msg, 4, VBOXGUEST98_HGCM_PARM_BUFFER_OUT, *pcb, NULL);
    rc = sfMsgCall(&Msg);
    cbRead = 0;
    if (rc >= 0)
    {
        cbRead = sfMsgGetU32(&Msg, 3);
        if (cbRead > *pcb)
            cbRead = *pcb;
        SfMemCopy(pvBuf, pvData, cbRead);
    }
    *pcb = cbRead;
    sfMsgFree(&Msg);
    return rc;
}

/**
 * Reads directory entries (SHFL_FN_LIST).
 *
 * @returns VBox status code, VERR_NO_MORE_FILES at the end.
 * @param   pFilter     The full path with wildcards for the first call, or
 *                      NULL to list all of the directory hHost is open on.
 * @param   pcbBuf      IN: buffer size, at most SF_MAX_IO_CHUNK; OUT: bytes used.
 * @param   pcEntries   Where to return the number of SHFLDIRINFO entries.
 */
int SfHostList(SHFLROOT idRoot, SHFLHANDLE hHost, UINT32 fFlags, const SHFLSTRING *pFilter,
               void *pvBuf, ULONG *pcbBuf, UINT32 *pcEntries)
{
    SFHGCMMSG Msg;
    void *pvData;
    ULONG cbFilter = pFilter ? SF_SHFLSTRING_SIZE(pFilter) : 0;
    ULONG cbUsed;
    int rc = sfMsgInit(&Msg, SHFL_FN_LIST, SHFL_FN_LIST_CPARMS, ((cbFilter + 7) & ~7UL) + *pcbBuf);
    if (rc < 0)
        return rc;
    sfMsgSetU32(&Msg, 0, idRoot);
    sfMsgSetU64(&Msg, 1, hHost);
    sfMsgSetU32(&Msg, 2, fFlags);
    sfMsgSetU32(&Msg, 3, *pcbBuf);
    sfMsgSetBuf(&Msg, 4, VBOXGUEST98_HGCM_PARM_BUFFER_IN, cbFilter, pFilter);
    pvData = sfMsgSetBuf(&Msg, 5, VBOXGUEST98_HGCM_PARM_BUFFER_OUT, *pcbBuf, NULL);
    sfMsgSetU32(&Msg, 6, 0);
    sfMsgSetU32(&Msg, 7, 0);
    rc = sfMsgCall(&Msg);
    cbUsed = 0;
    *pcEntries = 0;
    if (rc >= 0)
    {
        cbUsed = sfMsgGetU32(&Msg, 3);
        if (cbUsed > *pcbBuf)
            cbUsed = *pcbBuf;
        *pcEntries = sfMsgGetU32(&Msg, 7);
        SfMemCopy(pvBuf, pvData, cbUsed);
    }
    *pcbBuf = cbUsed;
    sfMsgFree(&Msg);
    return rc;
}

/**
 * Gets or sets object or volume information (SHFL_FN_INFORMATION).
 *
 * @param   pcbInfo     IN: size of pvInfo; OUT: bytes the host used.
 */
int SfHostInformation(SHFLROOT idRoot, SHFLHANDLE hHost, UINT32 fFlags, void *pvInfo, ULONG *pcbInfo)
{
    SFHGCMMSG Msg;
    void *pvData;
    ULONG cbUsed;
    int rc = sfMsgInit(&Msg, SHFL_FN_INFORMATION, SHFL_FN_INFORMATION_CPARMS, *pcbInfo);
    if (rc < 0)
        return rc;
    sfMsgSetU32(&Msg, 0, idRoot);
    sfMsgSetU64(&Msg, 1, hHost);
    sfMsgSetU32(&Msg, 2, fFlags);
    sfMsgSetU32(&Msg, 3, *pcbInfo);
    pvData = sfMsgSetBuf(&Msg, 4, VBOXGUEST98_HGCM_PARM_BUFFER_INOUT, *pcbInfo, pvInfo);
    rc = sfMsgCall(&Msg);
    cbUsed = 0;
    if (rc >= 0)
    {
        cbUsed = sfMsgGetU32(&Msg, 3);
        if (cbUsed > *pcbInfo)
            cbUsed = *pcbInfo;
        SfMemCopy(pvInfo, pvData, cbUsed);
    }
    *pcbInfo = cbUsed;
    sfMsgFree(&Msg);
    return rc;
}

/**
 * Writes up to SF_MAX_IO_CHUNK bytes (SHFL_FN_WRITE).
 *
 * @param   pcb     IN: bytes to write, at most SF_MAX_IO_CHUNK; OUT: bytes written.
 * @param   fPaging Paging I/O (see SFHGCMMSG::fDirect).
 */
int SfHostWrite(SHFLROOT idRoot, SHFLHANDLE hHost, UINT64 off, const void *pvBuf, ULONG *pcb, BOOL fPaging)
{
    SFHGCMMSG Msg;
    ULONG cbWritten;
    int rc = sfMsgInit(&Msg, SHFL_FN_WRITE, SHFL_FN_WRITE_CPARMS, *pcb);
    if (rc < 0)
        return rc;
    Msg.fDirect = fPaging;
    sfMsgSetU32(&Msg, 0, idRoot);
    sfMsgSetU64(&Msg, 1, hHost);
    sfMsgSetU64(&Msg, 2, off);
    sfMsgSetU32(&Msg, 3, *pcb);
    sfMsgSetBuf(&Msg, 4, VBOXGUEST98_HGCM_PARM_BUFFER_IN, *pcb, pvBuf);
    rc = sfMsgCall(&Msg);
    cbWritten = rc >= 0 ? sfMsgGetU32(&Msg, 3) : 0;
    *pcb = cbWritten < *pcb ? cbWritten : *pcb;
    sfMsgFree(&Msg);
    return rc;
}

/**
 * Truncates or extends a file (SHFL_FN_SET_FILE_SIZE).
 */
int SfHostSetFileSize(SHFLROOT idRoot, SHFLHANDLE hHost, UINT64 cbNew)
{
    SFHGCMMSG Msg;
    int rc = sfMsgInit(&Msg, SHFL_FN_SET_FILE_SIZE, SHFL_FN_SET_FILE_SIZE_CPARMS, 0);
    if (rc < 0)
        return rc;
    sfMsgSetU32(&Msg, 0, idRoot);
    sfMsgSetU64(&Msg, 1, hHost);
    sfMsgSetU64(&Msg, 2, cbNew);
    rc = sfMsgCall(&Msg);
    sfMsgFree(&Msg);
    return rc;
}

/**
 * Flushes a file (SHFL_FN_FLUSH).
 */
int SfHostFlush(SHFLROOT idRoot, SHFLHANDLE hHost)
{
    SFHGCMMSG Msg;
    int rc = sfMsgInit(&Msg, SHFL_FN_FLUSH, SHFL_FN_FLUSH_CPARMS, 0);
    if (rc < 0)
        return rc;
    sfMsgSetU32(&Msg, 0, idRoot);
    sfMsgSetU64(&Msg, 1, hHost);
    rc = sfMsgCall(&Msg);
    sfMsgFree(&Msg);
    return rc;
}

/**
 * Removes a file or an empty directory (SHFL_FN_REMOVE).
 *
 * @param   fFlags  SHFL_REMOVE_FILE or SHFL_REMOVE_DIR.
 */
int SfHostRemove(SHFLROOT idRoot, const SHFLSTRING *pPath, UINT32 fFlags)
{
    SFHGCMMSG Msg;
    int rc = sfMsgInit(&Msg, SHFL_FN_REMOVE, SHFL_FN_REMOVE_CPARMS, SF_SHFLSTRING_SIZE(pPath));
    if (rc < 0)
        return rc;
    sfMsgSetU32(&Msg, 0, idRoot);
    sfMsgSetBuf(&Msg, 1, VBOXGUEST98_HGCM_PARM_BUFFER_IN, SF_SHFLSTRING_SIZE(pPath), pPath);
    sfMsgSetU32(&Msg, 2, fFlags);
    rc = sfMsgCall(&Msg);
    sfMsgFree(&Msg);
    return rc;
}

/**
 * Locks or unlocks a byte range of an open file (SHFL_FN_LOCK).
 *
 * @param   fFlags  SHFL_LOCK_EXCLUSIVE, SHFL_LOCK_SHARED or SHFL_LOCK_CANCEL.
 */
int SfHostLock(SHFLROOT idRoot, SHFLHANDLE hHost, UINT64 off, UINT64 cb, UINT32 fFlags)
{
    SFHGCMMSG Msg;
    int rc = sfMsgInit(&Msg, SHFL_FN_LOCK, SHFL_FN_LOCK_CPARMS, 0);
    if (rc < 0)
        return rc;
    sfMsgSetU32(&Msg, 0, idRoot);
    sfMsgSetU64(&Msg, 1, hHost);
    sfMsgSetU64(&Msg, 2, off);
    sfMsgSetU64(&Msg, 3, cb);
    sfMsgSetU32(&Msg, 4, fFlags);
    rc = sfMsgCall(&Msg);
    sfMsgFree(&Msg);
    return rc;
}

/**
 * Renames a file or directory (SHFL_FN_RENAME).
 *
 * @param   fFlags  SHFL_RENAME_FILE or SHFL_RENAME_DIR, maybe with
 *                  SHFL_RENAME_REPLACE_IF_EXISTS.
 */
int SfHostRename(SHFLROOT idRoot, const SHFLSTRING *pSrc, const SHFLSTRING *pDst, UINT32 fFlags)
{
    SFHGCMMSG Msg;
    int rc = sfMsgInit(&Msg, SHFL_FN_RENAME, SHFL_FN_RENAME_CPARMS,
                       ((SF_SHFLSTRING_SIZE(pSrc) + 7) & ~7UL) + SF_SHFLSTRING_SIZE(pDst));
    if (rc < 0)
        return rc;
    sfMsgSetU32(&Msg, 0, idRoot);
    sfMsgSetBuf(&Msg, 1, VBOXGUEST98_HGCM_PARM_BUFFER_IN, SF_SHFLSTRING_SIZE(pSrc), pSrc);
    sfMsgSetBuf(&Msg, 2, VBOXGUEST98_HGCM_PARM_BUFFER_IN, SF_SHFLSTRING_SIZE(pDst), pDst);
    sfMsgSetU32(&Msg, 3, fFlags);
    rc = sfMsgCall(&Msg);
    sfMsgFree(&Msg);
    return rc;
}
