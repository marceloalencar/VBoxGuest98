/** @file
 * VBoxSF - Initialization, the Win32 interface and release logging.
 *
 * Logging writes byte by byte to RTLOG_DEBUG_PORT, so the host sees it in
 * VBox.log.  The formatter is the one of VBoxGst (Log.c).
 */
#include "VBoxSF.h"

#pragma VxD_LOCKED_CODE_SEG
#pragma VxD_LOCKED_DATA_SEG


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/

/** Longest line SfLogRel produces, prefix and newline included. */
#define SF_LOG_LINE_MAX         256


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/

/** Output buffer of the formatter. */
typedef struct SFLOGBUF
{
    ULONG off;
    char  ach[SF_LOG_LINE_MAX];
} SFLOGBUF;


/*********************************************************************************************************************************
*   Global Variables                                                                                                             *
*********************************************************************************************************************************/

/** Provider ID IFSMgr gave us, -1 if the registration failed. */
static int g_idProvider = -1;
/** Serializes the resource and search tables. */
PVOID g_hSfMutex;


/*********************************************************************************************************************************
*   Memory helpers (there is no C runtime in a VxD)                                                                              *
*********************************************************************************************************************************/

void SfMemZero(void *pv, ULONG cb)
{
    char *pb = (char *)pv;
    while (cb--)
        *pb++ = 0;
}

void SfMemCopy(void *pvDst, const void *pvSrc, ULONG cb)
{
    char *pbDst = (char *)pvDst;
    const char *pbSrc = (const char *)pvSrc;
    while (cb--)
        *pbDst++ = *pbSrc++;
}


/*********************************************************************************************************************************
*   64-bit arithmetic and time conversion (no __aulldiv or __allshl without the C runtime)                                     *
*********************************************************************************************************************************/

/** The halves of a 64-bit value (shifts would need the run time without
 *  optimization). */
typedef union SFU64
{
    UINT64 u64;
    struct
    {
        UINT32 uLo;
        UINT32 uHi;
    } s;
} SFU64;

UINT64 SfU64DivU32(UINT64 u64, UINT32 uDivisor)
{
    SFU64 Dividend, Quotient;
    UINT32 uHi, uLo, uQuotHi, uQuotLo;

    Dividend.u64 = u64;
    uHi = Dividend.s.uHi;
    uLo = Dividend.s.uLo;
    __asm
    {
        mov     ecx, [uDivisor]
        mov     eax, [uHi]
        xor     edx, edx
        div     ecx
        mov     [uQuotHi], eax
        mov     eax, [uLo]
        div     ecx
        mov     [uQuotLo], eax
    }
    Quotient.s.uHi = uQuotHi;
    Quotient.s.uLo = uQuotLo;
    return Quotient.u64;
}

UINT64 SfU64MulU32(UINT64 u64, UINT32 uFactor)
{
    SFU64 Value, Product;
    UINT32 uHi, uLo, uProdHi, uProdLo;

    Value.u64 = u64;
    uHi = Value.s.uHi;
    uLo = Value.s.uLo;
    __asm
    {
        mov     eax, [uLo]
        mul     dword ptr [uFactor]
        mov     [uProdLo], eax
        mov     ecx, edx
        mov     eax, [uHi]
        mul     dword ptr [uFactor]
        add     eax, ecx
        mov     [uProdHi], eax
    }
    Product.s.uHi = uProdHi;
    Product.s.uLo = uProdLo;
    return Product.u64;
}

/**
 * Converts an RTTIMESPEC (nanoseconds since 1970, UTC) to a Win32 FILETIME
 * (100 ns units since 1601, UTC).
 */
void SfTimeToFileTime(INT64 nsSince1970, _FILETIME *pFileTime)
{
    SFU64 Time;
    Time.u64 = 0;
    if (nsSince1970 > 0)
        Time.u64 = SfU64DivU32((UINT64)nsSince1970, 100);
    Time.u64 += 116444736000000000i64;
    pFileTime->dwLowDateTime  = Time.s.uLo;
    pFileTime->dwHighDateTime = Time.s.uHi;
}

/**
 * Converts an RTTIMESPEC to a DOS date and time (local time, IFSMgr does it).
 */
dos_time SfTimeToDosTime(INT64 nsSince1970)
{
    _FILETIME FileTime;
    SfTimeToFileTime(nsSince1970, &FileTime);
    return SfIfsWin32ToDosTime(FileTime);
}

/**
 * Converts a DOS date and time (local time) to an RTTIMESPEC; 0 (which the
 * host takes as "leave it") for dates before 1970.
 */
INT64 SfDosTimeToTime(dos_time DosTime)
{
    _FILETIME FileTime = SfIfsDosToWin32Time(DosTime);
    SFU64 Time;

    Time.s.uLo = FileTime.dwLowDateTime;
    Time.s.uHi = FileTime.dwHighDateTime;
    if (Time.u64 <= 116444736000000000i64)
        return 0;
    return (INT64)SfU64MulU32(Time.u64 - 116444736000000000i64, 100);
}

/**
 * Converts a VBox status code to the Win32 error code for ir_error.
 */
int SfRcToWin32(int rc)
{
    switch (rc)
    {
        case VERR_FILE_NOT_FOUND:       return SF_ERROR_FILE_NOT_FOUND;
        case VERR_PATH_NOT_FOUND:
        case VERR_NOT_A_DIRECTORY:      return SF_ERROR_PATH_NOT_FOUND;
        case VERR_TOO_MANY_OPEN_FILES:  return SF_ERROR_TOO_MANY_OPEN_FILES;
        case VERR_ACCESS_DENIED:
        case VERR_PERMISSION_DENIED:
        case VERR_IS_A_DIRECTORY:       return SF_ERROR_ACCESS_DENIED;
        case VERR_INVALID_HANDLE:       return SF_ERROR_INVALID_HANDLE;
        case VERR_NO_MEMORY:            return SF_ERROR_NOT_ENOUGH_MEMORY;
        case VERR_WRITE_PROTECT:        return SF_ERROR_WRITE_PROTECT;
        case VERR_NO_MORE_FILES:        return SF_ERROR_NO_MORE_FILES;
        case VERR_SHARING_VIOLATION:    return SF_ERROR_SHARING_VIOLATION;
        case VERR_FILE_LOCK_VIOLATION:  return SF_ERROR_LOCK_VIOLATION;
        case VERR_ALREADY_EXISTS:       return SF_ERROR_ALREADY_EXISTS;
        case VERR_INVALID_NAME:
        case VERR_FILENAME_TOO_LONG:    return SF_ERROR_INVALID_NAME;
        case VERR_DIR_NOT_EMPTY:        return SF_ERROR_DIR_NOT_EMPTY;
        case VERR_DISK_FULL:            return SF_ERROR_DISK_FULL;
        case VERR_INVALID_PARAMETER:    return SF_ERROR_INVALID_PARAMETER;
        default:                        return SF_ERROR_UNEXP_NET_ERR;
    }
}


/*********************************************************************************************************************************
*   Release logging                                                                                                              *
*********************************************************************************************************************************/

static void sfLogWrite(const char *pch, ULONG cch)
{
    if (!cch)
        return;
    __asm
    {
        pushfd
        cli
        mov     esi, [pch]
        mov     ecx, [cch]
        mov     edx, RTLOG_DEBUG_PORT
        cld
        rep outsb
        popfd
    }
}

static void sfLogPutC(SFLOGBUF *pBuf, char ch)
{
    if (pBuf->off < sizeof(pBuf->ach))
        pBuf->ach[pBuf->off++] = ch;
}

static void sfLogPutS(SFLOGBUF *pBuf, const char *psz)
{
    while (*psz)
        sfLogPutC(pBuf, *psz++);
}

/**
 * Formats into the buffer.  Supports %s, %c, %d, %u, %x, %X and %p with the
 * '#' (0x prefix) and '0' flags, a field width and an ignored 'l' modifier.
 */
static void sfLogFormatV(SFLOGBUF *pBuf, const char *pszFormat, va_list va)
{
    static const char s_achLower[] = "0123456789abcdef";
    static const char s_achUpper[] = "0123456789ABCDEF";

    while (*pszFormat)
    {
        const char *pachDigits = s_achLower;
        const char *psz;
        char achNum[12];
        ULONG cchNum = 0;
        ULONG cchWidth = 0;
        ULONG cchPrefix = 0;
        BOOLEAN fAlt = FALSE;
        BOOLEAN fZero = FALSE;
        BOOLEAN fNegative = FALSE;
        unsigned uBase = 10;
        UINT32 u32;
        char ch = *pszFormat++;

        if (ch != '%')
        {
            sfLogPutC(pBuf, ch);
            continue;
        }

        if (*pszFormat == '#')
        {
            fAlt = TRUE;
            pszFormat++;
        }
        if (*pszFormat == '0')
        {
            fZero = TRUE;
            pszFormat++;
        }
        while (*pszFormat >= '0' && *pszFormat <= '9')
            cchWidth = cchWidth * 10 + (ULONG)(*pszFormat++ - '0');
        if (*pszFormat == 'l')
            pszFormat++;
        ch = *pszFormat;
        if (!ch)
            break;
        pszFormat++;

        switch (ch)
        {
            case 's':
                psz = va_arg(va, const char *);
                if (!psz)
                    psz = "<NULL>";
                for (cchNum = 0; psz[cchNum]; cchNum++)
                    ;
                for (; cchWidth > cchNum; cchWidth--)
                    sfLogPutC(pBuf, ' ');
                sfLogPutS(pBuf, psz);
                continue;

            case 'c':
                sfLogPutC(pBuf, (char)va_arg(va, int));
                continue;

            case 'd':
            {
                INT32 i32 = va_arg(va, INT32);
                fNegative = i32 < 0;
                u32 = fNegative ? 0U - (UINT32)i32 : (UINT32)i32;
                break;
            }

            case 'u':
                u32 = va_arg(va, UINT32);
                break;

            case 'p':
                fZero = TRUE;
                cchWidth = 8;
                /* fall thru */
            case 'X':
                if (ch != 'p')
                    pachDigits = s_achUpper;
                /* fall thru */
            case 'x':
                u32 = va_arg(va, UINT32);
                uBase = 16;
                break;

            default: /* '%' and anything unknown. */
                sfLogPutC(pBuf, ch);
                continue;
        }

        do
        {
            achNum[cchNum++] = pachDigits[u32 % uBase];
            u32 /= uBase;
        } while (u32);

        if (fNegative)
            cchPrefix = 1;
        else if (fAlt && uBase == 16)
            cchPrefix = 2;
        if (!fZero)
            for (; cchWidth > cchNum + cchPrefix; cchWidth--)
                sfLogPutC(pBuf, ' ');
        if (fNegative)
            sfLogPutC(pBuf, '-');
        else if (cchPrefix)
            sfLogPutS(pBuf, "0x");
        for (; cchWidth > cchNum + cchPrefix; cchWidth--)
            sfLogPutC(pBuf, '0');
        while (cchNum)
            sfLogPutC(pBuf, achNum[--cchNum]);
    }
}

/**
 * Writes a formatted line, prefixed with "VBoxSF: ", to the release log.
 * The format should end with a newline.
 */
void __cdecl SfLogRel(const char *pszFormat, ...)
{
    SFLOGBUF Buf;
    va_list va;

    Buf.off = 0;
    sfLogPutS(&Buf, "VBoxSF: ");
    va_start(va, pszFormat);
    sfLogFormatV(&Buf, pszFormat, va);
    va_end(va);
    if (Buf.off == sizeof(Buf.ach))
        Buf.ach[sizeof(Buf.ach) - 1] = '\n'; /* Truncated, still end the line. */
    sfLogWrite(Buf.ach, Buf.off);
}


/*********************************************************************************************************************************
*   Control messages                                                                                                             *
*********************************************************************************************************************************/

/**
 * Device_Init: registers the network FSD.  VBoxGst may not be running yet
 * (it starts with the PnP devices, sometimes after the desktop), so the host
 * connection waits for the first use.
 *
 * @returns 0 (VBoxSF stays loaded even if the registration fails, for the log).
 */
int __cdecl VBoxSFDeviceInit(void)
{
    int uIfsVersion = SfIfsGetVersion();

    g_hSfMutex = SfCreateMutex(0, 0);
    if (!g_hSfMutex)
    {
        SfLogRel("no mutex, not registering\n");
        return 0;
    }
    g_idProvider = SfIfsRegisterNet(SfNetConnect, VBOXSF_IFSMGR_VERSION, VBOXSF_NET_ID);
    SfLogRel("%s (VirtualBox %s r%u) loaded, IFSMgr version %#x, provider ID %d\n",
             VBOXGUEST98_VERSION_STRING, VBOX_VERSION_STRING, VBOX_SVN_REV, uIfsVersion, g_idProvider);
    return 0;
}

/**
 * Init_Complete: tries the host connection once, for the log.
 */
void __cdecl VBoxSFInitComplete(void)
{
    int rc = SfHostConnect(TRUE);
    if (rc >= 0)
        SfHostLogMappings();
    else
        SfLogRel("Init_Complete: no host connection yet (%d)\n", rc);
}

/**
 * W32_DEVICEIOCONTROL.
 *
 * @returns 0 or a Win32 error code.
 * @param   uCode   The control code (DIOC_OPEN and DIOC_CLOSEHANDLE included).
 * @param   pParms  The DeviceIoControl parameters.
 */
ULONG __cdecl VBoxSFW32IoCtl(ULONG uCode, DIOCPARAMETERS *pParms)
{
    switch (uCode)
    {
        case DIOC_OPEN:
        case DIOC_CLOSEHANDLE:
            return 0;

        case VBOXSF_IOCTL_LOG_MAPPINGS:
        {
            int rc = SfHostConnect(FALSE);
            if (rc < 0)
            {
                SfLogRel("no host connection (%d)\n", rc);
                return SF_ERROR_UNEXP_NET_ERR;
            }
            SfHostLogMappings();
            return 0;
        }

        case VBOXSF_IOCTL_QUERY_NAMES:
        {
            ULONG cwcRet = 0;
            int rc;

            if (!pParms->lpvOutBuffer || pParms->cbOutBuffer < sizeof(WCHAR))
                return SF_ERROR_INVALID_PARAMETER;
            rc = SfHostConnect(FALSE);
            if (rc >= 0)
                rc = SfHostQueryNames((WCHAR *)pParms->lpvOutBuffer, pParms->cbOutBuffer / sizeof(WCHAR), &cwcRet);
            if (rc < 0)
                return SF_ERROR_UNEXP_NET_ERR;
            if (pParms->lpcbBytesReturned)
                *(DWORD *)pParms->lpcbBytesReturned = cwcRet * sizeof(WCHAR);
            return 0;
        }

        default:
            return SF_ERROR_NOT_SUPPORTED;
    }
}
