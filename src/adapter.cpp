#include "adapter.h"

#include <setupapi.h>
#include <winusb.h>
#include <usbiodef.h>
#include <cfgmgr32.h>
#include <shlwapi.h>
#include <algorithm>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "winusb.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "shlwapi.lib")

// WinUSB 设备的接口描述符校验：必须是蓝牙类 E0/01/01
static bool IsBluetoothWinUsb(const std::wstring& path)
{
    HANDLE h = CreateFileW(path.c_str(),
                           GENERIC_WRITE | GENERIC_READ,
                           FILE_SHARE_WRITE | FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    WINUSB_INTERFACE_HANDLE w = nullptr;
    bool ok = WinUsb_Initialize(h, &w);
    if (ok) {
        USB_INTERFACE_DESCRIPTOR desc = {};
        ok = SUCCEEDED(WinUsb_QueryInterfaceSettings(w, 0, &desc))
             && desc.bInterfaceClass    == 0xE0
             && desc.bInterfaceSubClass == 0x01
             && desc.bInterfaceProtocol == 0x01;
        WinUsb_Free(w);
    }
    CloseHandle(h);
    return ok;
}

// 从硬件 ID 串解析 VID/PID（形如 USB\VID_0A12&PID_0001，大小写不限）
static bool ParseVidPid(const wchar_t* hwid, uint16_t& vid, uint16_t& pid)
{
    if (!hwid) return false;
    const wchar_t* v = StrStrIW(hwid, L"VID_");
    const wchar_t* p = StrStrIW(hwid, L"PID_");
    if (!v || !p) return false;
    long vv = wcstol(v + 4, nullptr, 16);
    long pp = wcstol(p + 4, nullptr, 16);
    if (vv <= 0 || pp < 0) return false;
    vid = (uint16_t)vv;
    pid = (uint16_t)pp;
    return true;
}

std::vector<BtAdapter> AdapterEnumAll(uint16_t curVid, uint16_t curPid)
{
    std::vector<BtAdapter> out;
    HDEVINFO di = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_USB_DEVICE, nullptr, nullptr,
                                       DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (di == INVALID_HANDLE_VALUE) return out;

    SP_DEVICE_INTERFACE_DATA ifd = {};
    ifd.cbSize = sizeof(ifd);
    for (int i = 0; SetupDiEnumDeviceInterfaces(di, nullptr, &GUID_DEVINTERFACE_USB_DEVICE, i, &ifd); ++i) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(di, &ifd, nullptr, 0, &need, nullptr);
        if (!need) continue;
        std::vector<uint8_t> detBuf(need);
        auto* det = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)detBuf.data();
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA dv = {};
        dv.cbSize = sizeof(dv);
        if (!SetupDiGetDeviceInterfaceDetailW(di, &ifd, det, need, nullptr, &dv)) continue;

        wchar_t svc[128] = L"";
        SetupDiGetDeviceRegistryPropertyW(di, &dv, SPDRP_SERVICE, nullptr,
                                          (PBYTE)svc, sizeof(svc), nullptr);
        bool winusb = _wcsicmp(svc, L"WinUSB") == 0;
        bool bthusb = _wcsicmp(svc, L"BTHUSB") == 0;
        if (!winusb && !bthusb) continue;   // 只关心这两类

        wchar_t hwids[2048] = L"";
        SetupDiGetDeviceRegistryPropertyW(di, &dv, SPDRP_HARDWAREID, nullptr,
                                          (PBYTE)hwids, sizeof(hwids), nullptr);

        BtAdapter a;
        a.path = det->DevicePath;
        a.service = svc;
        a.winusb = winusb;
        if (!ParseVidPid(hwids, a.vid, a.pid)) continue;

        wchar_t name[256] = L"";
        if (!SetupDiGetDeviceRegistryPropertyW(di, &dv, SPDRP_FRIENDLYNAME, nullptr,
                                               (PBYTE)name, sizeof(name), nullptr))
            SetupDiGetDeviceRegistryPropertyW(di, &dv, SPDRP_DEVICEDESC, nullptr,
                                              (PBYTE)name, sizeof(name), nullptr);
        a.name = name;

        wchar_t iid[MAX_DEVICE_ID_LEN] = L"";
        if (SUCCEEDED(CM_Get_Device_IDW(dv.DevInst, iid, MAX_DEVICE_ID_LEN, 0)))
            a.instanceId = iid;

        if (winusb) {
            // 本程序正占用该设备（推流中）时 WinUsb_Initialize 会因设备忙失败，
            // 不能据此否定它——正在使用的 VID/PID 直接视为蓝牙适配器
            a.isBluetooth = (a.vid == curVid && a.pid == curPid) || IsBluetoothWinUsb(a.path);
        } else {
            a.isBluetooth = true;                        // BTHUSB = Windows 蓝牙栈 = 必是蓝牙适配器
        }
        if (!a.isBluetooth) continue;

        // 去重：同一 VID/PID 的多个接口只留一条，优先 WinUSB
        auto it = std::find_if(out.begin(), out.end(), [&a](const BtAdapter& x) {
            return x.vid == a.vid && x.pid == a.pid;
        });
        if (it == out.end()) {
            out.push_back(a);
        } else if (a.winusb && !it->winusb) {
            *it = a;
        }
    }
    SetupDiDestroyDeviceInfoList(di);

    // 使用中的排前面
    std::sort(out.begin(), out.end(), [](const BtAdapter& x, const BtAdapter& y) {
        return x.winusb > y.winusb;
    });
    return out;
}

bool AdapterWriteInf(uint16_t vid, uint16_t pid, std::wstring& outInfPath)
{
    // 模板 = exe 同目录的 DualBltWinUSB.inf，替换硬件 ID 后存到 adapters 子目录
    wchar_t exePathW[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, exePathW, MAX_PATH);
    std::wstring dir(exePathW);
    size_t p = dir.find_last_of(L"\\/");
    if (p == std::wstring::npos) return false;
    dir = dir.substr(0, p + 1);

    std::wstring tplPath = dir + L"DualBltWinUSB.inf";
    FILE* f = nullptr;
    if (_wfopen_s(&f, tplPath.c_str(), L"rb") != 0 || !f) return false;
    std::string tpl;
    char chunk[4096];
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), f)) > 0) tpl.append(chunk, got);
    fclose(f);

    const std::string oldA = "VID_0A12&PID_0001";
    const std::string oldB = "vid_0a12&pid_0001";
    char newId[32];
    sprintf_s(newId, "VID_%04X&PID_%04X", vid, pid);
    auto ReplaceAll = [&](std::string& s, const std::string& o, const std::string& n) {
        size_t p2 = 0;
        while ((p2 = s.find(o, p2)) != std::string::npos) {
            s.replace(p2, o.size(), n);
            p2 += n.size();
        }
    };
    ReplaceAll(tpl, oldA, newId);
    ReplaceAll(tpl, oldB, newId);
    ReplaceAll(tpl, "DualBltWinUSB.inf", std::string("DualBltWinUSB_") + newId + ".inf");

    std::wstring sub = dir + L"adapters\\" + std::to_wstring(vid) + L"_" + std::to_wstring(pid);
    CreateDirectoryW(sub.c_str(), nullptr);
    std::wstring infPath = sub + L"\\DualBltWinUSB.inf";

    FILE* o = nullptr;
    if (_wfopen_s(&o, infPath.c_str(), L"wb") != 0 || !o) return false;
    fwrite(tpl.data(), 1, tpl.size(), o);
    fclose(o);

    outInfPath = infPath;
    return true;
}
