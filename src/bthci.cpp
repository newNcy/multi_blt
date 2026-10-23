#ifndef UNICODE
#define UNICODE
#endif

#include <windows.h>
#include <winusb.h>
#include <setupapi.h>
#include <initguid.h>
#include <usbspec.h>

#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "winusb.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "user32.lib")

// 与 DualBltWinUSB.inf 里的 DeviceInterfaceGUIDs 保持一致
DEFINE_GUID(GUID_DEVIF_DUALBT_WINUSB,
    0x2F5A1E1B, 0x9C4E, 0x4E27, 0x8A, 0x0D, 0x6B, 0x5C, 0x1D, 0x3E, 0x7A, 0x01);

static bool FindWinUsbPath(std::wstring& outPath)
{
    HDEVINFO di = SetupDiGetClassDevsW(&GUID_DEVIF_DUALBT_WINUSB, nullptr, nullptr,
                                       DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (di == INVALID_HANDLE_VALUE) return false;

    SP_DEVICE_INTERFACE_DATA ifd = {};
    ifd.cbSize = sizeof(ifd);
    if (!SetupDiEnumDeviceInterfaces(di, nullptr, &GUID_DEVIF_DUALBT_WINUSB, 0, &ifd)) {
        SetupDiDestroyDeviceInfoList(di);
        return false;
    }

    DWORD need = 0;
    SetupDiGetDeviceInterfaceDetailW(di, &ifd, nullptr, 0, &need, nullptr);
    std::vector<BYTE> buf(need ? need : 512);
    PSP_DEVICE_INTERFACE_DETAIL_DATA_W d = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)buf.data();
    d->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    if (!SetupDiGetDeviceInterfaceDetailW(di, &ifd, d, (DWORD)buf.size(), nullptr, nullptr)) {
        SetupDiDestroyDeviceInfoList(di);
        return false;
    }

    outPath = d->DevicePath;
    SetupDiDestroyDeviceInfoList(di);
    wprintf(L"[diag] interface path: %s\n", outPath.c_str());
    return true;
}

// 等待 overlapped 完成
static bool WaitOv(WINUSB_INTERFACE_HANDLE h, OVERLAPPED& ov, ULONG& transferred)
{
    if (WaitForSingleObject(ov.hEvent, 3000) != WAIT_OBJECT_0) return false;
    return WinUsb_GetOverlappedResult(h, &ov, &transferred, FALSE) ? true : false;
}

// HCI 命令走 USB 控制传输（class 请求、bRequest=0）
static bool HciCommand(WINUSB_INTERFACE_HANDLE h, const BYTE* cmd, int len)
{
    WINUSB_SETUP_PACKET sp = {};
    sp.RequestType = 0x20;          // Host->Device | Class | Device
    sp.Request     = 0x00;
    sp.Value       = 0x0000;
    sp.Index       = 0x0000;
    sp.Length      = (WORD)len;

    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return false;

    ULONG tx = 0;
    BOOL ok = WinUsb_ControlTransfer(h, sp, (BYTE*)cmd, (ULONG)len, &tx, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) ok = WaitOv(h, ov, tx);

    DWORD err = GetLastError();
    CloseHandle(ov.hEvent);
    if (!ok) { wprintf(L"   [control transfer failed, err=%lu]\n", err); return false; }
    return tx == (ULONG)len;
}

static int ReadEvent(WINUSB_INTERFACE_HANDLE h, UCHAR pipeId, BYTE* buf, int cap)
{
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return -1;

    ULONG got = 0;
    BOOL ok = WinUsb_ReadPipe(h, pipeId, buf, (ULONG)cap, &got, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) ok = WaitOv(h, ov, got);

    DWORD err = GetLastError();
    CloseHandle(ov.hEvent);
    if (!ok) {
        // 超时不算错误，返回 0
        if (err == ERROR_SEM_TIMEOUT || err == WAIT_TIMEOUT) return 0;
        return -1;
    }
    return (int)got;
}

int wmain()
{
    std::wstring path;
    if (!FindWinUsbPath(path)) {
        wprintf(L"No WinUSB device interface found.\n");
        return 1;
    }
    wprintf(L"device : %s\n\n", path.c_str());

    HANDLE dev = CreateFileW(path.c_str(),
                             GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                             nullptr);
    if (dev == INVALID_HANDLE_VALUE) {
        wprintf(L"CreateFile failed: %lu\n", GetLastError());
        return 1;
    }

    WINUSB_INTERFACE_HANDLE h = nullptr;
    if (!WinUsb_Initialize(dev, &h)) {
        wprintf(L"WinUsb_Initialize failed: %lu\n", GetLastError());
        CloseHandle(dev);
        return 1;
    }

    USB_INTERFACE_DESCRIPTOR ifd = {};
    if (!WinUsb_QueryInterfaceSettings(h, 0, &ifd)) {
        wprintf(L"QueryInterfaceSettings failed: %lu\n", GetLastError());
        WinUsb_Free(h); CloseHandle(dev);
        return 1;
    }
    wprintf(L"interface 0: class=0x%02X sub=0x%02X proto=0x%02X endpoints=%d\n",
            ifd.bInterfaceClass, ifd.bInterfaceSubClass,
            ifd.bInterfaceProtocol, ifd.bNumEndpoints);

    // 找中断 IN 管道（HCI 事件）
    UCHAR evtPipe = 0xFF;
    for (UCHAR i = 0; i < ifd.bNumEndpoints; ++i) {
        WINUSB_PIPE_INFORMATION pi = {};
        if (!WinUsb_QueryPipe(h, 0, i, &pi)) continue;
        wprintf(L"  pipe id=0x%02X type=%d maxpkt=%d interval=%d\n",
                pi.PipeId, (int)pi.PipeType, pi.MaximumPacketSize, pi.Interval);
        if (pi.PipeType == UsbdPipeTypeInterrupt && (pi.PipeId & 0x80))
            evtPipe = pi.PipeId;
    }
    if (evtPipe == 0xFF) {
        wprintf(L"No interrupt IN pipe found.\n");
        WinUsb_Free(h); CloseHandle(dev);
        return 1;
    }
    wprintf(L"event pipe = 0x%02X\n\n", evtPipe);

    ULONG timeoutMs = 2000;
    WinUsb_SetPipePolicy(h, evtPipe, PIPE_TRANSFER_TIMEOUT, sizeof(timeoutMs), &timeoutMs);
    BOOL autoClear = TRUE;
    WinUsb_SetPipePolicy(h, evtPipe, AUTO_CLEAR_STALL, sizeof(autoClear), &autoClear);

    BYTE ev[512] = {};

    // 1) HCI_Reset  (opcode 0x0C03)
    {
        BYTE cmd[3] = { 0x03, 0x0C, 0x00 };
        wprintf(L"[1] HCI_Reset ... ");
        if (!HciCommand(h, cmd, 3)) {
            wprintf(L"failed\n");
        } else {
            int n = ReadEvent(h, evtPipe, ev, sizeof(ev));
            // 事件布局: 0=0x0E  1=参数总长  2=num cmd packets  3..4=opcode  5=status
            if (n > 0 && ev[0] == 0x0E)
                wprintf(L"OK, Command Complete (opcode=0x%02X%02X status=0x%02X)\n",
                        ev[4], ev[3], ev[5]);
            else
                wprintf(L"event n=%d code=0x%02X\n", n, n > 0 ? ev[0] : 0xFF);
        }
    }

    // 2) HCI_Read_BD_ADDR  (opcode 0x1009)
    {
        BYTE cmd[3] = { 0x09, 0x10, 0x00 };
        wprintf(L"[2] HCI_Read_BD_ADDR ... ");
        if (!HciCommand(h, cmd, 3)) {
            wprintf(L"failed\n");
        } else {
            int n = ReadEvent(h, evtPipe, ev, sizeof(ev));
            if (n >= 12 && ev[0] == 0x0E &&
                ev[3] == 0x09 && ev[4] == 0x10 && ev[5] == 0x00)
            {
                // 地址 6 字节按反序打印（小端）
                wprintf(L"OK\n\n");
                wprintf(L"    BD_ADDR = %02X:%02X:%02X:%02X:%02X:%02X\n",
                        ev[11], ev[10], ev[9], ev[8], ev[7], ev[6]);
                wprintf(L"RESULT_ADDR=%02X:%02X:%02X:%02X:%02X:%02X\n",
                        ev[11], ev[10], ev[9], ev[8], ev[7], ev[6]);
            } else if (n > 0) {
                wprintf(L"\n    unexpected event n=%d: ", n);
                for (int i = 0; i < n && i < 20; ++i) wprintf(L"%02X ", ev[i]);
                wprintf(L"\n");
            } else {
                wprintf(L"no event (n=%d)\n", n);
            }
        }
    }

    WinUsb_Free(h);
    CloseHandle(dev);
    return 0;
}
