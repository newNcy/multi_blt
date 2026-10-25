// DualBlt 蓝牙推流面板 —— WebView2 版（flap.sh 风格 HTML UI）
// C++ 引擎（btsvc/loopcap/adapter）与 btpanel2 相同，界面换成 WebView2 加载
// assets/ui.html；C++<->JS 通过 WebMessage 桥：
//   JS -> C++ : 简单键值串  "c=connect&addr=XX:XX:..."
//   C++ -> JS : 每 500ms PostWebMessageAsJson 全量状态
#define UNICODE

#include "btpanel3.h"
#include "btsvc.h"
#include "loopcap.h"
#include "adapter.h"

#include <windows.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <string>
#include <map>
#include <vector>
#include <functional>

#include <WebView2.h>
#include <wrl/client.h>
#include <wrl/event.h>

using namespace Microsoft::WRL;

static ComPtr<ICoreWebView2Controller> g_ctrl;

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ole32.lib")

#define WM_A2_STATUS (WM_APP + 30)   // wParam = HeapAlloc 的 UTF-16 文本（btsvc Post 回调）
#define WM_A2_DEVICE (WM_APP + 31)   // wParam = HeapAlloc 的 addr\0name\0cod 包

// ------------------------------------------------------------ 工具

static std::string ToUtf8(const wchar_t* w)
{
    std::string out;
    if (!w || !*w) return out;
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    out.resize(n - 1);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], n, nullptr, nullptr);
    return out;
}

static std::wstring Utf8ToWideW(const std::string& s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out(n ? n - 1 : 0, 0);
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &out[0], n);
    return out;
}

static std::wstring AppFileW(const wchar_t* name)
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    size_t p = dir.find_last_of(L"\\/");
    return dir.substr(0, p + 1) + name;
}

static std::wstring JsonEscape(const std::wstring& s)
{
    std::wstring o;
    for (wchar_t ch : s) {
        switch (ch) {
        case L'"':  o += L"\\\""; break;
        case L'\\': o += L"\\\\"; break;
        default:
            if (ch < 0x20) o += L' ';
            else o += ch;
        }
    }
    return o;
}

// 启动诊断日志（定位崩溃点用）
static void WVLog(const wchar_t* line)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, AppFileW(L"wv2.log").c_str(), L"a, ccs=UTF-8") != 0 || !f) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fwprintf(f, L"[%02d:%02d:%02d.%03d] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, line);
    fclose(f);
}

// 崩溃时记录异常码 + 栈回溯地址（配合 /MAP 文件定位符号）
static LONG WINAPI WVCrashHandler(EXCEPTION_POINTERS* ep)
{
    void* stack[24];
    USHORT n = CaptureStackBackTrace(0, 24, stack, nullptr);
    HMODULE base = GetModuleHandleW(nullptr);
    FILE* f = nullptr;
    if (_wfopen_s(&f, AppFileW(L"wv2.log").c_str(), L"a, ccs=UTF-8") == 0 && f) {
        SYSTEMTIME st; GetLocalTime(&st);
        fwprintf(f, L"[%02d:%02d:%02d] CRASH code=0x%08X addr=%p base=%p frames=%u\n",
                 st.wHour, st.wMinute, st.wSecond,
                 ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress, base, n);
        for (USHORT i = 0; i < n; ++i) fwprintf(f, L"  #%02u %p (rva 0x%llX)\n", i, stack[i],
                 (unsigned long long)((uintptr_t)stack[i] - (uintptr_t)base));
        fclose(f);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

// "c=connect&addr=XX&i=2" -> 取参数值（URL 解码 %XX）
static std::wstring GetParam(const std::wstring& msg, const wchar_t* key)
{
    size_t pos = 0;
    while (true) {
        pos = msg.find(key, pos);
        if (pos == std::wstring::npos) return L"";
        size_t ke = pos + wcslen(key);
        bool startOk = (pos == 0 || msg[pos - 1] == L'&');
        if (startOk && ke < msg.size() && msg[ke] == L'=') {
            std::wstring v;
            size_t i = ke + 1;
            while (i < msg.size() && msg[i] != L'&') {
                if (msg[i] == L'%' && i + 2 < msg.size()) {
                    wchar_t hex[3] = { msg[i + 1], msg[i + 2], 0 };
                    v += (wchar_t)wcstoul(hex, nullptr, 16);
                    i += 3;
                } else v += msg[i++];
            }
            return v;
        }
        pos = ke;
    }
}

// ------------------------------------------------------------ 数据

struct DevRec {
    std::wstring addr;
    std::string  addrUtf8;
    std::string  nameUtf8;
    unsigned     cod = 0;
    std::string  statusUtf8 = "未配对";
    bool         statusStreaming = false;
};

static HWND g_hwnd = nullptr;
static ComPtr<ICoreWebView2> g_web;
static std::map<std::wstring, DevRec> g_devs;
static std::vector<BtAdapter> g_adapters;
static int g_adapterSel = -1;
static std::vector<LoopDevice> g_srcDevs;
static int g_srcSel = 0;
static float g_volUi = 100.0f;
static bool g_volMute = false;
static DWORD g_lastVolumeEdit = 0;
static std::wstring g_status, g_detail;

static void PushState();   // fwd

// btsvc 状态回调：投递到 UI 线程
static void StatusCb(const wchar_t* msg, void* user)
{
    HWND h = (HWND)user;
    if (!h) return;
    size_t n = (wcslen(msg) + 1) * sizeof(wchar_t);
    wchar_t* copy = (wchar_t*)HeapAlloc(GetProcessHeap(), 0, n);
    if (!copy) return;
    wcscpy_s(copy, wcslen(msg) + 1, msg);
    if (!PostMessageW(h, WM_A2_STATUS, (WPARAM)copy, 0)) HeapFree(GetProcessHeap(), 0, copy);
}

// btsvc 设备发现回调：投递到 UI 线程（addr\0name\0cod）
static void DeviceCb(const char* addr, const char* name, unsigned cod, void* user)
{
    HWND h = (HWND)user;
    if (!h) return;
    std::wstring a = Utf8ToWideW(addr), n = Utf8ToWideW(name ? name : "");
    size_t need = (a.size() + 1 + n.size() + 1 + 2) * sizeof(wchar_t);
    wchar_t* buf = (wchar_t*)HeapAlloc(GetProcessHeap(), 0, need);
    if (!buf) return;
    wcscpy_s(buf, a.size() + 1, a.c_str());
    wcscpy_s(buf + a.size() + 1, n.size() + 1, n.c_str());
    buf[a.size() + 1 + n.size() + 1] = (wchar_t)cod;
    if (!PostMessageW(h, WM_A2_DEVICE, (WPARAM)buf, 0)) HeapFree(GetProcessHeap(), 0, buf);
}

// ------------------------------------------------------------ 配对名单持久化

static void SavePairedName(const std::wstring& addr, const std::wstring& name)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, AppFileW(L"paired_names.txt").c_str(), L"a, ccs=UTF-8") != 0 || !f) return;
    fwprintf(f, L"%s\t%s\n", addr.c_str(), name.c_str());
    fclose(f);
}

static void LoadPairedDevices()
{
    std::map<std::wstring, std::wstring> names;
    FILE* nf = nullptr;
    if (_wfopen_s(&nf, AppFileW(L"paired_names.txt").c_str(), L"r, ccs=UTF-8") == 0 && nf) {
        wchar_t line[256];
        while (fgetws(line, 256, nf)) {
            wchar_t* tab = wcschr(line, L'\t');
            if (!tab) continue;
            *tab = 0;
            wchar_t* e = wcschr(tab + 1, L'\n');
            if (e) *e = 0;
            names[line] = tab + 1;
        }
        fclose(nf);
    }
    FILE* kf = nullptr;
    if (_wfopen_s(&kf, AppFileW(L"linkkeys.txt").c_str(), L"r") != 0 || !kf) return;
    char line[256];
    while (fgets(line, 256, kf)) {
        char a[32] = "";
        if (sscanf_s(line, "%31s", a, (unsigned)sizeof(a)) != 1) continue;
        if (!a[0]) continue;
        wchar_t waddr[32];
        MultiByteToWideChar(CP_ACP, 0, a, -1, waddr, 32);
        std::wstring key = waddr;
        if (g_devs.count(key)) continue;

        DevRec d;
        d.addr = key;
        d.addrUtf8 = ToUtf8(waddr);
        auto itn = names.find(key);
        std::wstring nm = (itn != names.end()) ? itn->second : L"(已配对设备)";
        d.nameUtf8 = ToUtf8(nm.c_str());
        d.cod = 0x240404;
        d.statusUtf8 = "已配对";
        g_devs[key] = d;
    }
    fclose(kf);
}

// ------------------------------------------------------------ 音量

static std::wstring CurrentSrcId()
{
    if (g_srcDevs.empty()) return std::wstring();
    if (g_srcSel < 0 || g_srcSel >= (int)g_srcDevs.size()) g_srcSel = 0;
    return g_srcDevs[g_srcSel].id;
}

static bool EndpointVolGet(const std::wstring& id, float& vol, bool& muted)
{
    IMMDeviceEnumerator* de = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&de))) return false;
    IMMDevice* dev = nullptr;
    bool ok = id.empty()
        ? SUCCEEDED(de->GetDefaultAudioEndpoint(eRender, eMultimedia, &dev))
        : SUCCEEDED(de->GetDevice(id.c_str(), &dev));
    de->Release();
    if (!ok || !dev) return false;
    IAudioEndpointVolume* ev = nullptr;
    ok = SUCCEEDED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)&ev));
    dev->Release();
    if (!ok || !ev) return false;
    float v = 0; BOOL m = FALSE;
    ok = SUCCEEDED(ev->GetMasterVolumeLevelScalar(&v)) && SUCCEEDED(ev->GetMute(&m));
    if (ok) { vol = v; muted = (m != FALSE); }
    ev->Release();
    return ok;
}

static bool EndpointVolSet(const std::wstring& id, float vol, bool muted)
{
    IMMDeviceEnumerator* de = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&de))) return false;
    IMMDevice* dev = nullptr;
    bool ok = id.empty()
        ? SUCCEEDED(de->GetDefaultAudioEndpoint(eRender, eMultimedia, &dev))
        : SUCCEEDED(de->GetDevice(id.c_str(), &dev));
    de->Release();
    if (!ok || !dev) return false;
    IAudioEndpointVolume* ev = nullptr;
    ok = SUCCEEDED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)&ev));
    dev->Release();
    if (!ok || !ev) return false;
    ev->SetMasterVolumeLevelScalar(vol, nullptr);
    ev->SetMute(muted ? TRUE : FALSE, nullptr);
    ev->Release();
    return true;
}

static void VolumeLoadUI()
{
    float v = 0; bool m = false;
    if (EndpointVolGet(CurrentSrcId(), v, m)) {
        g_volUi = v * 100.0f;
        g_volMute = m;
    }
    BtSvcSetGain(g_volMute ? 0.0f : g_volUi / 100.0f);
}

static void VolumeApplyUI()
{
    BtSvcSetGain(g_volMute ? 0.0f : g_volUi / 100.0f);
    EndpointVolSet(CurrentSrcId(), g_volUi / 100.0f, g_volMute);
    g_lastVolumeEdit = GetTickCount();
}

static void VolumeSyncFromSystem()
{
    if (GetTickCount() - g_lastVolumeEdit < 700) return;
    float v = 0; bool m = false;
    if (!EndpointVolGet(CurrentSrcId(), v, m)) return;
    g_volUi = v * 100.0f;
    g_volMute = m;
    BtSvcSetGain(g_volMute ? 0.0f : g_volUi / 100.0f);
}

// ------------------------------------------------------------ 设备操作

static void ConnectDev(const DevRec& d)
{
    const char* cur = BtSvcCurrentAddr();
    if (cur[0] && _stricmp(cur, d.addrUtf8.c_str()) == 0 && BtSvcPhase() == 3) {
        g_detail = L"该设备已在推流中，无需重复连接。";
        return;
    }
    if (!BtSvcThreadActive()) {
        if (!BtSvcStart(nullptr, StatusCb, DeviceCb, g_hwnd)) {
            g_detail = L"启动失败。";
            return;
        }
    }
    BtSvcConnect(d.addrUtf8.c_str());
}

static void PairDev(const DevRec& d)
{
    if (!BtSvcThreadActive()) {
        if (!BtSvcStart(nullptr, StatusCb, DeviceCb, g_hwnd)) return;
    }
    if (!BtSvcIsPaired(d.addrUtf8.c_str()))
        SavePairedName(d.addr, d.nameUtf8.empty() ? L"(已配对设备)" : Utf8ToWideW(d.nameUtf8));
    BtSvcPair(d.addrUtf8.c_str());
}

static void ForgetDev(const DevRec& d)
{
    BtSvcDisconnect();
    BtSvcForgetDevice(d.addrUtf8.c_str());
    g_devs.erase(d.addr);
    FILE* f = nullptr;
    if (_wfopen_s(&f, AppFileW(L"paired_names.txt").c_str(), L"r, ccs=UTF-8") == 0 && f) {
        std::vector<std::wstring> lines;
        wchar_t line[256];
        while (fgetws(line, 256, f))
            if (wcsncmp(line, d.addr.c_str(), d.addr.size()) != 0 || line[d.addr.size()] != L'\t')
                lines.push_back(line);
        fclose(f);
        if (_wfopen_s(&f, AppFileW(L"paired_names.txt").c_str(), L"w, ccs=UTF-8") == 0 && f) {
            for (auto& l : lines) fputws(l.c_str(), f);
            fclose(f);
        }
    }
}

static void ScanToggle()
{
    if (BtSvcThreadActive() && BtSvcScanActive()) {
        BtSvcScanStop();
        g_status = L"已停止扫描。";
        return;
    }
    if (!BtSvcThreadActive()) {
        if (!BtSvcStart(nullptr, StatusCb, DeviceCb, g_hwnd)) {
            g_detail = L"启动失败。";
            return;
        }
    }
    for (auto it = g_devs.begin(); it != g_devs.end();) {
        if (BtSvcIsPaired(it->second.addrUtf8.c_str())) ++it;
        else it = g_devs.erase(it);
    }
    BtSvcScanStart();
}

static void CopyToggle()
{
    if (LoopCaptureRunning()) {
        LoopCaptureStop();
        BtSvcSetTestTone(false);
        g_detail = L"已停止复制声音。";
    } else {
        BtSvcSetTestTone(false);
        std::wstring err;
        if (LoopCaptureStart(CurrentSrcId(), err)) {
            g_detail = L"正在复制系统声音到蓝牙（在电脑上播放音乐即可听到）。";
        } else {
            g_detail = L"回环采集失败：" + err;
        }
    }
}

// 切换驱动（BTHUSB -> WinUSB），管理员执行签名 + pnputil
static void SwitchDriver(int idx)
{
    if (idx < 0 || idx >= (int)g_adapters.size()) return;
    BtAdapter& a = g_adapters[idx];
    if (a.winusb) return;
    std::wstring infPath;
    if (!AdapterWriteInf(a.vid, a.pid, infPath)) {
        g_detail = L"生成 INF 失败（缺 DualBltWinUSB.inf 模板）。";
        return;
    }
    BtSvcStop();
    std::wstring dir = infPath.substr(0, infPath.find_last_of(L'\\'));
    std::wstring ps1 = dir + L"\\switch.ps1";
    std::wstring log = dir + L"\\switch_log.txt";
    std::wstring pfx = AppFileW(L"winusb_cert.pfx");
    FILE* f = nullptr;
    if (_wfopen_s(&f, ps1.c_str(), L"w, ccs=UTF-8") == 0 && f) {
        fwprintf(f, L"$ErrorActionPreference='Continue'\n");
        fwprintf(f, L"Start-Transcript -Path '%s' -Force\n", log.c_str());
        fwprintf(f, L"$dir='%s'\n", dir.c_str());
        fwprintf(f, L"$pfx='%s'\n", pfx.c_str());
        fwprintf(f, L"$pwd2=ConvertTo-SecureString 'DualBlt123' -AsPlainText -Force\n");
        fwprintf(f, L"if (-not (Get-ChildItem Cert:\\LocalMachine\\My | Where-Object {$_.Subject -eq 'CN=DualBlt Test Signing'})) {\n");
        fwprintf(f, L"  [void](Import-PfxCertificate -FilePath $pfx -CertStoreLocation Cert:\\LocalMachine\\My -Password $pwd2)\n");
        fwprintf(f, L"}\n");
        fwprintf(f, L"$cat=Join-Path $dir 'DualBltWinUSB.cat'\n");
        fwprintf(f, L"Remove-Item $cat -Force -ErrorAction SilentlyContinue\n");
        fwprintf(f, L"New-FileCatalog -Path $dir -CatalogFilePath $cat | Out-Null\n");
        fwprintf(f, L"$st = Get-ChildItem 'C:\\Program Files (x86)\\Windows Kits\\10\\bin' -Recurse -Filter signtool.exe -ErrorAction SilentlyContinue | Where-Object {$_.FullName -match '\\\\x64\\\\'} | Sort-Object FullName -Descending | Select-Object -First 1\n");
        fwprintf(f, L"if ($st) { & $st.FullName sign /f $pfx /p 'DualBlt123' /fd SHA256 $cat } else { Write-Output 'signtool missing' }\n");
        fwprintf(f, L"$inf = Join-Path $dir 'DualBltWinUSB.inf'\n");
        fwprintf(f, L"pnputil /add-driver $inf /install\n");
        fwprintf(f, L"pnputil /remove-device '%s'\n", a.instanceId.c_str());
        fwprintf(f, L"pnputil /scan-devices\n");
        fwprintf(f, L"Stop-Transcript\n");
        fclose(f);
        wchar_t params[512];
        swprintf_s(params, 512, L"-NoProfile -ExecutionPolicy Bypass -File \"%s\"", ps1.c_str());
        SHELLEXECUTEINFOW sx = {};
        sx.cbSize = sizeof(sx);
        sx.lpFile = L"powershell.exe";
        sx.lpParameters = params;
        sx.lpVerb = L"runas";
        sx.nShow = SW_HIDE;
        sx.fMask = SEE_MASK_NOCLOSEPROCESS;
        if (ShellExecuteExW(&sx) && sx.hProcess) {
            WaitForSingleObject(sx.hProcess, 120000);
            CloseHandle(sx.hProcess);
        }
        unsigned cv = 0, cp = 0;
        BtSvcGetUsbDevice(cv, cp);
        g_adapters = AdapterEnumAll((uint16_t)cv, (uint16_t)cp);
        g_detail = L"驱动切换完成，确认适配器状态后重新连接设备。";
    }
}

// ------------------------------------------------------------ 状态刷新 + 推送

static void UpdateDeviceStatuses()
{
    const char* cur = BtSvcCurrentAddr();
    int phase = BtSvcThreadActive() ? BtSvcPhase() : -1;
    for (auto& kv : g_devs) {
        DevRec& d = kv.second;
        const char* st = "未配对";
        bool streaming = false;
        if (cur[0] && _stricmp(d.addrUtf8.c_str(), cur) == 0) {
            if (phase == 3)      { st = "推流中"; streaming = true; }
            else if (phase == 2) st = "连接中";
            else                 st = "已连接";
        } else if (BtSvcIsPaired(d.addrUtf8.c_str())) {
            st = "已配对";
        }
        d.statusUtf8 = st;
        d.statusStreaming = streaming;
    }
}

static void PushState()
{
    int phase = BtSvcThreadActive() ? BtSvcPhase() : -1;
    std::wstring j = L"{";
    j += L"\"phase\":" + std::to_wstring(phase) + L",";
    j += L"\"status\":\"" + JsonEscape(g_status) + L"\",";
    j += L"\"detail\":\"" + JsonEscape(g_detail) + L"\",";
    j += L"\"capOn\":" + std::wstring(LoopCaptureRunning() ? L"true" : L"false") + L",";
    j += L"\"testTone\":" + std::wstring(BtSvcTestTone() ? L"true" : L"false") + L",";
    j += L"\"vol\":" + std::to_wstring((int)(g_volUi + 0.5f)) + L",";
    j += L"\"mute\":" + std::wstring(g_volMute ? L"true" : L"false") + L",";
    j += L"\"adapterSel\":" + std::to_wstring(g_adapterSel) + L",";

    j += L"\"adapters\":[";
    for (size_t i = 0; i < g_adapters.size(); ++i) {
        if (i) j += L",";
        wchar_t vid[8], pid[8];
        swprintf_s(vid, 8, L"%04X", g_adapters[i].vid);
        swprintf_s(pid, 8, L"%04X", g_adapters[i].pid);
        j += L"{\"name\":\"" + JsonEscape(g_adapters[i].name) + L"\",";
        j += L"\"vid\":\"" + std::wstring(vid) + L"\",\"pid\":\"" + std::wstring(pid) + L"\",";
        j += L"\"winusb\":" + std::wstring(g_adapters[i].winusb ? L"true" : L"false") + L"}";
    }
    j += L"],";

    j += L"\"srcSel\":" + std::to_wstring(g_srcSel) + L",";
    j += L"\"srcs\":[";
    for (size_t i = 0; i < g_srcDevs.size(); ++i) {
        if (i) j += L",";
        j += L"\"" + JsonEscape(g_srcDevs[i].name) + L"\"";
    }
    j += L"],";

    const char* curAddr = BtSvcCurrentAddr();
    j += L"\"cur\":\"" + JsonEscape(Utf8ToWideW(curAddr)) + L"\",";
    j += L"\"devs\":[";
    bool first = true;
    for (auto& kv : g_devs) {
        DevRec& d = kv.second;
        if (!first) j += L",";
        first = false;
        bool hp = false;
        if ((d.cod >> 8 & 0x1F) == 0x04) {
            unsigned minor = (d.cod >> 2) & 0x3F;
            hp = (minor == 0x01 || minor == 0x02 || minor == 0x07);
        }
        bool cur = curAddr[0] && _stricmp(d.addrUtf8.c_str(), curAddr) == 0;
        j += L"{\"addr\":\"" + JsonEscape(d.addr) + L"\",";
        j += L"\"name\":\"" + JsonEscape(Utf8ToWideW(d.nameUtf8)) + L"\",";
        j += L"\"status\":\"" + JsonEscape(Utf8ToWideW(d.statusUtf8)) + L"\",";
        j += L"\"streaming\":" + std::wstring(d.statusStreaming ? L"true" : L"false") + L",";
        j += L"\"cur\":" + std::wstring(cur ? L"true" : L"false") + L",";
        j += L"\"hp\":" + std::wstring(hp ? L"true" : L"false") + L"}";
    }
    j += L"]}";
    if (g_web) g_web->PostWebMessageAsJson(j.c_str());
}

static void HandleWebMessage(const wchar_t* msg)
{
    std::wstring c = GetParam(msg, L"c=");
    if (c == L"scan")          ScanToggle();
    else if (c == L"refresh")  {
        unsigned cv = 0, cp = 0;
        BtSvcGetUsbDevice(cv, cp);
        g_adapters = AdapterEnumAll((uint16_t)cv, (uint16_t)cp);
    }
    else if (c == L"driver")   SwitchDriver(_wtoi(GetParam(msg, L"i=").c_str()));
    else if (c == L"adap") {
        int i = _wtoi(GetParam(msg, L"i=").c_str());
        BtSvcStop();
        if (i >= 0 && i < (int)g_adapters.size()) {
            g_adapterSel = i;
            BtSvcSetUsbDevice(g_adapters[i].vid, g_adapters[i].pid);
            g_detail.clear();
        }
    }
    else if (c == L"src") {
        int i = _wtoi(GetParam(msg, L"i=").c_str());
        if (i >= 0 && i < (int)g_srcDevs.size()) { g_srcSel = i; VolumeLoadUI(); }
    }
    else if (c == L"copy")     CopyToggle();
    else if (c == L"tone")     BtSvcSetTestTone(!BtSvcTestTone());
    else if (c == L"disconnect") BtSvcDisconnect();
    else if (c == L"vol") {
        float v = (float)_wtof(GetParam(msg, L"v=").c_str());
        g_volUi = v * 100.0f;
        VolumeApplyUI();
    }
    else if (c == L"mute") {
        g_volMute = _wtoi(GetParam(msg, L"v=").c_str()) != 0;
        VolumeApplyUI();
    }
    else if (c == L"connect" || c == L"pair" || c == L"forget") {
        std::wstring addr = GetParam(msg, L"addr=");
        if (!addr.empty()) {
            auto it = g_devs.find(addr);
            DevRec d;
            if (it != g_devs.end()) d = it->second;
            else { d.addr = addr; d.addrUtf8 = ToUtf8(addr.c_str()); d.nameUtf8 = "(unknown)"; }
            if (c == L"connect")      ConnectDev(d);
            else if (c == L"pair")    PairDev(d);
            else                      ForgetDev(d);
        }
    }
}

// ------------------------------------------------------------ WebView2 初始化

static bool InitWebView(HWND hwnd)
{
    WVLog(L"InitWebView: enter");
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    size_t p = dir.find_last_of(L"\\/");
    dir = dir.substr(0, p + 1);
    // 用户数据目录放本地系统盘：G 盘是可移动磁盘，Chromium 在其上做 I/O 可能挂起
    std::wstring userData;
    {
        wchar_t* lad = nullptr;
        char ladb[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, ladb)))
            userData = std::wstring(ladb) + L"\\DualBlt\\WebView2Data";
        else
            userData = dir + L"WebView2Data";
    }
    std::wstring htmlUrl = L"file:///" + dir + L"ui.html";
    for (auto& ch : htmlUrl) if (ch == L'\\') ch = L'/';

    auto envDone = Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
        [hwnd, htmlUrl](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
        WVLog(L"env callback: enter");
        if (FAILED(hr) || !env) { WVLog(L"env callback: FAILED"); return hr; }
        HRESULT hrC = env->CreateCoreWebView2Controller(hwnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [hwnd, htmlUrl](HRESULT hr2, ICoreWebView2Controller* ctrl) -> HRESULT {
            WVLog(L"ctrl callback: enter");
            if (FAILED(hr2) || !ctrl) { WVLog(L"ctrl callback: FAILED"); return hr2; }
            g_ctrl = ctrl;
            ComPtr<ICoreWebView2> web;
            ctrl->get_CoreWebView2(&web);
            g_web = web;

            ComPtr<ICoreWebView2Settings> settings;
            web->get_Settings(&settings);
            if (settings) {
                settings->put_AreDefaultContextMenusEnabled(FALSE);
                settings->put_AreDevToolsEnabled(FALSE);
                settings->put_IsStatusBarEnabled(FALSE);
                settings->put_IsZoomControlEnabled(FALSE);
            }

            // JS -> C++
            web->add_WebMessageReceived(
                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                LPWSTR raw = nullptr;
                if (SUCCEEDED(args->TryGetWebMessageAsString(&raw)) && raw) {
                    HandleWebMessage(raw);
                    CoTaskMemFree(raw);
                }
                return S_OK;
            }).Get(), nullptr);

            RECT rc;
            GetClientRect(hwnd, &rc);
            ctrl->put_Bounds(rc);
            ctrl->put_IsVisible(TRUE);

            // 导航结果诊断
            web->add_NavigationStarting(
                Callback<ICoreWebView2NavigationStartingEventHandler>(
                [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                LPWSTR u = nullptr;
                if (SUCCEEDED(args->get_Uri(&u)) && u) {
                    WVLog((std::wstring(L"nav start: ") + u).c_str());
                    CoTaskMemFree(u);
                }
                return S_OK;
            }).Get(), nullptr);
            web->add_NavigationCompleted(
                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                [](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                BOOL ok = FALSE;
                COREWEBVIEW2_WEB_ERROR_STATUS err = (COREWEBVIEW2_WEB_ERROR_STATUS)0;
                args->get_IsSuccess(&ok);
                args->get_WebErrorStatus(&err);
                wchar_t b[96];
                swprintf_s(b, 96, L"nav completed: ok=%d err=0x%08X", ok, err);
                WVLog(b);
                return S_OK;
            }).Get(), nullptr);

            web->Navigate(htmlUrl.c_str());
            WVLog((std::wstring(L"ctrl callback: Navigate ") + htmlUrl).c_str());
            return S_OK;
        }).Get());
        WVLog(L"env callback: CreateCoreWebView2Controller called");
        return hrC;
    }).Get();

    // 用户数据目录放 exe 旁（免装临时目录权限问题）
    HRESULT hrE = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, userData.c_str(), nullptr, envDone);
    wchar_t buf[64];
    swprintf_s(buf, 64, L"InitWebView: CreateEnvironment hr=0x%08X", hrE);
    WVLog(buf);
    return SUCCEEDED(hrE);
}

// ------------------------------------------------------------ 窗口

static LRESULT CALLBACK WndProcWV(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE: {
        if (wp != SIZE_MINIMIZED && g_ctrl) {
            RECT rc;
            GetClientRect(hWnd, &rc);
            g_ctrl->put_Bounds(rc);
        }
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hWnd, 1);
        LoopCaptureStop();
        BtSvcStop();
        PostQuitMessage(0);
        return 0;
    case WM_TIMER:
        if (wp == 1) {
            UpdateDeviceStatuses();
            VolumeSyncFromSystem();
            int ph = BtSvcThreadActive() ? BtSvcPhase() : -1;
            switch (ph) {
            case 1: g_status = L"【扫描中】让设备进入配对模式"; break;
            case 2: g_status = L"【连接中】正在配对并建立音频链路……"; break;
            case 3: g_status = L"【推流中】正在发送音频 —— 可再选设备加入"; break;
            case 4: g_status = L"【失败】详见下方信息，可重新扫描再试"; break;
            default:
                if (BtSvcThreadActive())
                    g_status = L"【已就绪】选中设备后点「连接并推流」或右键操作";
                break;
            }
            PushState();
        }
        return 0;
    case WM_A2_STATUS: {
        wchar_t* p = (wchar_t*)wp;
        if (p) {
            g_detail = p;
            HeapFree(GetProcessHeap(), 0, p);
        }
        return 0;
    }
    case WM_A2_DEVICE: {
        wchar_t* p = (wchar_t*)wp;
        if (!p) return 0;
        const wchar_t* addrW = p;
        const wchar_t* nameW = addrW + wcslen(addrW) + 1;
        unsigned cod = (unsigned)*(nameW + wcslen(nameW) + 1);

        DevRec d;
        d.addr = addrW;
        d.nameUtf8 = ToUtf8(nameW);
        d.cod = cod;
        d.addrUtf8 = ToUtf8(addrW);
        HeapFree(GetProcessHeap(), 0, p);

        if (g_devs.find(d.addr) == g_devs.end()) g_devs[d.addr] = d;
        return 0;
    }
    }
    return DefWindowProcW(hWnd, msg, wp, lp);
}

void ShowBtA2dpPanelWV(HWND parent)
{
    (void)parent;
    SetUnhandledExceptionFilter(WVCrashHandler);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProcWV;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"DualBltWebViewPanel";
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Dual Blt",
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 720, 640,
                             nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hwnd) return;

    // 深色标题栏 + flap 配色
    {
        BOOL dark = TRUE;
        DwmSetWindowAttribute(g_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        COLORREF caption = 0x080807;
        COLORREF textcol = 0x00FFD0;
        COLORREF borderc = 0x313131;
        DwmSetWindowAttribute(g_hwnd, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
        DwmSetWindowAttribute(g_hwnd, DWMWA_TEXT_COLOR, &textcol, sizeof(textcol));
        DwmSetWindowAttribute(g_hwnd, DWMWA_BORDER_COLOR, &borderc, sizeof(borderc));
    }

    // 图标
    {
        wchar_t exePath[MAX_PATH], icoPath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        wchar_t* slash = wcsrchr(exePath, L'\\');
        if (slash) *(slash + 1) = L'\0';
        swprintf_s(icoPath, MAX_PATH, L"%sDualBlt.ico", exePath);
        HICON hIcon = (HICON)LoadImageW(nullptr, icoPath, IMAGE_ICON, 0, 0,
                                        LR_LOADFROMFILE | LR_DEFAULTSIZE);
        if (hIcon) {
            SendMessageW(g_hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
            SendMessageW(g_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
        }
    }

    ShowWindow(g_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(g_hwnd);

    // 二分定位：先创建 WebView2（在任何枚举/COM 使用之前），再加载初始数据
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    wchar_t coBuf[64];
    swprintf_s(coBuf, 64, L"panel: CoInitializeEx hr=0x%08X", hrCo);
    WVLog(coBuf);
    if (!InitWebView(g_hwnd)) {
        MessageBoxW(g_hwnd,
            L"WebView2 Runtime 未安装。\n\n请安装 Microsoft Edge WebView2 Runtime（系统一般自带，"
            L"也可从 https://developer.microsoft.com/microsoft-edge/webview2 下载 Evergreen 安装器）。",
            L"Dual Blt", MB_ICONWARNING);
    }

    // 初始数据
    WVLog(L"panel: init data begin");
    unsigned cv = 0, cp = 0;
    BtSvcGetUsbDevice(cv, cp);
    g_adapters = AdapterEnumAll((uint16_t)cv, (uint16_t)cp);
    for (size_t k = 0; k < g_adapters.size(); ++k)
        if (g_adapters[k].winusb) { g_adapterSel = (int)k; break; }
    if (g_adapterSel >= 0 && g_adapterSel < (int)g_adapters.size())
        BtSvcSetUsbDevice(g_adapters[g_adapterSel].vid, g_adapters[g_adapterSel].pid);
    g_srcDevs = LoopEnumRenderDevices();
    LoadPairedDevices();
    VolumeLoadUI();
    WVLog(L"panel: init data done");

    SetTimer(g_hwnd, 1, 500, nullptr);

    // 消息循环
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}
