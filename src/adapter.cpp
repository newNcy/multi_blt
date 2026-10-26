#include "adapter.h"

#include <setupapi.h>
#include <winusb.h>
#include <newdev.h>
#include <usbiodef.h>
#include <cfgmgr32.h>
#include <shlwapi.h>
#include <algorithm>

// 临时诊断日志：适配器切换的步进记录（exe 目录 adapter_switch.log）
static void SwLog(const wchar_t* fmt, ...)
{
    wchar_t exePathW[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, exePathW, MAX_PATH);
    std::wstring dir(exePathW);
    size_t p = dir.find_last_of(L"\\/");
    if (p == std::wstring::npos) return;
    std::wstring logPath = dir.substr(0, p + 1) + L"adapter_switch.log";
    FILE* lf = nullptr;
    if (_wfopen_s(&lf, logPath.c_str(), L"a, ccs=UTF-8") != 0 || !lf) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fwprintf(lf, L"[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
    va_list ap; va_start(ap, fmt);
    vfwprintf(lf, fmt, ap);
    va_end(ap);
    fwprintf(lf, L"\n");
    fclose(lf);
}

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "winusb.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "advapi32.lib")

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
        // 过渡期/驱动异常（如 Code 31）的设备服务名可能为空或未知，
        // 但设备类仍是 Bluetooth——这类要显示出来让用户一键修复，不能跳过
        if (!winusb && !bthusb) {
            wchar_t cls[128] = L"";
            SetupDiGetDeviceRegistryPropertyW(di, &dv, SPDRP_CLASS, nullptr,
                                              (PBYTE)cls, sizeof(cls), nullptr);
            if (_wcsicmp(cls, L"Bluetooth") != 0) continue;   // 只关心蓝牙相关
        }

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

        // 去重：同一实例 ID（同一根 dongle 的多个接口节点）只留一条，优先 WinUSB。
        // 注意不能按 VID/PID 去重——两根同型号 dongle 的 VID/PID 相同，
        // 按 VID/PID 会把两根物理适配器误合并成一条。
        auto it = std::find_if(out.begin(), out.end(), [&a](const BtAdapter& x) {
            return x.instanceId == a.instanceId;
        });
        if (it == out.end()) {
            out.push_back(a);
        } else if (a.winusb && !it->winusb) {
            *it = a;
        }
    }
    SetupDiDestroyDeviceInfoList(di);

    // 第二遍：按设备节点枚举。驱动加载失败（如 Code 31）的设备接口不可枚举，
    // 第一遍看不到；节点层能看见它们，用户才能在列表里选中并一键修复
    HDEVINFO dn = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (dn != INVALID_HANDLE_VALUE) {
        SP_DEVINFO_DATA dv2 = { sizeof(dv2) };
        for (int i = 0; SetupDiEnumDeviceInfo(dn, i, &dv2); ++i) {
            wchar_t hwids[2048] = L"";
            SetupDiGetDeviceRegistryPropertyW(dn, &dv2, SPDRP_HARDWAREID, nullptr,
                                              (PBYTE)hwids, sizeof(hwids), nullptr);
            uint16_t v = 0, p = 0;
            if (!ParseVidPid(hwids, v, p)) continue;

            wchar_t svc2[128] = L"";
            SetupDiGetDeviceRegistryPropertyW(dn, &dv2, SPDRP_SERVICE, nullptr,
                                              (PBYTE)svc2, sizeof(svc2), nullptr);
            bool wu2 = _wcsicmp(svc2, L"WinUSB") == 0;
            bool bt2 = _wcsicmp(svc2, L"BTHUSB") == 0;
            // 正常 WinUSB 设备由第一遍（带 WinUsb 接口描述符验证）负责，
            // 这里跳过，否则会混进其它厂商的 WinUSB 设备（如 AURA 灯效）
            if (wu2) continue;
            if (!bt2) {
                wchar_t cls[128] = L"";
                SetupDiGetDeviceRegistryPropertyW(dn, &dv2, SPDRP_CLASS, nullptr,
                                                  (PBYTE)cls, sizeof(cls), nullptr);
                if (_wcsicmp(cls, L"Bluetooth") != 0) continue;
            }

            wchar_t iid2[MAX_DEVICE_ID_LEN] = L"";
            if (FAILED(CM_Get_Device_IDW(dv2.DevInst, iid2, MAX_DEVICE_ID_LEN, 0))) continue;

            // 接口遍历已覆盖的正常设备跳过（那条记录带 WinUSB 接口路径，更有用）
            bool exists = false;
            for (auto& x : out)
                if (x.instanceId == iid2) { exists = true; break; }
            if (exists) continue;

            BtAdapter a;
            a.instanceId = iid2;
            ULONG st2 = 0, prob2 = 0;
            if (CM_Get_DevNode_Status(&st2, &prob2, dv2.DevInst, 0) == CR_SUCCESS)
                a.problemCode = prob2;
            a.service = svc2;
            a.winusb = wu2;
            a.vid = v;
            a.pid = p;
            a.isBluetooth = true;   // 节点层：类过滤已确认是蓝牙设备
            wchar_t name[256] = L"";
            if (!SetupDiGetDeviceRegistryPropertyW(dn, &dv2, SPDRP_FRIENDLYNAME, nullptr,
                                                   (PBYTE)name, sizeof(name), nullptr))
                SetupDiGetDeviceRegistryPropertyW(dn, &dv2, SPDRP_DEVICEDESC, nullptr,
                                                  (PBYTE)name, sizeof(name), nullptr);
            a.name = name;
            out.push_back(a);
        }
        SetupDiDestroyDeviceInfoList(dn);
    }

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
    errno_t fe = _wfopen_s(&f, tplPath.c_str(), L"rb");
    if (fe != 0 || !f) { SwLog(L"WriteInf: 模板打开失败 fe=%d path=%s", fe, tplPath.c_str()); return false; }
    SwLog(L"WriteInf: 模板打开成功");
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
    CreateDirectoryW((dir + L"adapters").c_str(), nullptr);
    CreateDirectoryW(sub.c_str(), nullptr);
    std::wstring infPath = sub + L"\\DualBltWinUSB.inf";

    FILE* o = nullptr;
    errno_t fo = _wfopen_s(&o, infPath.c_str(), L"wb");
    if (fo != 0 || !o) { SwLog(L"WriteInf: 写入失败 fo=%d path=%s", fo, infPath.c_str()); return false; }
    fwrite(tpl.data(), 1, tpl.size(), o);
    fclose(o);
    SwLog(L"WriteInf: 成功 -> %s", infPath.c_str());

    outInfPath = infPath;
    return true;
}

// ------------------------------------------------------------ 按设备实例精准切换驱动
// 需要管理员权限（GUI 清单 requireAdministrator）。
// 原理：SetupDi 打开指定设备节点 -> 兼容驱动列表中选中目标 INF ->
//       DiInstallDevice 只对该节点安装。绑定写入设备实例注册表，拔插/重启持久。
// toWinUSB=true : 暂存按 VID/PID 生成的 INF，安装到该实例
// toWinUSB=false: 选中收件箱 bthusb.inf 安装到该实例（驱动包不动，别的 dongle 不受影响）
bool AdapterSwitchDriver(const std::wstring& instanceId, uint16_t vid, uint16_t pid,
                         bool toWinUSB, std::wstring& err)
{
    // ============ 驱动切换：注册表 Service 直改 + 节点重启 ============
    // WinUSB.sys 与 BTHUSB.sys 都是系统自带驱动，无需驱动库/签名/INF 安装——
    // 直接改设备实例的 Service 指向，再禁用/启用节点让 PnP 重新加载。
    // 绑定写实例注册表持久保存；改的是选中实例，另一根 dongle 不受影响。
    wchar_t hwid[64];
    swprintf_s(hwid, L"USB\\VID_%04X&PID_%04X", vid, pid);

    LPCWSTR newService;
    LPCWSTR newClassGuid;
    LPCWSTR newClass;
    if (toWinUSB) {
        newService  = L"WinUSB";
        newClassGuid = L"{36fc9e60-c465-11cf-8056-444553540000}";   // USB 类
        newClass     = L"USB";
        SwLog(L"Switch: -> WinUSB");
    } else {
        newService  = L"BTHUSB";
        newClassGuid = L"{e0cbf06c-cd8b-4647-bb8b-7b98a2f4d20a}";   // Bluetooth 类
        newClass     = L"Bluetooth";
        SwLog(L"Switch: -> BTHUSB");
    }

    std::wstring enumKey = L"SYSTEM\\CurrentControlSet\\Enum\\" + instanceId;
    HKEY hk = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, enumKey.c_str(), 0, KEY_SET_VALUE, &hk) != ERROR_SUCCESS) {
        err = L"\u6253\u5f00\u5b9e\u4f8b\u6ce8\u518c\u8868\u5931\u8d25\uff08\u9700\u7ba1\u7406\u5458\uff09";
        return false;
    }
    RegSetValueExW(hk, L"Service", 0, REG_SZ, (const BYTE*)newService, (DWORD)((wcslen(newService) + 1) * sizeof(WCHAR)));
    RegSetValueExW(hk, L"ClassGUID", 0, REG_SZ, (const BYTE*)newClassGuid, (DWORD)((wcslen(newClassGuid) + 1) * sizeof(WCHAR)));
    RegSetValueExW(hk, L"Class", 0, REG_SZ, (const BYTE*)newClass, (DWORD)((wcslen(newClass) + 1) * sizeof(WCHAR)));
    RegDeleteValueW(hk, L"Driver");   // 清掉旧类驱动键引用
    RegCloseKey(hk);
    SwLog(L"Switch: registry Service=%s ClassGUID=%s", newService, newClassGuid);

    // 节点重启：禁用 -> 启用（PnP 重新按 Service 加载驱动）
    DEVINST dn = 0;
    if (CM_Locate_DevNodeW(&dn, (DEVINSTID_W)instanceId.c_str(), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) {
        err = L"\u5b9a\u4f4d\u8bbe\u5907\u8282\u70b9\u5931\u8d25";
        return false;
    }
    if (CM_Disable_DevNode(dn, CM_DISABLE_ABSOLUTE) != CR_SUCCESS) {
        err = L"\u7981\u7528\u8bbe\u5907\u5931\u8d25\uff08\u53ef\u62d4\u63d2 dongle \u6062\u590d\uff09";
        return false;
    }
    Sleep(500);
    if (CM_Enable_DevNode(dn, 0) != CR_SUCCESS) {
        err = L"\u542f\u7528\u8bbe\u5907\u5931\u8d25\uff08\u53ef\u62d4\u63d2 dongle \u6062\u590d\uff09";
        return false;
    }
    SwLog(L"Switch: node restarted");
    return true;
}
