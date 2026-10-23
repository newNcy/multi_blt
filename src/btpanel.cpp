#ifndef UNICODE
#define UNICODE
#endif

#include "btpanel.h"
#include "btcore.h"

#include <windows.h>
#include <commctrl.h>
#include <bthdef.h>
#include <bluetoothapis.h>

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <cstdio>
#include <cstdarg>

#pragma comment(lib, "Bthprops.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

// 经典蓝牙服务 UUID
static const GUID GUID_A2DP_SINK = { 0x0000110B, 0x0000, 0x1000, { 0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB } };
static const GUID GUID_HANDSFREE = { 0x0000111E, 0x0000, 0x1000, { 0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB } };
static const GUID GUID_HEADSET   = { 0x00001108, 0x0000, 0x1000, { 0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB } };

enum {
    ID_BT_RADIOS = 2101,
    ID_BT_DEVS,
    ID_BT_REFRESH,
    ID_BT_SCAN,
    ID_BT_PAIR,
    ID_BT_CONNECT,
    ID_BT_DISCONNECT,
    ID_BT_UNPAIR,
    ID_BT_DISCOVERABLE,
    ID_BT_STATUS
};

#define WM_BT_STATUS     (WM_APP + 20)
#define WM_BT_REFRESHED  (WM_APP + 21)
#define WM_BT_DEVS_READY (WM_APP + 22)   // WinUSB 扫描完成

// ---------------------------------------------------------------- 数据模型

enum AdapterKind { ADK_WINDOWS = 0, ADK_WINUSB = 1 };

struct Adapter {
    int         kind = ADK_WINDOWS;
    HANDLE      hRadio = INVALID_HANDLE_VALUE;   // 仅 Windows
    std::wstring name;
    std::wstring addr;
    std::wstring note;                            // WinUSB 打不开时的说明
};

struct DevEntry {
    std::wstring name, addr, state;
    bool isAudio = false;
    bool isWin   = false;      // 可用 Windows API 操作
    int  adapterIdx = -1;
    BLUETOOTH_DEVICE_INFO wdi = {};
};

static HWND                       g_bt = nullptr;
static HINSTANCE                  g_btInst = nullptr;
static HWND                       g_lvRadios = nullptr;
static HWND                       g_lvDevs = nullptr;
static HWND                       g_btStatus = nullptr;
static HFONT                      g_btFont = nullptr;
static std::vector<Adapter>       g_adapters;
static std::vector<DevEntry>      g_devs;
static std::atomic<bool>          g_busy{ false };
static std::atomic<bool>          g_scanRun{ false };
static std::mutex                 g_devsMx;
static HWND                       g_btnScan = nullptr;
static HANDLE                     g_authRadio = INVALID_HANDLE_VALUE;
static HBLUETOOTH_AUTHENTICATION_REGISTRATION g_authReg = nullptr;

// ---------------------------------------------------------------- 工具

static void PostStatus(const std::wstring& s)
{
    if (!g_bt) return;
    std::wstring* p = new std::wstring(s);
    if (!PostMessageW(g_bt, WM_BT_STATUS, (WPARAM)p, 0)) delete p;
}

static std::wstring AddrStr(const BLUETOOTH_ADDRESS& a)
{
    wchar_t buf[64];
    swprintf(buf, 64, L"%02X:%02X:%02X:%02X:%02X:%02X",
             a.rgBytes[5], a.rgBytes[4], a.rgBytes[3],
             a.rgBytes[2], a.rgBytes[1], a.rgBytes[0]);
    return buf;
}

static std::wstring AddrStr6(const u8 a[6])
{
    wchar_t buf[64];
    swprintf(buf, 64, L"%02X:%02X:%02X:%02X:%02X:%02X",
             a[5], a[4], a[3], a[2], a[1], a[0]);
    return buf;
}

static std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static const wchar_t* MajorClass(ULONG cod)
{
    switch ((cod >> 8) & 0x1F) {
    case 0x00: return L"杂项";
    case 0x01: return L"计算机";
    case 0x02: return L"手机";
    case 0x03: return L"网络设备";
    case 0x04: return L"音频/视频";
    case 0x05: return L"外设";
    case 0x06: return L"成像";
    case 0x07: return L"可穿戴";
    case 0x08: return L"玩具";
    case 0x09: return L"健康";
    default:   return L"未知";
    }
}

static bool IsAudioCod(const u8 cod[3]) { return ((cod[1] >> 0) & 0x1F) == 0x04; }
static bool IsAudioDevice(ULONG cod) { return ((cod >> 8) & 0x1F) == 0x04; }

// ---------------------------------------------------------------- 认证回调（配对）

static BOOL CALLBACK AuthCallback(LPVOID, PBLUETOOTH_AUTHENTICATION_CALLBACK_PARAMS p)
{
    if (!p) return FALSE;

    BLUETOOTH_AUTHENTICATE_RESPONSE resp = {};
    resp.bthAddressRemote = p->deviceInfo.Address;
    resp.authMethod = p->authenticationMethod;
    resp.negativeResponse = 0;

    switch (p->authenticationMethod) {
    case BLUETOOTH_AUTHENTICATION_METHOD_NUMERIC_COMPARISON:
        resp.numericCompInfo.NumericValue = p->Numeric_Value;
        break;
    case BLUETOOTH_AUTHENTICATION_METHOD_PASSKEY_NOTIFICATION:
    case BLUETOOTH_AUTHENTICATION_METHOD_PASSKEY:
        resp.passkeyInfo.passkey = p->Passkey;
        break;
    case BLUETOOTH_AUTHENTICATION_METHOD_LEGACY:
        resp.pinInfo.pinLength = 4;
        resp.pinInfo.pin[0] = '0'; resp.pinInfo.pin[1] = '0';
        resp.pinInfo.pin[2] = '0'; resp.pinInfo.pin[3] = '0';
        break;
    default:
        break;
    }

    BluetoothSendAuthenticationResponseEx(g_authRadio, &resp);
    return TRUE;
}

// ---------------------------------------------------------------- 列表

static int SelIndex(HWND lv)
{
    return (int)SendMessageW(lv, LVM_GETNEXTITEM, -1, LVNI_SELECTED);
}

static void CloseAdapters()
{
    for (auto& a : g_adapters)
        if (a.kind == ADK_WINDOWS && a.hRadio != INVALID_HANDLE_VALUE)
            CloseHandle(a.hRadio);
    g_adapters.clear();
}

// ---- 枚举 WinUSB dongle ----
static void EnumWinusbAdapters()
{
    Adapter a;
    a.kind = ADK_WINUSB;
    a.name = L"用户态 (WinUSB)";

    // 扫描线程握着设备时 WinUSB 是独占的，此时探测必然失败，如实提示
    if (g_busy.load() || g_scanRun.load()) {
        a.addr = L"--:--:--:--:--:--";
        a.note = L"（正在扫描，暂无法探测地址）";
        g_adapters.push_back(a);
        return;
    }

    BtHci hci;
    u8 addr[6] = {};
    if (hci.Open() && hci.ReadBdAddr(addr)) {
        a.addr = AddrStr6(addr);
    } else {
        a.addr = L"--:--:--:--:--:--";
        a.note = L"（本次探测失败，点「刷新适配器」重试）";
    }
    g_adapters.push_back(a);
}

static void ScanStop();   // 前置声明：定义在后面

static void RefreshAdapters()
{
    // 先停掉可能占用 dongle 的后台扫描，否则 WinUSB 打不开
    if (g_scanRun.load()) {
        ScanStop();
        Sleep(300);
    }
    CloseAdapters();

    // 1) Windows 协议栈接纳的 radio
    BLUETOOTH_FIND_RADIO_PARAMS p = { sizeof(p) };
    HANDLE h = INVALID_HANDLE_VALUE;
    HBLUETOOTH_RADIO_FIND f = BluetoothFindFirstRadio(&p, &h);
    if (f) {
        do {
            Adapter a;
            a.kind = ADK_WINDOWS;
            a.hRadio = h;
            BLUETOOTH_RADIO_INFO ri = {};
            ri.dwSize = sizeof(ri);
            if (BluetoothGetRadioInfo(h, &ri) == ERROR_SUCCESS) {
                a.name = ri.szName;
                a.addr = AddrStr(ri.address);
            } else {
                a.name = L"Windows radio";
                a.addr = L"--:--:--:--:--:--";
            }
            g_adapters.push_back(a);
            h = INVALID_HANDLE_VALUE;
        } while (BluetoothFindNextRadio(f, &h));
        BluetoothFindRadioClose(f);
    }

    // 2) 用户态直接控制的 dongle（WinUSB）
    EnumWinusbAdapters();

    ListView_DeleteAllItems(g_lvRadios);
    for (size_t i = 0; i < g_adapters.size(); ++i) {
        const Adapter& a = g_adapters[i];
        std::wstring t = a.name + L"   " + a.addr + L"   " + a.note;
        LVITEMW it = {};
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = (int)i;
        it.pszText = (LPWSTR)t.c_str();
        it.lParam = (LPARAM)i;
        ListView_InsertItem(g_lvRadios, &it);
    }

    if (g_adapters.empty()) {
        PostStatus(L"没有枚举到任何适配器。");
    } else {
        int nwin = 0, nusr = 0;
        for (auto& a : g_adapters) (a.kind == ADK_WINDOWS ? nwin++ : nusr++);
        wchar_t buf[160];
        swprintf(buf, 160, L"共 %d 个适配器：Windows 协议栈 %d 个，用户态 WinUSB %d 个。",
                 (int)g_adapters.size(), nwin, nusr);
        PostStatus(buf);
        LVITEMW it = {};
        it.mask = LVIF_STATE;
        it.state = LVIS_SELECTED | LVIS_FOCUSED;
        it.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
        SendMessageW(g_lvRadios, LVM_SETITEMSTATE, 0, (LPARAM)&it);
    }
}

// ---------------------------------------------------------------- 设备列表渲染

static void FillDevList()
{
    std::lock_guard<std::mutex> lk(g_devsMx);
    ListView_DeleteAllItems(g_lvDevs);
    for (size_t i = 0; i < g_devs.size(); ++i) {
        const DevEntry& d = g_devs[i];
        LVITEMW it = {};
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = (int)i;
        it.pszText = (LPWSTR)d.name.c_str();
        it.lParam = (LPARAM)i;
        ListView_InsertItem(g_lvDevs, &it);
        ListView_SetItemText(g_lvDevs, (int)i, 1, (LPWSTR)d.addr.c_str());
        ListView_SetItemText(g_lvDevs, (int)i, 2, (LPWSTR)d.state.c_str());
    }
}

// ---------------------------------------------------------------- Windows 路径

static void RefreshDevicesWin(bool inquiry)
{
    std::vector<DevEntry> out;

    int ai = SelIndex(g_lvRadios);
    if (ai < 0 || ai >= (int)g_adapters.size() || g_adapters[ai].kind != ADK_WINDOWS) {
        g_devs = out;
        FillDevList();
        return;
    }

    HANDLE hRadio = g_adapters[ai].hRadio;

    BLUETOOTH_DEVICE_SEARCH_PARAMS sp = {};
    sp.dwSize = sizeof(sp);
    sp.fReturnAuthenticated = TRUE;
    sp.fReturnRemembered = TRUE;
    sp.fReturnUnknown = TRUE;
    sp.fReturnConnected = TRUE;
    sp.fIssueInquiry = inquiry ? TRUE : FALSE;
    sp.cTimeoutMultiplier = inquiry ? 8 : 0;
    sp.hRadio = hRadio;

    BLUETOOTH_DEVICE_INFO di = {};
    di.dwSize = sizeof(di);
    HBLUETOOTH_DEVICE_FIND f = BluetoothFindFirstDevice(&sp, &di);
    if (f) {
        do {
            BLUETOOTH_DEVICE_INFO cur = di;
            cur.dwSize = sizeof(cur);
            BluetoothGetDeviceInfo(hRadio, &cur);

            DevEntry d;
            d.isWin = true;
            d.adapterIdx = ai;
            d.wdi = cur;
            d.name = cur.szName;
            d.addr = AddrStr(cur.Address);
            d.state = cur.fConnected ? L"已连接" : L"未连接";
            d.state += cur.fAuthenticated ? L" · 已配对" : (cur.fRemembered ? L" · 已记住" : L" · 未配对");
            d.isAudio = IsAudioDevice(cur.ulClassofDevice);
            if (d.isAudio) d.state += L" · 音频设备";
            out.push_back(d);
            di.dwSize = sizeof(di);
        } while (BluetoothFindNextDevice(f, &di));
        BluetoothFindDeviceClose(f);
    }

    g_devs = out;
    FillDevList();

    wchar_t buf[128];
    swprintf(buf, 128, L"Windows 适配器下有 %u 个设备。", (unsigned)g_devs.size());
    PostStatus(buf);
}

// ---------------------------------------------------------------- WinUSB 路径

static void ScanWinusb(int ai)
{
    std::vector<DevEntry> out;
    std::wstring status;

    BtHci hci;
    if (!hci.Open()) {
        status = L"WinUSB 适配器打开失败（未绑定驱动）。";
    } else {
        std::vector<BtHci::ScanDev> found;
        std::wstring err;
        if (!hci.Scan(found, 10, err)) {
            status = L"扫描失败：" + err;
        } else {
            for (auto& s : found) {
                DevEntry d;
                d.name = s.name.empty() ? L"(未知名)" : Utf8ToWide(s.name);
                d.addr = AddrStr6(s.addr);
                d.isAudio = IsAudioCod(s.cod);
                d.state = std::wstring(MajorClass((ULONG)((s.cod[0] << 16) | (s.cod[1] << 8) | s.cod[2])));
                if (s.rssi) {
                    wchar_t rb[32];
                    swprintf(rb, 32, L" · RSSI %d", s.rssi);
                    d.state += rb;
                }
                if (d.isAudio) d.state += L" · 音频设备";
                out.push_back(d);
            }
            wchar_t buf[128];
            swprintf(buf, 128, L"用户态适配器扫描到 %u 个设备（不含已配对记录，纯空中发现）。",
                     (unsigned)out.size());
            status = buf;
        }
        hci.Close();
    }

    g_devs = out;
    FillDevList();
    PostStatus(status);
    if (g_bt) PostMessageW(g_bt, WM_BT_DEVS_READY, 0, 0);
}

// ---------------------------------------------------------------- Windows 已配对设备名字缓存

// 空中扫描拿不到设备名（芯片拒绝 Remote_Name_Request），但用户自己的耳机/手机
// 都在 Windows 里配对过，名字直接从这里补。
static std::map<std::wstring, std::wstring> g_winNames;

static void BuildWinNameCache()
{
    g_winNames.clear();
    BLUETOOTH_FIND_RADIO_PARAMS rp = { sizeof(rp) };
    HANDLE h = INVALID_HANDLE_VALUE;
    HBLUETOOTH_RADIO_FIND fr = BluetoothFindFirstRadio(&rp, &h);
    if (!fr) return;
    do {
        BLUETOOTH_DEVICE_SEARCH_PARAMS sp = {};
        sp.dwSize = sizeof(sp);
        sp.hRadio = h;
        sp.fReturnAuthenticated = TRUE;
        sp.fReturnRemembered = TRUE;
        sp.fReturnUnknown = FALSE;
        sp.fReturnConnected = TRUE;
        sp.fIssueInquiry = FALSE;

        BLUETOOTH_DEVICE_INFO di = {};
        di.dwSize = sizeof(di);
        HBLUETOOTH_DEVICE_FIND fd = BluetoothFindFirstDevice(&sp, &di);
        if (fd) {
            do {
                g_winNames[AddrStr(di.Address)] = di.szName;
                di.dwSize = sizeof(di);
            } while (BluetoothFindNextDevice(fd, &di));
            BluetoothFindDeviceClose(fd);
        }
    } while (BluetoothFindNextRadio(fr, &h));
    BluetoothFindRadioClose(fr);
}

static std::wstring LookupWinName(const std::wstring& addr)
{
    auto it = g_winNames.find(addr);
    return (it != g_winNames.end()) ? it->second : std::wstring();
}

// ---------------------------------------------------------------- 扫描日志（诊断用）

static void ScanLog(const wchar_t* fmt, ...)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, L"scan.log", L"a, ccs=UTF-8") != 0 || !f) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fwprintf(f, L"[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
    va_list ap;
    va_start(ap, fmt);
    vfwprintf(f, fmt, ap);
    va_end(ap);
    fputwc(L'\n', f);
    fclose(f);
}

// ---------------------------------------------------------------- 持续扫描

static void ScanStop()
{
    g_scanRun.store(false);
    if (g_btnScan) SetWindowTextW(g_btnScan, L"扫描设备");
}

static void ScanLoopWinusb(int ai)
{
    BtHci hci;
    std::map<std::wstring, DevEntry> acc;

    if (!hci.Open() || !hci.ScanBegin()) {
        PostStatus(L"用户态适配器打开/初始化失败，持续扫描未启动。");
        g_scanRun.store(false);
        if (g_btnScan) SetWindowTextW(g_btnScan, L"扫描设备");
        return;
    }

    PostStatus(L"持续扫描中（用户态适配器）—— 再点一次「停止扫描」结束。");

    BuildWinNameCache();   // 预先取好 Windows 侧已配对设备的名字
    ScanLog(L"=== 持续扫描开始，Windows 名字缓存 %u 条 ===", (unsigned)g_winNames.size());
    for (auto& kv : g_winNames) ScanLog(L"  cache: %s = %s", kv.first.c_str(), kv.second.c_str());
    ScanLog(L"=== 以上为缓存内容 ===");

    int rounds = 0;
    std::map<std::wstring, BtHci::ScanDev> meta;   // 各地址最近一次的寻呼参数

    while (g_scanRun.load()) {
        std::vector<BtHci::ScanDev> found;
        if (!hci.InquiryRound(found, 6, false)) break;
        ++rounds;

        // 1) 合并本轮发现
        std::vector<std::wstring> present;
        for (auto& s : found) {
            std::wstring key = AddrStr6(s.addr);
            meta[key] = s;
            auto it = acc.find(key);
            if (it == acc.end()) {
                // 新设备：先占位入列表
                DevEntry d;
                d.name = L"(识别中...)";
                d.addr = key;
                d.isAudio = IsAudioCod(s.cod);
                d.state = std::wstring(MajorClass((ULONG)((s.cod[0] << 16) | (s.cod[1] << 8) | s.cod[2])));
                if (s.rssi) {
                    wchar_t rb[32];
                    swprintf(rb, 32, L" · RSSI %d", s.rssi);
                    d.state += rb;
                }
                if (d.isAudio) d.state += L" · 音频设备";
                acc[key] = d;
                ScanLog(L"new device %s psrm=%d clk=0x%04X cod=%02X%02X%02X",
                        key.c_str(), s.psrm, s.clk, s.cod[0], s.cod[1], s.cod[2]);
            } else {
                // 老设备：只更新 RSSI，保留名字
                DevEntry& e = it->second;
                size_t pos = e.state.find(L" · RSSI");
                std::wstring base = (pos != std::wstring::npos) ? e.state.substr(0, pos) : e.state;
                wchar_t rb[32];
                swprintf(rb, 32, L" · RSSI %d", s.rssi);
                e.state = base + rb;
            }
            present.push_back(key);
        }

        // 2) 刷新列表
        {
            std::lock_guard<std::mutex> lk(g_devsMx);
            g_devs.clear();
            for (auto& kv : acc) g_devs.push_back(kv.second);
        }
        if (g_bt) PostMessageW(g_bt, WM_BT_DEVS_READY, 0, 0);

        // 3) 给还没名字的设备补名字（本轮在场、每轮最多 2 个）
        //    连接失败多半是对方当前不接受连接（如耳机配对模式超时），
        //    不做失败标记，下一轮会自动重试。
        int tried = 0;
        for (auto& key : present) {
            if (!g_scanRun.load()) break;
            auto it = acc.find(key);
            if (it == acc.end()) continue;
            if (it->second.name != L"(识别中...)" && it->second.name != L"(未知设备)") continue;
            if (tried >= 2) break;
            ++tried;

            const BtHci::ScanDev& s = meta[key];
            std::wstring wn;
            u16 hh = 0xFFFF;
            if (hci.ConnectAcl(s.addr, hh, 5000)) {
                ScanLog(L"  connected handle=0x%04X", hh);
                std::string nm;
                bool ok = hci.RemoteName(s.addr, s.psrm, s.clk, nm);
                if (ok) wn = Utf8ToWide(nm);
                ScanLog(L"  RemoteName ok=%d name=%s", (int)ok, wn.empty() ? L"(空)" : wn.c_str());
                hci.DisconnectAcl(hh);
            } else {
                ScanLog(L"  ConnectAcl 失败（对方当前不接受连接，下轮重试）");
            }
            if (wn.empty()) wn = LookupWinName(key);   // 协议失败退回 Windows 记录
            ScanLog(L"  名字: %s", wn.empty() ? L"(空)" : wn.c_str());

            it->second.name = wn.empty() ? L"(未知设备)" : wn;

            {
                std::lock_guard<std::mutex> lk(g_devsMx);
                g_devs.clear();
                for (auto& kv : acc) g_devs.push_back(kv.second);
            }
            if (g_bt) PostMessageW(g_bt, WM_BT_DEVS_READY, 0, 0);
        }

        wchar_t buf[160];
        swprintf(buf, 160, L"持续扫描中（第 %d 轮，累计 %u 个设备）—— 再点一次「停止扫描」结束。",
                 rounds, (unsigned)acc.size());
        PostStatus(buf);
    }

    hci.Close();
    if (g_btnScan) SetWindowTextW(g_btnScan, L"扫描设备");
    PostStatus(L"扫描已停止。");
}

static void ScanLoopWin(int ai)
{
    PostStatus(L"持续扫描中（Windows 适配器）—— 再点一次「停止扫描」结束。");
    while (g_scanRun.load()) {
        RefreshDevicesWin(true);
        Sleep(400);
    }
    if (g_btnScan) SetWindowTextW(g_btnScan, L"扫描设备");
    PostStatus(L"扫描已停止。");
}

// ---------------------------------------------------------------- 操作

static DevEntry* CurDev()
{
    int i = SelIndex(g_lvDevs);
    if (i < 0 || i >= (int)g_devs.size()) return nullptr;
    return &g_devs[i];
}

static int CurAdapterIdx()
{
    return SelIndex(g_lvRadios);
}

static bool CurIsWinusb()
{
    int i = CurAdapterIdx();
    return i >= 0 && i < (int)g_adapters.size() && g_adapters[i].kind == ADK_WINUSB;
}

static void SetService(bool enable)
{
    if (CurIsWinusb()) { PostStatus(L"用户态适配器不支持此操作（它不走 Windows 协议栈）。"); return; }

    DevEntry* d = CurDev();
    int ai = CurAdapterIdx();
    if (!d || !d->isWin || ai < 0) { PostStatus(L"请先选一个 Windows 适配器和设备。"); return; }
    HANDLE h = g_adapters[ai].hRadio;

    DWORD err = BluetoothSetServiceState(h, &d->wdi, &GUID_A2DP_SINK,
                                         enable ? BLUETOOTH_SERVICE_ENABLE : BLUETOOTH_SERVICE_DISABLE);
    if (err == ERROR_SUCCESS) {
        PostStatus(enable ? L"已请求连接音频服务（A2DP）。"
                          : L"已请求断开音频服务（A2DP）。");
    } else {
        err = BluetoothSetServiceState(h, &d->wdi, &GUID_HANDSFREE,
                                       enable ? BLUETOOTH_SERVICE_ENABLE : BLUETOOTH_SERVICE_DISABLE);
        if (err != ERROR_SUCCESS)
            err = BluetoothSetServiceState(h, &d->wdi, &GUID_HEADSET,
                                           enable ? BLUETOOTH_SERVICE_ENABLE : BLUETOOTH_SERVICE_DISABLE);
        wchar_t buf[128];
        if (err == ERROR_SUCCESS) {
            swprintf(buf, 128, L"已通过免提服务%s。", enable ? L"连接" : L"断开");
        } else {
            swprintf(buf, 128, L"操作失败，错误码 %lu。若设备未配对请先配对。", err);
        }
        PostStatus(buf);
    }
    RefreshDevicesWin(false);
}

// ---------------------------------------------------------------- 窗口

static HWND BtControl(const wchar_t* cls, const wchar_t* text, int id,
                      DWORD style, int x, int y, int w, int h, DWORD exStyle = 0)
{
    HWND c = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style,
                             x, y, w, h, g_bt, (HMENU)(INT_PTR)id, g_btInst, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_btFont, TRUE);
    return c;
}

static void BtCreateControls()
{
    const int x0 = 14, W = 592;

    BtControl(L"STATIC", L"适配器（Windows 协议栈 radio + 用户态 WinUSB dongle）", 0, 0, x0, 12, W, 18);
    g_lvRadios = BtControl(WC_LISTVIEWW, L"", ID_BT_RADIOS,
                           LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                           x0, 32, W, 84, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(g_lvRadios, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    LVCOLUMNW c = {};
    c.mask = LVCF_WIDTH | LVCF_TEXT;
    c.cx = W - 24;
    c.pszText = (LPWSTR)L"适配器";
    ListView_InsertColumn(g_lvRadios, 0, &c);

    BtControl(L"BUTTON", L"刷新适配器", ID_BT_REFRESH, 0, x0, 122, 110, 26);

    BtControl(L"STATIC", L"设备", 0, 0, x0, 160, W, 18);
    g_lvDevs = BtControl(WC_LISTVIEWW, L"", ID_BT_DEVS,
                         LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                         x0, 180, W, 200, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(g_lvDevs, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);
    const wchar_t* cols[3] = { L"名称", L"地址", L"状态" };
    int cw[3] = { 240, 150, 190 };
    for (int i = 0; i < 3; ++i) {
        LVCOLUMNW cc = {};
        cc.mask = LVCF_WIDTH | LVCF_TEXT | LVCF_SUBITEM;
        cc.cx = cw[i];
        cc.iSubItem = i;
        cc.pszText = (LPWSTR)cols[i];
        ListView_InsertColumn(g_lvDevs, i, &cc);
    }

    int bx = x0, by = 392;
    g_btnScan = BtControl(L"BUTTON", L"扫描设备", ID_BT_SCAN, 0, bx, by, 90, 26); bx += 98;
    BtControl(L"BUTTON", L"配对", ID_BT_PAIR, 0, bx, by, 70, 26); bx += 78;
    BtControl(L"BUTTON", L"连接音频", ID_BT_CONNECT, 0, bx, by, 90, 26); bx += 98;
    BtControl(L"BUTTON", L"断开", ID_BT_DISCONNECT, 0, bx, by, 70, 26); bx += 78;
    BtControl(L"BUTTON", L"取消配对", ID_BT_UNPAIR, 0, bx, by, 90, 26); bx += 98;
    BtControl(L"BUTTON", L"切换可发现", ID_BT_DISCOVERABLE, 0, bx, by, 100, 26);

    g_btStatus = BtControl(L"STATIC", L"", ID_BT_STATUS, 0, x0, 430, W, 40);
}

static LRESULT CALLBACK BtWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_bt = hwnd;
        BtCreateControls();
        return 0;

    case WM_BT_STATUS: {
        std::wstring* p = (std::wstring*)wp;
        if (p) { SetWindowTextW(g_btStatus, p->c_str()); delete p; }
        return 0;
    }

    case WM_BT_REFRESHED:
        RefreshDevicesWin((BOOL)wp);
        return 0;

    case WM_BT_DEVS_READY:
        FillDevList();
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_BT_REFRESH:
            RefreshAdapters();
            g_devs.clear();
            FillDevList();
            break;

        case ID_BT_SCAN: {
            // 再点一次 = 停止
            if (g_scanRun.exchange(false)) {
                if (g_btnScan) SetWindowTextW(g_btnScan, L"扫描设备");
                PostStatus(L"已请求停止扫描，当前这一轮结束后生效。");
                break;
            }
            if (g_busy.exchange(true)) { PostStatus(L"上一个操作还在进行中。"); break; }
            int ai = CurAdapterIdx();
            if (ai < 0) { PostStatus(L"请先选一个适配器。"); g_busy.store(false); break; }

            g_scanRun.store(true);
            if (g_btnScan) SetWindowTextW(g_btnScan, L"停止扫描");

            if (g_adapters[ai].kind == ADK_WINUSB)
                std::thread([ai]() { ScanLoopWinusb(ai); g_busy.store(false); }).detach();
            else
                std::thread([ai]() { ScanLoopWin(ai);   g_busy.store(false); }).detach();
            break;
        }

        case ID_BT_PAIR: {
            if (CurIsWinusb()) { PostStatus(L"用户态适配器不支持配对操作。"); break; }
            DevEntry* d = CurDev();
            int ai = CurAdapterIdx();
            if (!d || !d->isWin || ai < 0) { PostStatus(L"请先选一个 Windows 适配器和设备。"); break; }
            if (g_busy.exchange(true)) { PostStatus(L"上一个操作还在进行中。"); break; }

            BLUETOOTH_DEVICE_INFO copy = d->wdi;
            HANDLE h = g_adapters[ai].hRadio;
            g_authRadio = h;
            wchar_t buf[160];
            swprintf(buf, 160, L"正在与 %s 配对...", copy.szName);
            PostStatus(buf);

            std::thread([copy, h]() {
                BLUETOOTH_DEVICE_INFO di = copy;
                DWORD err = BluetoothAuthenticateDeviceEx(nullptr, h, &di, nullptr,
                                                          MITMProtectionNotRequiredBonding);
                wchar_t b[200];
                if (err == ERROR_SUCCESS)
                    swprintf(b, 200, L"配对成功：%s", di.szName);
                else
                    swprintf(b, 200, L"配对失败，错误码 %lu（常见原因：设备未进入配对模式）", err);
                PostStatus(b);
                g_busy.store(false);
                if (g_bt) PostMessageW(g_bt, WM_BT_REFRESHED, 0, 0);
            }).detach();
            break;
        }

        case ID_BT_CONNECT:      SetService(true);  break;
        case ID_BT_DISCONNECT:   SetService(false); break;

        case ID_BT_UNPAIR: {
            if (CurIsWinusb()) { PostStatus(L"用户态适配器不支持此操作。"); break; }
            DevEntry* d = CurDev();
            if (!d || !d->isWin) { PostStatus(L"请先选一个 Windows 适配器的设备。"); break; }
            BLUETOOTH_ADDRESS a = d->wdi.Address;
            DWORD err = BluetoothRemoveDevice(&a);
            wchar_t b[160];
            if (err == ERROR_SUCCESS) swprintf(b, 160, L"已取消配对。");
            else swprintf(b, 160, L"取消配对失败，错误码 %lu。", err);
            PostStatus(b);
            RefreshDevicesWin(false);
            break;
        }

        case ID_BT_DISCOVERABLE: {
            if (CurIsWinusb()) { PostStatus(L"用户态适配器不支持此操作（用 HCI 命令控制）。"); break; }
            int ai = CurAdapterIdx();
            if (ai < 0) { PostStatus(L"请先选一个适配器。"); break; }
            HANDLE h = g_adapters[ai].hRadio;
            BOOL cur = BluetoothIsDiscoverable(h);
            BOOL ok = BluetoothEnableDiscovery(h, !cur);
            PostStatus(ok ? (!cur ? L"本设备已设为可被发现。" : L"已停止被其他设备发现。")
                          : L"切换可发现状态失败。");
            break;
        }
        }
        return 0;

    case WM_NOTIFY: {
        NMHDR* nm = (NMHDR*)lp;
        if (nm->idFrom == ID_BT_RADIOS && nm->code == LVN_ITEMCHANGED) {
            NM_LISTVIEW* lv = (NM_LISTVIEW*)lp;
            if (lv->iItem >= 0 && (lv->uNewState & LVIS_SELECTED)) {
                if (g_scanRun.load()) { ScanStop(); PostStatus(L"切换适配器，已停止扫描。"); }
                int i = SelIndex(g_lvRadios);
                if (i >= 0 && i < (int)g_adapters.size() && g_adapters[i].kind == ADK_WINUSB)
                    PostStatus(L"用户态适配器：点「扫描设备」做空中扫描。配对/连接需走 Windows 适配器。");
                g_devs.clear();
                FillDevList();
            }
        }
        return 0;
    }

    case WM_DESTROY:
        g_scanRun.store(false);
        if (g_authReg) { BluetoothUnregisterAuthentication(g_authReg); g_authReg = nullptr; }
        CloseAdapters();
        g_bt = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ShowBtPanel(HWND parent)
{
    if (g_bt) { SetForegroundWindow(g_bt); return; }

    g_btInst = GetModuleHandleW(nullptr);

    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = BtWndProc;
        wc.hInstance = g_btInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"DualBltBtPanel";
        if (!RegisterClassW(&wc)) return;
        registered = true;
    }

    g_btFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    g_bt = CreateWindowExW(0, L"DualBltBtPanel", L"蓝牙适配器",
                           WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                           CW_USEDEFAULT, CW_USEDEFAULT, 636, 520,
                           parent, nullptr, g_btInst, nullptr);
    if (!g_bt) return;

    if (!g_authReg) {
        BluetoothRegisterForAuthenticationEx(nullptr, &g_authReg, AuthCallback, nullptr);
    }

    ShowWindow(g_bt, SW_SHOW);
    UpdateWindow(g_bt);
    RefreshAdapters();
}
