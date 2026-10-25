/** @file
 * VBGTest - Checks that the VBoxGst driver is installed and talks to the host.
 *
 * Exercises the I/O control interface from VBoxGuest98.h:
 *  1. Locate and open the device through its device interface.
 *  2. Query driver and host versions.
 *  3. Perform a raw VMMDev request (host time).
 *  4. Check that a driver managed request is refused.
 *  5. Wait for an event with a short timeout.
 *  6. Read a guest property through HGCM (VBoxGuestPropSvc).
 */

#include <windows.h>
#include <winioctl.h>
#include <initguid.h>
#include <setupapi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "VBoxGuest98.h"


/** Number of failed tests. */
static int g_cErrors = 0;

static void testFailed(const char *pszTest, const char *pszWhy)
{
    printf("FAILED: %s: %s (GetLastError=%lu)\n", pszTest, pszWhy, GetLastError());
    g_cErrors++;
}

/**
 * Opens the VBoxGuest device through its device interface.
 *
 * @returns Device handle, INVALID_HANDLE_VALUE on failure.
 */
static HANDLE openDriver(void)
{
    HANDLE hDevice = INVALID_HANDLE_VALUE;
    SP_INTERFACE_DEVICE_DATA ifdata;
    PSP_INTERFACE_DEVICE_DETAIL_DATA ifDetail;
    HDEVINFO info;
    DWORD ReqLen = 0;

    /* Get handle to relevant device information set. */
    info = SetupDiGetClassDevs(&GUID_DEVINTERFACE_VBOXGUEST98, NULL, NULL, DIGCF_PRESENT | DIGCF_INTERFACEDEVICE);
    if (info == INVALID_HANDLE_VALUE)
    {
        printf("No HDEVINFO available for this GUID\n");
        return INVALID_HANDLE_VALUE;
    }

    /* Get interface data for the requested instance. */
    ifdata.cbSize = sizeof(ifdata);
    if (!SetupDiEnumDeviceInterfaces(info, NULL, &GUID_DEVINTERFACE_VBOXGUEST98, 0, &ifdata))
    {
        printf("No SP_INTERFACE_DEVICE_DATA available for this GUID instance\n");
        SetupDiDestroyDeviceInfoList(info);
        return INVALID_HANDLE_VALUE;
    }

    /* Get size of symbolic link name. */
    SetupDiGetDeviceInterfaceDetail(info, &ifdata, NULL, 0, &ReqLen, NULL);
    ifDetail = (PSP_INTERFACE_DEVICE_DETAIL_DATA)malloc(ReqLen);
    if (ifDetail == NULL)
    {
        SetupDiDestroyDeviceInfoList(info);
        return INVALID_HANDLE_VALUE;
    }

    /* Get symbolic link name. */
    ifDetail->cbSize = sizeof(SP_INTERFACE_DEVICE_DETAIL_DATA);
    if (SetupDiGetDeviceInterfaceDetail(info, &ifdata, ifDetail, ReqLen, NULL, NULL))
    {
        printf("Symbolic link is %s\n", ifDetail->DevicePath);
        hDevice = CreateFile(ifDetail->DevicePath,
                             GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE,
                             NULL,
                             OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL,
                             NULL);
        if (hDevice == INVALID_HANDLE_VALUE)
            printf("CreateFile did not work. GetLastError() 0x%lx\n", GetLastError());
    }

    free(ifDetail);
    SetupDiDestroyDeviceInfoList(info);
    return hDevice;
}

/**
 * Wrapper around DeviceIoControl using the same buffer for input and output.
 */
static BOOL doIoCtl(HANDLE hDevice, DWORD uCmd, void *pvBuf, DWORD cbIn, DWORD cbOut)
{
    DWORD cbReturned = 0;
    return DeviceIoControl(hDevice, uCmd, pvBuf, cbIn, pvBuf, cbOut, &cbReturned, NULL);
}

static void initRequestHeader(VMMDevRequestHeader *pHdr, UINT32 cb, VMMDevRequestType enmType)
{
    memset(pHdr, 0, cb);
    pHdr->size        = cb;
    pHdr->version     = VMMDEV_REQUEST_HEADER_VERSION;
    pHdr->requestType = enmType;
    pHdr->rc          = VERR_GENERAL_FAILURE;
}

static void testVersionInfo(HANDLE hDevice)
{
    VBOXGUEST98VERSIONINFO Info;

    memset(&Info, 0, sizeof(Info));
    if (!doIoCtl(hDevice, IOCTL_VBOXGUEST98_GET_VERSION_INFO, &Info, 0, sizeof(Info)))
    {
        testFailed("GET_VERSION_INFO", "DeviceIoControl failed");
        return;
    }
    printf("Interface version: %lx\n", (unsigned long)Info.u32InterfaceVersion);
    printf("Driver version:    %u.%u.%lur%lu\n", Info.uDriverMajor, Info.uDriverMinor,
           (unsigned long)Info.uDriverBuild, (unsigned long)Info.uDriverRevision);
    printf("Host version:      %u.%u.%lur%lu features %lx\n", Info.uHostMajor, Info.uHostMinor,
           (unsigned long)Info.uHostBuild, (unsigned long)Info.uHostRevision, (unsigned long)Info.fHostFeatures);
    printf("OS type:           %lx\n", (unsigned long)Info.enmOsType);
    printf("Heartbeat:         %lu ms\n", (unsigned long)Info.cMsHeartbeatInterval);
    if ((Info.u32InterfaceVersion >> 16) != (VBOXGUEST98_IOCTL_VERSION >> 16))
        testFailed("GET_VERSION_INFO", "interface version mismatch");
}

static void testHostTime(HANDLE hDevice)
{
    VMMDevReqHostTime Req;

    initRequestHeader(&Req.header, sizeof(Req), VMMDevReq_GetHostTime);
    if (!doIoCtl(hDevice, IOCTL_VBOXGUEST98_VMMDEV_REQUEST, &Req, sizeof(Req), sizeof(Req)))
        testFailed("VMMDEV_REQUEST(GetHostTime)", "DeviceIoControl failed");
    else if (RT_FAILURE(Req.header.rc))
    {
        printf("GetHostTime rc=%ld\n", (long)Req.header.rc);
        testFailed("VMMDEV_REQUEST(GetHostTime)", "request failed");
    }
    else
        printf("Host time:         %I64u ms since 1970\n", Req.time);
}

static void testRefusedRequest(HANDLE hDevice)
{
    VMMDevEvents Req;

    initRequestHeader(&Req.header, sizeof(Req), VMMDevReq_AcknowledgeEvents);
    if (doIoCtl(hDevice, IOCTL_VBOXGUEST98_VMMDEV_REQUEST, &Req, sizeof(Req), sizeof(Req)))
        testFailed("VMMDEV_REQUEST(AcknowledgeEvents)", "should have been refused");
    else
        printf("AcknowledgeEvents refused as expected\n");
}

static void testWaitForEvents(HANDLE hDevice)
{
    VBOXGUEST98CHANGEMASK Mask;
    VBOXGUEST98WAITEVENTS Wait;

    Mask.fOrMask  = VMMDEV_EVENT_DISPLAY_CHANGE_REQUEST;
    Mask.fNotMask = 0;
    if (!doIoCtl(hDevice, IOCTL_VBOXGUEST98_CHANGE_FILTER_MASK, &Mask, sizeof(Mask), 0))
    {
        testFailed("CHANGE_FILTER_MASK", "DeviceIoControl failed");
        return;
    }

    Wait.cMsTimeout = 250;
    Wait.fEvents    = VMMDEV_EVENT_DISPLAY_CHANGE_REQUEST;
    Wait.rc         = VERR_GENERAL_FAILURE;
    if (!doIoCtl(hDevice, IOCTL_VBOXGUEST98_WAIT_FOR_EVENTS, &Wait, sizeof(Wait), sizeof(Wait)))
        testFailed("WAIT_FOR_EVENTS", "DeviceIoControl failed");
    else if (Wait.rc != VERR_TIMEOUT && Wait.rc != VINF_SUCCESS)
        testFailed("WAIT_FOR_EVENTS", "unexpected status");
    else
        printf("WAIT_FOR_EVENTS:   rc=%ld events=%lx\n", (long)Wait.rc, (unsigned long)Wait.fEvents);

    Mask.fOrMask  = 0;
    Mask.fNotMask = VMMDEV_EVENT_DISPLAY_CHANGE_REQUEST;
    doIoCtl(hDevice, IOCTL_VBOXGUEST98_CHANGE_FILTER_MASK, &Mask, sizeof(Mask), 0);
}

#pragma pack(push, 4)
/** Layout of a GUEST_PROP_FN_GET_PROP call. */
typedef struct GETPROPCALL
{
    VBOXGUEST98HGCMCALL Hdr;
    VBOXGUEST98HGCMPARM aParms[GUEST_PROP_FN_GET_PROP_CPARMS];
    char szName[64];
    char achValue[1024];
} GETPROPCALL;
#pragma pack(pop)

static void testGuestProperty(HANDLE hDevice)
{
    static const char s_szProp[] = "/VirtualBox/HostInfo/VBoxVer";
    VBOXGUEST98HGCMCONNECT Connect;
    VBOXGUEST98HGCMDISCONNECT Disconnect;
    GETPROPCALL Call;

    memset(&Connect, 0, sizeof(Connect));
    strcpy(Connect.szServiceName, GUEST_PROP_SERVICE_NAME);
    if (!doIoCtl(hDevice, IOCTL_VBOXGUEST98_HGCM_CONNECT, &Connect, sizeof(Connect), sizeof(Connect)))
    {
        testFailed("HGCM_CONNECT", "DeviceIoControl failed");
        return;
    }
    if (RT_FAILURE(Connect.rc))
    {
        printf("HGCM_CONNECT rc=%ld\n", (long)Connect.rc);
        testFailed("HGCM_CONNECT", "connect failed");
        return;
    }
    printf("HGCM client:       %lu\n", (unsigned long)Connect.u32ClientID);

    memset(&Call, 0, sizeof(Call));
    strcpy(Call.szName, s_szProp);
    Call.Hdr.u32ClientID = Connect.u32ClientID;
    Call.Hdr.u32Function = GUEST_PROP_FN_GET_PROP;
    Call.Hdr.cMsTimeout  = 5000;
    Call.Hdr.cParms      = GUEST_PROP_FN_GET_PROP_CPARMS;
    Call.aParms[0].enmType = VBOXGUEST98_HGCM_PARM_BUFFER_IN;
    Call.aParms[0].cb      = sizeof(s_szProp);
    Call.aParms[0].off     = (UINT32)((char *)&Call.szName[0] - (char *)&Call);
    Call.aParms[1].enmType = VBOXGUEST98_HGCM_PARM_BUFFER_OUT;
    Call.aParms[1].cb      = sizeof(Call.achValue);
    Call.aParms[1].off     = (UINT32)((char *)&Call.achValue[0] - (char *)&Call);
    Call.aParms[2].enmType = VBOXGUEST98_HGCM_PARM_64BIT;
    Call.aParms[3].enmType = VBOXGUEST98_HGCM_PARM_32BIT;

    if (!doIoCtl(hDevice, IOCTL_VBOXGUEST98_HGCM_CALL, &Call, sizeof(Call), sizeof(Call)))
        testFailed("HGCM_CALL", "DeviceIoControl failed");
    else if (RT_FAILURE(Call.Hdr.rc))
    {
        printf("HGCM_CALL rc=%ld\n", (long)Call.Hdr.rc);
        testFailed("HGCM_CALL", "GET_PROP failed");
    }
    else
    {
        Call.achValue[sizeof(Call.achValue) - 1] = '\0';
        printf("%s = %s\n", s_szProp, Call.achValue);
    }

    Disconnect.u32ClientID = Connect.u32ClientID;
    Disconnect.rc = VERR_GENERAL_FAILURE;
    if (   !doIoCtl(hDevice, IOCTL_VBOXGUEST98_HGCM_DISCONNECT, &Disconnect, sizeof(Disconnect), sizeof(Disconnect))
        || RT_FAILURE(Disconnect.rc))
        testFailed("HGCM_DISCONNECT", "disconnect failed");
}

int __cdecl main(void)
{
    HANDLE hDevice = openDriver();
    if (hDevice == INVALID_HANDLE_VALUE)
    {
        printf("openDriver failed!\n");
        return 1;
    }

    testVersionInfo(hDevice);
    testHostTime(hDevice);
    testRefusedRequest(hDevice);
    testWaitForEvents(hDevice);
    testGuestProperty(hDevice);

    CloseHandle(hDevice);

    if (g_cErrors == 0)
        printf("operation completed successfully!\n");
    else
        printf("error: %d test(s) failed\n", g_cErrors);
    return g_cErrors;
}
