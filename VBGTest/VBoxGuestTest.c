#include <windows.h>
#include <initguid.h>
#include <setupapi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// {719B4B48-C5DF-4474-A8E7-AF615CA3BC83}
DEFINE_GUID(WDM_GUID, 
			0x719b4b48, 0xc5df, 0x4474, 0xa8, 0xe7, 0xaf, 0x61, 0x5c, 0xa3, 0xbc, 0x83);

static HANDLE openDriver(void)
{
    HANDLE hDevice;
	SP_INTERFACE_DEVICE_DATA ifdata;
	HDEVINFO info;
	DWORD ReqLen;
	PSP_INTERFACE_DEVICE_DETAIL_DATA ifDetail;
	
	// Get handle to relevant device information set
	info = SetupDiGetClassDevs(&WDM_GUID, NULL, NULL, DIGCF_PRESENT | DIGCF_INTERFACEDEVICE);
	if(info==INVALID_HANDLE_VALUE)
	{
		printf("No HDEVINFO available for this GUID\n");
		return NULL;
	}
	
	// Get interface data for the requested instance
	ifdata.cbSize = sizeof(ifdata);
	if(!SetupDiEnumDeviceInterfaces(info, NULL, &WDM_GUID, 0, &ifdata))
	{
		printf("No SP_INTERFACE_DEVICE_DATA available for this GUID instance\n");
		SetupDiDestroyDeviceInfoList(info);
		return NULL;
	}
	
	// Get size of symbolic link name
	SetupDiGetDeviceInterfaceDetail(info, &ifdata, NULL, 0, &ReqLen, NULL);
	ifDetail = (PSP_INTERFACE_DEVICE_DETAIL_DATA)(malloc(ReqLen));
	if( ifDetail==NULL)
	{
		SetupDiDestroyDeviceInfoList(info);
		return NULL;
	}
	
	// Get symbolic link name
	ifDetail->cbSize = sizeof(SP_INTERFACE_DEVICE_DETAIL_DATA);
	if( !SetupDiGetDeviceInterfaceDetail(info, &ifdata, ifDetail, ReqLen, NULL, NULL))
	{
		SetupDiDestroyDeviceInfoList(info);
		free(ifDetail);
		return NULL;
	}
	
	printf("Symbolic link is %s\n",ifDetail->DevicePath);
	
    hDevice = CreateFile(ifDetail->DevicePath,
		GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		NULL);
    if (hDevice == INVALID_HANDLE_VALUE)
    {
        printf("CreateFile did not work. GetLastError() 0x%x\n", GetLastError());
    }
    return hDevice;
}

static int closeDriver(HANDLE hDevice)
{
    CloseHandle(hDevice);
    return 0;
}

static int performTest(void)
{
    int rc = 0;
	
    HANDLE hDevice = openDriver();
	
    if (hDevice != INVALID_HANDLE_VALUE)
        closeDriver(hDevice);
    else
        printf("openDriver failed!\n");
	
    return rc;
}

int _cdecl main(void)
{
	int rc;
	
    rc = performTest();
	
    if (rc == 0)
        printf("operation completed successfully!\n");
    else
        printf("error: operation failed with status code %d\n", rc);
	
    return rc;
}

