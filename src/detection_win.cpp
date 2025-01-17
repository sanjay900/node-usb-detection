#include <uv.h>
#include <dbt.h>
#include <iostream>
#include <iomanip>
#include <atlstr.h>

// Include Windows headers
#include <windows.h>
// Include `CM_DEVCAP_UNIQUEID`
#include <Cfgmgr32.h>
#include <stdio.h>
#include <tchar.h>
#include <strsafe.h>
#include <Setupapi.h>
#include <initguid.h>
#include <devpropdef.h>
#include <Devpkey.h>
#include <XInput.h>

#include "detection.h"
#include "deviceList.h"

using namespace std;

/**********************************
 * Local defines
 **********************************/
#define VID_TAG "VID_"
#define PID_TAG "PID_"
#define PORT_TAG "PORT_#"
#define HUB_TAG "HUB_#"
#define XINPUT_TAG "DEVICE_"

#define LIBRARY_NAME ("setupapi.dll")

#define DllImport __declspec(dllimport)

#define MAX_THREAD_WINDOW_NAME 64

/**********************************
 * Local typedefs
 **********************************/

/**********************************
 * Local Variables
 **********************************/
GUID GUID_DEVINTERFACE_USB_DEVICE = {
	0xA5DCBF10L,
	0x6530,
	0x11D2,
	0x90,
	0x1F,
	0x00,
	0xC0,
	0x4F,
	0xB9,
	0x51,
	0xED};

DWORD threadId;
HANDLE threadHandle;

HANDLE deviceChangedRegisteredEvent;
HANDLE deviceChangedSentEvent;

uv_signal_t term_signal;
uv_signal_t int_signal;

ListResultItem_t *currentDevice;
bool isAdded;
bool isRunning = false;

HINSTANCE hinstLib;

typedef BOOL(WINAPI *_SetupDiEnumDeviceInfo)(HDEVINFO DeviceInfoSet, DWORD MemberIndex, PSP_DEVINFO_DATA DeviceInfoData);
typedef HDEVINFO(WINAPI *_SetupDiGetClassDevs)(const GUID *ClassGuid, PCTSTR Enumerator, HWND hwndParent, DWORD Flags);
typedef BOOL(WINAPI *_SetupDiDestroyDeviceInfoList)(HDEVINFO DeviceInfoSet);
typedef BOOL(WINAPI *_SetupDiGetDeviceInstanceId)(HDEVINFO DeviceInfoSet, PSP_DEVINFO_DATA DeviceInfoData, PTSTR DeviceInstanceId, DWORD DeviceInstanceIdSize, PDWORD RequiredSize);
typedef BOOL(WINAPI *_SetupDiGetDeviceRegistryProperty)(HDEVINFO DeviceInfoSet, PSP_DEVINFO_DATA DeviceInfoData, DWORD Property, PDWORD PropertyRegDataType, PBYTE PropertyBuffer, DWORD PropertyBufferSize, PDWORD RequiredSize);

_SetupDiEnumDeviceInfo DllSetupDiEnumDeviceInfo;
_SetupDiGetClassDevs DllSetupDiGetClassDevs;
_SetupDiDestroyDeviceInfoList DllSetupDiDestroyDeviceInfoList;
_SetupDiGetDeviceInstanceId DllSetupDiGetDeviceInstanceId;
_SetupDiGetDeviceRegistryProperty DllSetupDiGetDeviceRegistryProperty;

/**********************************
 * Local Helper Functions protoypes
 **********************************/
void UpdateDevice(PDEV_BROADCAST_DEVICEINTERFACE pDevInf, WPARAM wParam, DeviceState_t state);
DWORD WINAPI ListenerThread(LPVOID lpParam);

void BuildInitialDeviceList();

void cbWork(uv_work_t *req);
void cbAfter(uv_work_t *req);
void cbTerminate(uv_signal_t *handle, int signum);

void ExtractDeviceInfo(HDEVINFO hDevInfo, SP_DEVINFO_DATA *pspDevInfoData, TCHAR *buf, DWORD buffSize, ListResultItem_t *resultItem);
bool CheckValidity(ListResultItem_t *item);

/**********************************
 * Public Functions
 **********************************/
void cbWork(uv_work_t *req)
{
	// We have this check in case we `Stop` before this thread starts,
	// otherwise the process will hang
	if (!isRunning)
	{
		return;
	}

	uv_signal_start(&int_signal, cbTerminate, SIGINT);
	uv_signal_start(&term_signal, cbTerminate, SIGTERM);

	WaitForSingleObject(deviceChangedRegisteredEvent, INFINITE);
}

void cbAfter(uv_work_t *req)
{
	if (!isRunning)
	{
		return;
	}

	if (isAdded)
	{
		NotifyAdded(currentDevice);
	}
	else
	{
		NotifyRemoved(currentDevice);
	}

	// Delete Item in case of removal
	if (isAdded == false)
	{
		delete currentDevice;
	}

	SetEvent(deviceChangedSentEvent);

	currentDevice = NULL;

	uv_queue_work(uv_default_loop(), req, cbWork, (uv_after_work_cb)cbAfter);
}

void cbTerminate(uv_signal_t *handle, int signum)
{
	Stop();
}

void LoadFunctions()
{

	bool success;

	hinstLib = LoadLibrary(LIBRARY_NAME);

	if (hinstLib != NULL)
	{
		DllSetupDiEnumDeviceInfo = (_SetupDiEnumDeviceInfo)GetProcAddress(hinstLib, "SetupDiEnumDeviceInfo");

		DllSetupDiGetClassDevs = (_SetupDiGetClassDevs)GetProcAddress(hinstLib, "SetupDiGetClassDevsA");

		DllSetupDiDestroyDeviceInfoList = (_SetupDiDestroyDeviceInfoList)GetProcAddress(hinstLib, "SetupDiDestroyDeviceInfoList");

		DllSetupDiGetDeviceInstanceId = (_SetupDiGetDeviceInstanceId)GetProcAddress(hinstLib, "SetupDiGetDeviceInstanceIdA");

		DllSetupDiGetDeviceRegistryProperty = (_SetupDiGetDeviceRegistryProperty)GetProcAddress(hinstLib, "SetupDiGetDeviceRegistryPropertyA");

		success = (DllSetupDiEnumDeviceInfo != NULL &&
				   DllSetupDiGetClassDevs != NULL &&
				   DllSetupDiDestroyDeviceInfoList != NULL &&
				   DllSetupDiGetDeviceInstanceId != NULL &&
				   DllSetupDiGetDeviceRegistryProperty != NULL);
	}
	else
	{
		success = false;
	}

	if (!success)
	{
		printf("Could not load library functions from dll -> abort (Check if %s is available)\r\n", LIBRARY_NAME);
		exit(1);
	}
}

void Start()
{
	if (isRunning)
	{
		return;
	}

	isRunning = true;

	// Start listening for the Windows API events
	threadHandle = CreateThread(
		NULL,			// default security attributes
		0,				// use default stack size
		ListenerThread, // thread function name
		NULL,			// argument to thread function
		0,				// use default creation flags
		&threadId);

	uv_signal_init(uv_default_loop(), &term_signal);
	uv_signal_init(uv_default_loop(), &int_signal);

	uv_work_t *req = new uv_work_t();
	uv_queue_work(uv_default_loop(), req, cbWork, (uv_after_work_cb)cbAfter);
}

void Stop()
{
	if (!isRunning)
	{
		return;
	}

	isRunning = false;

	uv_signal_stop(&int_signal);
	uv_signal_stop(&term_signal);

	SetEvent(deviceChangedRegisteredEvent);
}

void InitDetection()
{
	LoadFunctions();

	deviceChangedRegisteredEvent = CreateEvent(
		NULL,
		false, // auto-reset event
		false, // non-signalled state
		"");
	deviceChangedSentEvent = CreateEvent(
		NULL,
		false, // auto-reset event
		true,  // non-signalled state
		"");

	BuildInitialDeviceList();
}

void EIO_Find(uv_work_t *req)
{

	ListBaton *data = static_cast<ListBaton *>(req->data);

	CreateFilteredList(&data->results, data->vid, data->pid);
}

/**********************************
 * Local Functions
 **********************************/
void ToUpper(char *buf)
{
	char *c = buf;
	while (*c != '\0')
	{
		*c = toupper((unsigned char)*c);
		c++;
	}
}

void NormalizeSlashes(char *buf)
{
	char *c = buf;
	while (*c != '\0')
	{
		if (*c == '/')
			*c = '\\';
		c++;
	}
}
void extractIdBus(char *buf, ListResultItem_t *item)
{
	if (buf == NULL)
	{
		return;
	}

	ToUpper(buf);

	char *string;
	char *temp;
	char *portStr, *hubStr;
	int port = 0;
	int hub = 0;

	string = new char[strlen(buf) + 1];
	memcpy(string, buf, strlen(buf) + 1);

	portStr = strstr(string, PORT_TAG);
	hubStr = strstr(string, HUB_TAG);
	if (portStr != NULL)
	{
		temp = (char *)(portStr + strlen(PORT_TAG));
		temp[4] = '\0';
		port = strtol(temp, NULL, 16);
	}

	if (hubStr != NULL)
	{
		temp = (char *)(hubStr + strlen(HUB_TAG));
		temp[4] = '\0';
		hub = strtol(temp, NULL, 16);
	}
	item->deviceAddress = port;
	item->locationId = hub;

	delete string;
}
void extractVidPid(char *buf, ListResultItem_t *item)
{
	if (buf == NULL)
	{
		return;
	}

	ToUpper(buf);

	char *string;
	char *temp;
	char *pidStr, *vidStr;
	int vid = 0;
	int pid = 0;

	string = new char[strlen(buf) + 1];
	memcpy(string, buf, strlen(buf) + 1);

	vidStr = strstr(string, VID_TAG);
	pidStr = strstr(string, PID_TAG);

	if (vidStr != NULL)
	{
		temp = (char *)(vidStr + strlen(VID_TAG));
		temp[4] = '\0';
		vid = strtol(temp, NULL, 16);
	}

	if (pidStr != NULL)
	{
		temp = (char *)(pidStr + strlen(PID_TAG));
		temp[4] = '\0';
		pid = strtol(temp, NULL, 16);
	}
	item->vendorId = vid;
	item->productId = pid;

	delete string;
}
void extractXInput(char *buf, ListResultItem_t *item)
{
	if (buf == NULL)
	{
		return;
	}

	ToUpper(buf);

	char *string;
	char *temp;
	char *xStr;
	int x = 0;

	string = new char[strlen(buf) + 1];
	memcpy(string, buf, strlen(buf) + 1);

	xStr = strstr(string, XINPUT_TAG);

	if (xStr != NULL)
	{
		temp = (char *)(xStr + strlen(XINPUT_TAG));
		temp[4] = '\0';
		x = strtol(temp, NULL, 16);
	}

	item->xinputId = x;
	XINPUT_CAPABILITIES caps;
	ZeroMemory(&caps, sizeof(XINPUT_CAPABILITIES));
	DWORD result = XInputGetCapabilities((WORD)x, 0, &caps);
	item->subtype = caps.SubType;
	delete string;
}

LRESULT CALLBACK DetectCallback(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (msg == WM_DEVICECHANGE)
	{
		if (DBT_DEVICEARRIVAL == wParam || DBT_DEVICEREMOVECOMPLETE == wParam)
		{
			PDEV_BROADCAST_HDR pHdr = (PDEV_BROADCAST_HDR)lParam;
			PDEV_BROADCAST_DEVICEINTERFACE pDevInf;

			if (pHdr->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE)
			{
				pDevInf = (PDEV_BROADCAST_DEVICEINTERFACE)pHdr;
				UpdateDevice(pDevInf, wParam, (DBT_DEVICEARRIVAL == wParam) ? DeviceState_Connect : DeviceState_Disconnect);
			}
		}
	}

	return 1;
}

DWORD WINAPI ListenerThread(LPVOID lpParam)
{
	char className[MAX_THREAD_WINDOW_NAME];
	_snprintf_s(className, MAX_THREAD_WINDOW_NAME, "ListnerThreadUsbDetection_%d", GetCurrentThreadId());

	WNDCLASSA wincl = {0};
	wincl.hInstance = GetModuleHandle(0);
	wincl.lpszClassName = className;
	wincl.lpfnWndProc = DetectCallback;

	if (!RegisterClassA(&wincl))
	{
		DWORD le = GetLastError();
		printf("RegisterClassA() failed [Error: %x]\r\n", le);
		return 1;
	}

	HWND hwnd = CreateWindowExA(WS_EX_TOPMOST, className, className, 0, 0, 0, 0, 0, NULL, 0, 0, 0);
	if (!hwnd)
	{
		DWORD le = GetLastError();
		printf("CreateWindowExA() failed [Error: %x]\r\n", le);
		return 1;
	}

	DEV_BROADCAST_DEVICEINTERFACE_A notifyFilter = {0};
	notifyFilter.dbcc_size = sizeof(notifyFilter);
	notifyFilter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
	notifyFilter.dbcc_classguid = GUID_DEVINTERFACE_USB_DEVICE;

	HDEVNOTIFY hDevNotify = RegisterDeviceNotificationA(hwnd, &notifyFilter, DEVICE_NOTIFY_WINDOW_HANDLE);
	if (!hDevNotify)
	{
		DWORD le = GetLastError();
		printf("RegisterDeviceNotificationA() failed [Error: %x]\r\n", le);
		return 1;
	}

	MSG msg;
	while (TRUE)
	{
		BOOL bRet = GetMessage(&msg, hwnd, 0, 0);
		if ((bRet == 0) || (bRet == -1))
		{
			break;
		}

		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	return 0;
}

void BuildInitialDeviceList()
{
	DWORD dwFlag = (DIGCF_ALLCLASSES | DIGCF_PRESENT);
	HDEVINFO hDevInfo = DllSetupDiGetClassDevs(NULL, "USB", NULL, dwFlag);

	if (INVALID_HANDLE_VALUE == hDevInfo)
	{
		return;
	}

	SP_DEVINFO_DATA *pspDevInfoData = (SP_DEVINFO_DATA *)HeapAlloc(GetProcessHeap(), 0, sizeof(SP_DEVINFO_DATA));
	pspDevInfoData->cbSize = sizeof(SP_DEVINFO_DATA);
	for (int i = 0; DllSetupDiEnumDeviceInfo(hDevInfo, i, pspDevInfoData); i++)
	{
		DWORD nSize = 0;
		TCHAR buf[MAX_PATH];

		if (!DllSetupDiGetDeviceInstanceId(hDevInfo, pspDevInfoData, buf, sizeof(buf), &nSize))
		{
			break;
		}
		NormalizeSlashes(buf);

		DeviceItem_t *item = new DeviceItem_t();
		item->deviceState = DeviceState_Connect;

		DWORD DataT;
		DllSetupDiGetDeviceRegistryProperty(hDevInfo, pspDevInfoData, SPDRP_LOCATION_INFORMATION, &DataT, (PBYTE)buf, MAX_PATH, &nSize);
		DllSetupDiGetDeviceRegistryProperty(hDevInfo, pspDevInfoData, SPDRP_HARDWAREID, &DataT, (PBYTE)(buf + nSize - 1), MAX_PATH - nSize, &nSize);

		AddItemToList(buf, item);
		ExtractDeviceInfo(hDevInfo, pspDevInfoData, buf, MAX_PATH, &item->deviceParams);
	}

	if (pspDevInfoData)
	{
		HeapFree(GetProcessHeap(), 0, pspDevInfoData);
	}

	if (hDevInfo)
	{
		DllSetupDiDestroyDeviceInfoList(hDevInfo);
	}
}

void ExtractDeviceInfo(HDEVINFO hDevInfo, SP_DEVINFO_DATA *pspDevInfoData, TCHAR *buf, DWORD buffSize, ListResultItem_t *resultItem)
{

	DWORD DataT;
	DWORD nSize;
	static int dummy = 1;

	resultItem->locationId = 0;
	resultItem->deviceAddress = dummy++;
	resultItem->xinputId = -1;
	resultItem->subtype = -1;
	resultItem->deviceType = resultItem->deviceName;

	// device found
	if (DllSetupDiGetDeviceRegistryProperty(hDevInfo, pspDevInfoData, SPDRP_FRIENDLYNAME, &DataT, (PBYTE)buf, buffSize, &nSize))
	{
		resultItem->deviceName = buf;
	}
	else if (DllSetupDiGetDeviceRegistryProperty(hDevInfo, pspDevInfoData, SPDRP_DEVICEDESC, &DataT, (PBYTE)buf, buffSize, &nSize))
	{
		resultItem->deviceName = buf;
	}
	if (DllSetupDiGetDeviceRegistryProperty(hDevInfo, pspDevInfoData, SPDRP_MFG, &DataT, (PBYTE)buf, buffSize, &nSize))
	{
		resultItem->manufacturer = buf;
	}
	DEVINST child;
	CONFIGRET cr;
	DEVPROPTYPE PropertyType;
	ULONG size = MAX_PATH;
	cr = CM_Get_Child(&child, pspDevInfoData->DevInst, 0);
	if (cr == CR_SUCCESS)
	{
		cr = CM_Get_DevNode_PropertyW(child, &DEVPKEY_Device_DriverDesc, &PropertyType, (PBYTE)buf, &size, 0);
		if (cr == CR_SUCCESS)
		{
			const size_t origsize = wcslen((wchar_t*)buf) + 1;
			const size_t newsize = origsize*2;
    		size_t convertedChars = 0;
			TCHAR *buf2 = new TCHAR[newsize];
			wcstombs_s(&convertedChars, buf2, newsize, (const wchar_t *)buf, _TRUNCATE);
			resultItem->deviceType = buf2;
		}
	}
	
	if (DllSetupDiGetDeviceRegistryProperty(hDevInfo, pspDevInfoData, SPDRP_HARDWAREID, &DataT, (PBYTE)buf, buffSize, &nSize))
	{
		// Use this to extract VID / PID
		extractVidPid(buf, resultItem);
	}

	// Extract Serial Number
	//
	// Format: <device-ID>\<instance-specific-ID>
	//
	// Ex. `USB\VID_2109&PID_8110\5&376ABA2D&0&21`
	//  - `<device-ID>`: `USB\VID_2109&PID_8110`
	//  - `<instance-specific-ID>`: `5&376ABA2D&0&21`
	//
	// [Device instance IDs](https://docs.microsoft.com/en-us/windows-hardware/drivers/install/device-instance-ids) ->
	//  - [Device IDs](https://docs.microsoft.com/en-us/windows-hardware/drivers/install/device-ids) -> [Hardware IDs](https://docs.microsoft.com/en-us/windows-hardware/drivers/install/hardware-ids) -> [Device identifier formats](https://docs.microsoft.com/en-us/windows-hardware/drivers/install/device-identifier-formats) -> [Identifiers for USB devices](https://docs.microsoft.com/en-us/windows-hardware/drivers/install/identifiers-for-usb-devices)
	//     - [Standard USB Identifiers](https://docs.microsoft.com/en-us/windows-hardware/drivers/install/standard-usb-identifiers)
	//     - [Special USB Identifiers](https://docs.microsoft.com/en-us/windows-hardware/drivers/install/special-usb-identifiers)
	//  - [Instance specific ID](https://docs.microsoft.com/en-us/windows-hardware/drivers/install/instance-ids)
	DWORD dwCapabilities = 0x0;
	if (DllSetupDiGetDeviceRegistryProperty(hDevInfo, pspDevInfoData, SPDRP_CAPABILITIES, &DataT, (PBYTE)&dwCapabilities, sizeof(dwCapabilities), &nSize))
	{
		if ((dwCapabilities & CM_DEVCAP_UNIQUEID) == CM_DEVCAP_UNIQUEID)
		{
			if (DllSetupDiGetDeviceInstanceId(hDevInfo, pspDevInfoData, buf, buffSize, &nSize))
			{
				string deviceInstanceId = buf;
				size_t serialNumberIndex = deviceInstanceId.find_last_of("\\");
				if (serialNumberIndex != string::npos)
				{
					resultItem->serialNumber = deviceInstanceId.substr(serialNumberIndex + 1);
				}
			}
		}
	}
	if (DllSetupDiGetDeviceRegistryProperty(hDevInfo, pspDevInfoData, SPDRP_LOCATION_INFORMATION, &DataT, (PBYTE)buf, buffSize, &nSize))
	{
		extractIdBus(buf, resultItem);
		if (resultItem->deviceName.find("Xbox 360") != std::string::npos)
		{
			if (cr == CR_SUCCESS)
			{
				cr = CM_Get_DevNode_PropertyW(child, &DEVPKEY_Device_LocationInfo, &PropertyType, (PBYTE)buf, &size, 0);
				if (cr == CR_SUCCESS)
				{
					extractXInput(buf, resultItem);
				}
			}
		}
	}
}

void UpdateDevice(PDEV_BROADCAST_DEVICEINTERFACE pDevInf, WPARAM wParam, DeviceState_t state)
{
	// dbcc_name:
	// \\?\USB#Vid_04e8&Pid_503b#0002F9A9828E0F06#{a5dcbf10-6530-11d2-901f-00c04fb951ed}
	// convert to
	// USB\Vid_04e8&Pid_503b\0002F9A9828E0F06
	CString szDevId = pDevInf->dbcc_name + 4;
	int idx = szDevId.ReverseFind(_T('#'));

	szDevId.Truncate(idx);
	szDevId.Replace(_T('#'), _T('\\'));
	szDevId.MakeUpper();

	CString szClass;
	idx = szDevId.Find(_T('\\'));
	szClass = szDevId.Left(idx);

	// if we are adding device, we only need present devices
	// otherwise, we need all devices
	DWORD dwFlag = DBT_DEVICEARRIVAL != wParam ? DIGCF_ALLCLASSES : (DIGCF_ALLCLASSES | DIGCF_PRESENT);
	HDEVINFO hDevInfo = DllSetupDiGetClassDevs(NULL, szClass, NULL, dwFlag);
	if (INVALID_HANDLE_VALUE == hDevInfo)
	{
		return;
	}
	SP_DEVINFO_DATA *pspDevInfoData = (SP_DEVINFO_DATA *)HeapAlloc(GetProcessHeap(), 0, sizeof(SP_DEVINFO_DATA));
	pspDevInfoData->cbSize = sizeof(SP_DEVINFO_DATA);
	for (int i = 0; DllSetupDiEnumDeviceInfo(hDevInfo, i, pspDevInfoData); i++)
	{
		DWORD nSize = 0;
		TCHAR buf[MAX_PATH];

		if (!DllSetupDiGetDeviceInstanceId(hDevInfo, pspDevInfoData, buf, sizeof(buf), &nSize))
		{
			break;
		}
		NormalizeSlashes(buf);

		if (szDevId == buf)
		{
			WaitForSingleObject(deviceChangedSentEvent, INFINITE);

			DWORD DataT;
			DWORD nSize;
			DllSetupDiGetDeviceRegistryProperty(hDevInfo, pspDevInfoData, SPDRP_LOCATION_INFORMATION, &DataT, (PBYTE)buf, MAX_PATH, &nSize);
			DllSetupDiGetDeviceRegistryProperty(hDevInfo, pspDevInfoData, SPDRP_HARDWAREID, &DataT, (PBYTE)(buf + nSize - 1), MAX_PATH - nSize, &nSize);

			if (state == DeviceState_Connect)
			{
				DeviceItem_t *device = new DeviceItem_t();

				AddItemToList(buf, device);
				ExtractDeviceInfo(hDevInfo, pspDevInfoData, buf, MAX_PATH, &device->deviceParams);

				currentDevice = &device->deviceParams;
				isAdded = true;
			}
			else
			{

				ListResultItem_t *item = NULL;
				if (IsItemAlreadyStored(buf))
				{
					DeviceItem_t *deviceItem = GetItemFromList(buf);
					if (deviceItem)
					{
						item = CopyElement(&deviceItem->deviceParams);
					}
					RemoveItemFromList(deviceItem);
					delete deviceItem;
				}

				if (item == NULL)
				{
					item = new ListResultItem_t();
					ExtractDeviceInfo(hDevInfo, pspDevInfoData, buf, MAX_PATH, item);
				}
				currentDevice = item;
				isAdded = false;
			}

			break;
		}
	}

	if (pspDevInfoData)
	{
		HeapFree(GetProcessHeap(), 0, pspDevInfoData);
	}

	if (hDevInfo)
	{
		DllSetupDiDestroyDeviceInfoList(hDevInfo);
	}

	SetEvent(deviceChangedRegisteredEvent);
}
