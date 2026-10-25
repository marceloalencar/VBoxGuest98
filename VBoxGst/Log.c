/** @file
 * VBoxGst - Release logging through the VMMDev backdoor port.
 *
 * Writes go byte by byte to RTLOG_DEBUG_PORT, so the host sees them in
 * VBox.log.  A spinlock keeps lines of different threads apart.  There is no
 * vsprintf among the NTKERN exports, hence the small formatter below.
 */

#include "VBoxGst.h"
#include <stdarg.h>


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/

/** Longest line VGDrvLogRel produces, prefix and newline included. */
#define VGDRV_LOG_LINE_MAX      256


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/

/** Output buffer of the formatter. */
typedef struct VGDRVLOGBUF
{
    ULONG off;
    char  ach[VGDRV_LOG_LINE_MAX];
} VGDRVLOGBUF;


/*********************************************************************************************************************************
*   Global Variables                                                                                                             *
*********************************************************************************************************************************/

/** Serializes writes to the log port. */
static KSPIN_LOCK g_LogSpinLock;


/**
 * Initializes logging, called first thing in DriverEntry.
 */
void VGDrvLogInit(void)
{
    KeInitializeSpinLock(&g_LogSpinLock);
}

/**
 * Writes raw bytes to the log port.  IRQL <= DISPATCH_LEVEL.
 *
 * @param   pch     The text (nonpaged).
 * @param   cch     Its length.
 */
void VGDrvLogWrite(const char *pch, ULONG cch)
{
    KIRQL OldIrql;

    if (!cch)
        return;

    KeAcquireSpinLock(&g_LogSpinLock, &OldIrql);
    __asm
    {
        mov     esi, [pch]
        mov     ecx, [cch]
        mov     edx, RTLOG_DEBUG_PORT
        cld
        rep outsb
    }
    KeReleaseSpinLock(&g_LogSpinLock, OldIrql);
}

static void vgdrvLogPutC(VGDRVLOGBUF *pBuf, char ch)
{
    if (pBuf->off < sizeof(pBuf->ach))
        pBuf->ach[pBuf->off++] = ch;
}

static void vgdrvLogPutS(VGDRVLOGBUF *pBuf, const char *psz)
{
    while (*psz)
        vgdrvLogPutC(pBuf, *psz++);
}

/**
 * Formats into the buffer.  Supports %s, %c, %d, %u, %x, %X and %p with the
 * '#' (0x prefix) and '0' flags, a field width and an ignored 'l' modifier.
 */
static void vgdrvLogFormatV(VGDRVLOGBUF *pBuf, const char *pszFormat, va_list va)
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
            vgdrvLogPutC(pBuf, ch);
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
                    vgdrvLogPutC(pBuf, ' ');
                vgdrvLogPutS(pBuf, psz);
                continue;

            case 'c':
                vgdrvLogPutC(pBuf, (char)va_arg(va, int));
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
                vgdrvLogPutC(pBuf, ch);
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
                vgdrvLogPutC(pBuf, ' ');
        if (fNegative)
            vgdrvLogPutC(pBuf, '-');
        else if (cchPrefix)
            vgdrvLogPutS(pBuf, "0x");
        for (; cchWidth > cchNum + cchPrefix; cchWidth--)
            vgdrvLogPutC(pBuf, '0');
        while (cchNum)
            vgdrvLogPutC(pBuf, achNum[--cchNum]);
    }
}

/**
 * Writes a formatted line, prefixed with "VBoxGst: ", to the release log.
 * The format should end with a newline.  IRQL <= DISPATCH_LEVEL.
 */
void __cdecl VGDrvLogRel(const char *pszFormat, ...)
{
    VGDRVLOGBUF Buf;
    va_list va;

    Buf.off = 0;
    vgdrvLogPutS(&Buf, "VBoxGst: ");
    va_start(va, pszFormat);
    vgdrvLogFormatV(&Buf, pszFormat, va);
    va_end(va);
    if (Buf.off == sizeof(Buf.ach))
        Buf.ach[sizeof(Buf.ach) - 1] = '\n'; /* Truncated, still end the line. */

    VGDrvLogWrite(Buf.ach, Buf.off);
}
