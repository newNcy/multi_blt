#ifndef UNICODE
#define UNICODE
#endif

// 第二阶段探测 —— 全程只读。
// 目标：找出这个 dongle 到底响应哪条厂商命令通道（如果有的话）。
// 只发读取类请求，任何情况下都不写 NVM。

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
        SetupDiDestroyDeviceInfoList(di); return false;
    }
    DWORD need = 0;
    SetupDiGetDeviceInterfaceDetailW(di, &ifd, nullptr, 0, &need, nullptr);
    std::vector<BYTE> buf(need ? need : 512);
    PSP_DEVICE_INTERFACE_DETAIL_DATA_W d = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)buf.data();
    d->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    if (!SetupDiGetDeviceInterfaceDetailW(di, &ifd, d, (DWORD)buf.size(), nullptr, nullptr)) {
        SetupDiDestroyDeviceInfoList(di); return false;
    }
    outPath = d->DevicePath;
    SetupDiDestroyDeviceInfoList(di);
    return true;
}

static bool WaitOv(WINUSB_INTERFACE_HANDLE h, OVERLAPPED& ov, ULONG& tx)
{
    if (WaitForSingleObject(ov.hEvent, 2500) != WAIT_OBJECT_0) return false;
    return WinUsb_GetOverlappedResult(h, &ov, &tx, FALSE) ? true : false;
}

static bool ControlXfer(WINUSB_INTERFACE_HANDLE h, BYTE reqType, BYTE req,
                        WORD value, WORD index, BYTE* buf, int len, ULONG& tx)
{
    WINUSB_SETUP_PACKET sp = {};
    sp.RequestType = reqType;
    sp.Request     = req;
    sp.Value       = value;
    sp.Index       = index;
    sp.Length      = (WORD)len;

    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return false;
    BOOL ok = WinUsb_ControlTransfer(h, sp, buf, (ULONG)len, &tx, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) ok = WaitOv(h, ov, tx);
    CloseHandle(ov.hEvent);
    return ok;
}

static bool HciCommand(WINUSB_INTERFACE_HANDLE h, const BYTE* cmd, int len)
{
    ULONG tx = 0;
    return ControlXfer(h, 0x20, 0x00, 0, 0, (BYTE*)cmd, len, tx);
}

static int ReadEvent(WINUSB_INTERFACE_HANDLE h, UCHAR pipe, BYTE* buf, int cap)
{
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return -1;
    ULONG got = 0;
    BOOL ok = WinUsb_ReadPipe(h, pipe, buf, (ULONG)cap, &got, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) ok = WaitOv(h, ov, got);
    DWORD err = GetLastError();
    CloseHandle(ov.hEvent);
    if (!ok) {
        if (err == ERROR_SEM_TIMEOUT || err == WAIT_TIMEOUT) return 0;
        return -1;
    }
    return (int)got;
}

static void DumpHex(const BYTE* b, int n)
{
    for (int i = 0; i < n && i < 48; ++i) wprintf(L"%02X ", b[i]);
}

int wmain()
{
    std::wstring path;
    if (!FindWinUsbPath(path)) { wprintf(L"No WinUSB interface found.\n"); return 1; }

    HANDLE dev = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (dev == INVALID_HANDLE_VALUE) { wprintf(L"CreateFile failed %lu\n", GetLastError()); return 1; }

    WINUSB_INTERFACE_HANDLE h = nullptr;
    if (!WinUsb_Initialize(dev, &h)) { wprintf(L"WinUsb_Initialize failed %lu\n", GetLastError()); CloseHandle(dev); return 1; }

    USB_INTERFACE_DESCRIPTOR ifd = {};
    WinUsb_QueryInterfaceSettings(h, 0, &ifd);
    UCHAR evtPipe = 0xFF;
    for (UCHAR i = 0; i < ifd.bNumEndpoints; ++i) {
        WINUSB_PIPE_INFORMATION pi = {};
        if (!WinUsb_QueryPipe(h, 0, i, &pi)) continue;
        if (pi.PipeType == UsbdPipeTypeInterrupt && (pi.PipeId & 0x80)) evtPipe = pi.PipeId;
    }
    if (evtPipe == 0xFF) { wprintf(L"No interrupt IN pipe\n"); WinUsb_Free(h); CloseHandle(dev); return 1; }

    ULONG tmo = 1200;
    WinUsb_SetPipePolicy(h, evtPipe, PIPE_TRANSFER_TIMEOUT, sizeof(tmo), &tmo);
    BOOL ac = TRUE;
    WinUsb_SetPipePolicy(h, evtPipe, AUTO_CLEAR_STALL, sizeof(ac), &ac);

    BYTE ev[512] = {};

    // ---------- 阶段 1：确认 dongle 仍然活着 ----------
    wprintf(L"=== Phase 1: alive check ===\n");
    {
        BYTE cmd[3] = { 0x03, 0x0C, 0x00 };   // HCI_Reset
        HciCommand(h, cmd, 3);
        int n = ReadEvent(h, evtPipe, ev, sizeof(ev));
        wprintf(L"HCI_Reset -> n=%d ", n); DumpHex(ev, n); wprintf(L"\n");

        BYTE cmd2[3] = { 0x09, 0x10, 0x00 };  // Read_BD_ADDR
        HciCommand(h, cmd2, 3);
        n = ReadEvent(h, evtPipe, ev, sizeof(ev));
        wprintf(L"Read_BD_ADDR -> n=%d ", n); DumpHex(ev, n); wprintf(L"\n");
    }

    // ---------- 阶段 2：USB 厂商控制请求（CSR 的另一条 BCCMD 通道）----------
    wprintf(L"\n=== Phase 2: USB vendor control IN ===\n");
    for (BYTE req = 0x00; req <= 0x03; ++req) {
        BYTE buf[64] = {};
        ULONG tx = 0;
        bool ok = ControlXfer(h, 0xC0, req, 0, 0, buf, 64, tx);
        wprintf(L"  bRequest=0x%02X ok=%d tx=%3lu  ", req, (int)ok, tx);
        if (ok && tx > 0) DumpHex(buf, (int)tx);
        wprintf(L"\n");
    }

    // ---------- 阶段 3：厂商 OGF 是否有任何响应 ----------
    // 零参数命令；只要芯片认这个 opcode，哪怕参数不对也会回 Command Complete/Status
    wprintf(L"\n=== Phase 3: vendor opcodes (zero params) ===\n");
    unsigned opcodes[] = {
        0xFC00, 0xFC01, 0xFC0B, 0xFC1C, 0xFC2B, 0xFC3B,
        0xFC4C, 0xFC51, 0xFC5A, 0xFD00, 0xFE00, 0xFF00
    };
    for (unsigned op : opcodes) {
        BYTE cmd[3];
        cmd[0] = (BYTE)(op & 0xFF);
        cmd[1] = (BYTE)((op >> 8) & 0xFF);
        cmd[2] = 0x00;
        if (!HciCommand(h, cmd, 3)) {
            wprintf(L"  0x%04X : send failed\n", op);
            continue;
        }
        int n = ReadEvent(h, evtPipe, ev, sizeof(ev));
        if (n > 0) {
            wprintf(L"  0x%04X : RESPONSE n=%d  ", op, n);
            DumpHex(ev, n); wprintf(L"\n");
        } else {
            wprintf(L"  0x%04X : no response\n", op);
        }
    }

    // ---------- 阶段 4：读本地版本信息，看芯片真实身份 ----------
    wprintf(L"\n=== Phase 4: Read Local Version Information ===\n");
    {
        BYTE cmd[3] = { 0x01, 0x10, 0x00 };
        HciCommand(h, cmd, 3);
        int n = ReadEvent(h, evtPipe, ev, sizeof(ev));
        wprintf(L"  n=%d ", n); DumpHex(ev, n);
        if (n >= 13 && ev[0] == 0x0E && ev[5] == 0x00) {
            unsigned hciVer = ev[6];
            unsigned hciRev = (ev[8] << 8) | ev[7];
            unsigned lmpVer = ev[9];
            unsigned mfr    = (ev[11] << 8) | ev[10];
            unsigned subver = (ev[13] << 8) | ev[12];
            wprintf(L"\n  hci_ver=0x%02X hci_rev=0x%04X lmp_ver=0x%02X\n", hciVer, hciRev, lmpVer);
            wprintf(L"  manufacturer=0x%04X lmp_subversion=0x%04X\n", mfr, subver);
        }
        wprintf(L"\n");
    }

    WinUsb_Free(h);
    CloseHandle(dev);
    return 0;
}
