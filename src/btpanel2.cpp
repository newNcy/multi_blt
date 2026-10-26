// DualBlt 蓝牙推流面板 —— Dear ImGui 版
// C++ 引擎（btsvc/loopcap/adapter）不变，界面用 ImGui(D3D11) 渲染。
// 结构：Win32 窗口 + D3D11 交换链 + ImGui 每帧绘制；蓝牙线程通过
// WM_APP 消息把状态投递到 UI 线程。
#define UNICODE
#ifndef UNICODE
#endif

#include "btpanel2.h"
#include "btsvc.h"
#include "loopcap.h"
#include "adapter.h"

#include <windows.h>
#include <dwmapi.h>

// 旧 SDK（VS2019 自带）缺少的 DWM 属性常量，手动补齐
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWA_TEXT_COLOR
#define DWMWA_TEXT_COLOR 36
#endif
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <string>
#include <map>
#include <vector>
#include <atomic>
#include <cmath>
#include <d3d11.h>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#ifndef IM_PI
#define IM_PI 3.14159265358979323846f
#endif
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

#define WM_A2_STATUS (WM_APP + 30)   // wParam = HeapAlloc 的 UTF-16 文本（btsvc Post 回调）
#define WM_A2_DEVICE (WM_APP + 31)   // wParam = HeapAlloc 的 addr\0name\0cod 包

// ------------------------------------------------------------ 数据

struct DevRec {
    std::wstring addr;
    std::string  addrUtf8;
    std::string  nameUtf8;
    unsigned     cod = 0;
    std::string  statusUtf8 = "未配对";   // 每 500ms 刷新
    bool         statusStreaming = false;
};

static HWND g_hwnd = nullptr;
static ImFont* g_fontSmall = nullptr;   // 控件区小字体
static ImFont* g_fontIcon = nullptr;    // Font Awesome 图标字体

static std::vector<LoopDevice> g_srcDevs;
static int g_srcSel = 0;

static std::map<std::wstring, DevRec> g_devs;        // key: 地址（宽）
static std::vector<BtAdapter> g_adapters;
static int g_adapterSel = -1;

static std::string g_statusUtf8 = "【待机】点「扫描」查找设备";
static std::string g_detailUtf8;

static float g_volUi = 100.0f;
static bool  g_volMute = false;
static DWORD g_lastVolumeEdit = 0;                   // 滑条编辑时间（同步轮询避让）
static DWORD g_lastUiTick = 0;

// ---- 工具 ----

static std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static std::wstring AppFileW(const wchar_t* name)
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    size_t p = dir.find_last_of(L"\\/");
    if (p == std::wstring::npos) p = dir.find_last_of(L'/');
    return dir.substr(0, p + 1) + name;
}

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
    std::wstring a = Utf8ToWide(addr), n = Utf8ToWide(name);
    size_t need = (a.size() + 1 + n.size() + 1 + 2) * sizeof(wchar_t);
    wchar_t* buf = (wchar_t*)HeapAlloc(GetProcessHeap(), 0, need);
    if (!buf) return;
    wcscpy_s(buf, a.size() + 1, a.c_str());
    wcscpy_s(buf + a.size() + 1, n.size() + 1, n.c_str());
    buf[a.size() + 1 + n.size() + 1] = (wchar_t)cod;
    if (!PostMessageW(h, WM_A2_DEVICE, (WPARAM)buf, 0)) HeapFree(GetProcessHeap(), 0, buf);
}

// ------------------------------------------------------------ 配对名单持久化

// 待补名字队列：点击时设备未配对 → 等配对完成的第一刻写入密钥行
static std::vector<std::pair<std::string, std::string>> g_pendingNames;

static void SavePairedName(const std::wstring& addr, const std::wstring& name)
{
    // 名字与密钥同存 linkkeys.txt。点击时设备往往还没配对——先尝试直接写，
    // 不行就进待补队列，配对完成的第一刻（500ms 内）写入密钥行
    std::string a = WideToUtf8(addr), n = WideToUtf8(name);
    if (!BtSvcSetName(a.c_str(), n.c_str())) {
        for (auto& p : g_pendingNames)
            if (p.first == a) { p.second = n; return; }
        g_pendingNames.push_back({ a, n });
    }
}

static void EnumPairedCb(const char* addr, const char* nameUtf8, void* user)
{
    auto* out = (std::vector<std::pair<std::string, std::string>>*)user;
    out->emplace_back(addr, nameUtf8 ? nameUtf8 : "");
}

static void LoadPairedDevices()
{
    // 单文件：密钥 + 名字都在 linkkeys.txt（服务 API 加锁读取）
    std::vector<std::pair<std::string, std::string>> list;
    BtSvcEnumPaired(EnumPairedCb, &list);

    // 一次性迁移：旧 paired_names.txt 里的名字并入单文件后废弃
    std::map<std::wstring, std::wstring> oldNames;
    FILE* nf = nullptr;
    std::wstring oldPath = AppFileW(L"paired_names.txt");
    if (_wfopen_s(&nf, oldPath.c_str(), L"r, ccs=UTF-8") == 0 && nf) {
        wchar_t line[256];
        while (fgetws(line, 256, nf)) {
            wchar_t* tab = wcschr(line, L'\t');
            if (!tab) continue;
            *tab = 0;
            wchar_t* e = wcschr(tab + 1, L'\n');
            if (e) *e = 0;
            oldNames[line] = tab + 1;
        }
        fclose(nf);
    }
    bool migrated = false;

    for (auto& kv : list) {
        std::wstring key = Utf8ToWide(kv.first);
        if (g_devs.count(key)) continue;

        DevRec d;
        d.addr = key;
        d.addrUtf8 = kv.first;
        std::string nmUtf8 = kv.second;
        if (nmUtf8.empty()) {
            auto itn = oldNames.find(key);
            if (itn != oldNames.end()) {
                nmUtf8 = WideToUtf8(itn->second);
                if (BtSvcSetName(kv.first.c_str(), nmUtf8.c_str())) migrated = true;
            }
        }
        if (nmUtf8.empty()) nmUtf8 = "(已配对设备)";
        d.nameUtf8 = nmUtf8;
        d.cod = 0x240404;
        d.statusUtf8 = "已配对";
        g_devs[key] = d;
    }
    // 迁移完成：旧文件改名归档，避免重复处理
    if (migrated || !oldNames.empty())
        MoveFileW(oldPath.c_str(), (oldPath + L".migrated").c_str());
}

// ------------------------------------------------------------ 音量（系统端点音量，双向同步）

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
    // 增益与 UI 同步，避免“滑条显示 30% 实际还是 100%”的初始失配
    BtSvcSetGain(g_volMute ? 0.0f : g_volUi / 100.0f);
}

static void VolumeApplyUI()
{
    // 关键：滑条必须走软件增益（编码前缩放 PCM）才对推流有效。
    // Windows 端点主音量在回环采集点之后，调它不影响推出去的音频；测试音则完全绕过采集。
    BtSvcSetGain(g_volMute ? 0.0f : g_volUi / 100.0f);
    EndpointVolSet(CurrentSrcId(), g_volUi / 100.0f, g_volMute);
    g_lastVolumeEdit = GetTickCount();
}

static void VolumeSyncFromSystem()
{
    if (GetTickCount() - g_lastVolumeEdit < 700) return;   // 刚编辑过，避让
    float v = 0; bool m = false;
    if (!EndpointVolGet(CurrentSrcId(), v, m)) return;
    g_volUi = v * 100.0f;
    g_volMute = m;
    // 系统音量变化（如键盘音量键）同步到推流增益，保持滑条与实际推流音量一致
    BtSvcSetGain(g_volMute ? 0.0f : g_volUi / 100.0f);
}

// ------------------------------------------------------------ 设备操作

static void ConnectDev(const DevRec& d)
{
    // 已在推流的设备不重复建流（避免状态错乱）
    const char* cur = BtSvcCurrentAddr();
    if (cur[0] && _stricmp(cur, d.addrUtf8.c_str()) == 0 && BtSvcPhase() == 3) {
        g_detailUtf8 = "该设备已在推流中，无需重复连接。";
        return;
    }
    // 设备带真名 → 先尝试写密钥行；未配对则进待补队列（配对完成即写）
    if (!d.nameUtf8.empty())
        SavePairedName(d.addr, Utf8ToWide(d.nameUtf8));
    if (!BtSvcThreadActive()) {
        if (!BtSvcStart(nullptr, StatusCb, DeviceCb, g_hwnd)) {
            g_detailUtf8 = "启动失败。";
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
        SavePairedName(d.addr, d.nameUtf8.empty() ? L"(已配对设备)" : Utf8ToWide(d.nameUtf8));
    BtSvcPair(d.addrUtf8.c_str());
}

static void ForgetDev(const DevRec& d)
{
    BtSvcDisconnectAddr(d.addrUtf8.c_str());   // 只断这台，另一路不受影响
    BtSvcForgetDevice(d.addrUtf8.c_str());   // 密钥+名字同删（单文件）
    g_devs.erase(d.addr);
}

static void ScanToggle()
{
    if (BtSvcThreadActive() && BtSvcScanActive()) {
        BtSvcScanStop();
        g_statusUtf8 = "已停止扫描。";
        return;
    }
    if (!BtSvcThreadActive()) {
        if (!BtSvcStart(nullptr, StatusCb, DeviceCb, g_hwnd)) {
            g_detailUtf8 = "启动失败。";
            return;
        }
    }
    // 保留已配对设备，清掉非配对的扫描结果
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
        g_detailUtf8 = "已停止复制声音。";
    } else {
        BtSvcSetTestTone(false);
        std::wstring err;
        if (LoopCaptureStart(CurrentSrcId(), err)) {
            g_detailUtf8 = "正在复制系统声音到蓝牙（在电脑上播放音乐即可听到）。";
        } else {
            g_detailUtf8 = WideToUtf8(L"复制失败：" + err);
        }
    }
}

// 每 500ms：刷新设备状态文本
static void UpdateDeviceStatuses()
{
    // 待补名字：配对一完成（IsPaired 变真）立即写入密钥行
    for (auto it = g_pendingNames.begin(); it != g_pendingNames.end(); ) {
        if (BtSvcIsPaired(it->first.c_str())) {
            BtSvcSetName(it->first.c_str(), it->second.c_str());
            it = g_pendingNames.erase(it);
        } else {
            ++it;
        }
    }

    const char* cur = BtSvcCurrentAddr();
    for (auto& kv : g_devs) {
        DevRec& d = kv.second;
        const char* st = "未配对";
        bool streaming = false;
        // 双路：每台设备独立状态（不再由全局 phase 推导）
        if (BtSvcIsStreamingAddr(d.addrUtf8.c_str())) {
            st = "推流中"; streaming = true;
        } else if (BtSvcIsConnectingAddr(d.addrUtf8.c_str())) {
            st = "连接中";
        } else if (BtSvcIsPaired(d.addrUtf8.c_str())) {
            st = "已配对";
        }
        d.statusUtf8 = st;
        d.statusStreaming = streaming;
    }

    // 名字同步补全：列表里认识的名字 → 同步进密钥库（含引擎自动配对的设备，
    // 500ms 内必补上）。SetName 幂等：名字没变不写盘。
    for (auto& kv : g_devs) {
        DevRec& d = kv.second;
        if (d.nameUtf8.empty() || d.nameUtf8 == "(已配对设备)") continue;
        if (BtSvcIsPaired(d.addrUtf8.c_str()))
            BtSvcSetName(d.addrUtf8.c_str(), d.nameUtf8.c_str());
    }

    // 有设备进入推流 → 自动开始复制系统默认输出（用户手动停过后不打扰，
    // 到下一次推流开始时再自动开）
    bool anyStreaming = false;
    for (auto& kv : g_devs)
        if (kv.second.statusStreaming) { anyStreaming = true; break; }
    static bool prevAnyStreaming = false;
    if (anyStreaming && !prevAnyStreaming && !LoopCaptureRunning()) {
        std::wstring err;
        if (LoopCaptureStart(CurrentSrcId(), err))
            g_detailUtf8 = "推流已开始：自动复制系统声音。";
    }
    prevAnyStreaming = anyStreaming;
}

// ------------------------------------------------------------ D3D11

static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

static bool CreateDeviceD3D(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL fl;
    const D3D_FEATURE_LEVEL fls[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT r = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                              fls, 2, D3D11_SDK_VERSION, &sd,
                                              &g_pSwapChain, &g_pd3dDevice, &fl, &g_pd3dDeviceContext);
    if (FAILED(r)) return false;
    ID3D11Texture2D* back = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&back));
    if (!back) return false;
    g_pd3dDevice->CreateRenderTargetView(back, nullptr, &g_mainRenderTargetView);
    back->Release();
    return true;
}

static void CleanupDeviceD3D()
{
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

static void ResizeD3D(int w, int h)
{
    if (!g_pd3dDevice || w <= 0 || h <= 0) return;
    g_mainRenderTargetView->Release();
    g_pSwapChain->ResizeBuffers(0, (UINT)w, (UINT)h, DXGI_FORMAT_UNKNOWN, 0);
    ID3D11Texture2D* back = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) {
        g_pd3dDevice->CreateRenderTargetView(back, nullptr, &g_mainRenderTargetView);
        back->Release();
    }
}

// ------------------------------------------------------------ 窗口过程

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wp, lp)) return 1;

    switch (msg) {
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) ResizeD3D(LOWORD(lp), HIWORD(lp));
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_A2_STATUS: {
        wchar_t* p = (wchar_t*)wp;
        if (p) {
            g_detailUtf8 = WideToUtf8(p);
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
        d.nameUtf8 = WideToUtf8(nameW);
        d.cod = cod;
        int nb = WideCharToMultiByte(CP_UTF8, 0, addrW, -1, nullptr, 0, nullptr, nullptr);
        d.addrUtf8.resize(nb, 0);
        WideCharToMultiByte(CP_UTF8, 0, addrW, -1, &d.addrUtf8[0], nb, nullptr, nullptr);
        HeapFree(GetProcessHeap(), 0, p);

        if (g_devs.find(d.addr) == g_devs.end()) g_devs[d.addr] = d;
        // 已配对设备扫到真名 → 第一时间写进密钥库（名字与密钥同存一行），
        // 避免磁盘上的名字被内存里无名字的旧条目落盘时覆盖
        if (!d.nameUtf8.empty() && BtSvcIsPaired(d.addrUtf8.c_str()))
        BtSvcSetName(d.addrUtf8.c_str(), d.nameUtf8.c_str());
        return 0;
    }
    }
    return DefWindowProcW(hWnd, msg, wp, lp);
}

// ------------------------------------------------------------ UI 绘制

// 控件行标签图标：kind 0=适配器(fa-plug) 1=声音来源(fa-volume-low) 2=音量(fa-volume-high)
static void DrawRowIcon(int kind)
{
    static const char* kIconUtf8[3] = {
        "\xEF\x87\xA6",   // U+F1E6 fa-plug
        "\xEF\x80\xA7",   // U+F027 fa-volume-low
        "\xEF\x80\xA8",   // U+F028 fa-volume-high
    };
    ImFont* f = g_fontIcon ? g_fontIcon : ImGui::GetFont();
    ImGui::PushFont(f);
    ImGui::TextColored(ImVec4(0.929f, 0.929f, 0.937f, 0.9f), "%s", kIconUtf8[kind]);
    ImGui::PopFont();
}

static void DrawUI()
{
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 12));
    ImGui::Begin("##panel", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);

    int phase = BtSvcThreadActive() ? BtSvcPhase() : -1;
    bool scanning = (phase == 1);
    bool connecting = (phase == 2);
    bool streaming = (phase == 3);

    // 控件区：小字体 + 紧凑内边距（下拉框/滑条/按钮整体变矮）
    ImGui::PushFont(g_fontSmall ? g_fontSmall : ImGui::GetFont());
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 4));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10, 6));

    // 适配器行（标签图标固定对齐，下面声音来源/音量行与其同列）
    DrawRowIcon(0);
    ImGui::SameLine(44.0f);
    ImGui::SetNextItemWidth(300);
    {
        static std::vector<std::string> items;
        static int lastCount = -1;
        if ((int)g_adapters.size() != lastCount) {
            items.clear();
            for (auto& a : g_adapters) {
                // 两根同型号 dongle 的 VID/PID 和名字都相同，用实例 ID 尾号区分，
                // 并标注当前归谁（本软件 WinUSB / 系统蓝牙 BTHUSB）
                std::string tail;
                {
                    std::wstring iid = a.instanceId;
                    size_t p = iid.find_last_of(L'&');
                    tail = WideToUtf8(p != std::wstring::npos ? iid.substr(p) : iid);
                }
                char line[256];
                // 归属标签：本软件 / 系统蓝牙 / 驱动异常(问题码)
                char tag[48];
                if (a.winusb)               strcpy_s(tag, "本软件");
                else if (a.problemCode)     sprintf_s(tag, "驱动异常%u", a.problemCode);
                else                        strcpy_s(tag, "系统蓝牙");
                sprintf_s(line, "%s  [%s]  (%04X:%04X%s)",
                          a.name.empty() ? "蓝牙适配器" : WideToUtf8(a.name).c_str(),
                          tag,
                          a.vid, a.pid, tail.c_str());
                items.push_back(line);
            }
            lastCount = (int)g_adapters.size();
        }
        if (items.empty()) {
            static const char* none = "未发现可用的 USB 蓝牙适配器";
            ImGui::BeginDisabled(true);
            ImGui::Combo("##adap", &g_adapterSel, &none, 1);
            ImGui::EndDisabled();
        } else {
            std::vector<const char*> ptrs;
            for (auto& s : items) ptrs.push_back(s.c_str());
            if (ImGui::Combo("##adap", &g_adapterSel, ptrs.data(), (int)ptrs.size())) {
                BtSvcStop();
                if (g_adapterSel >= 0 && g_adapterSel < (int)g_adapters.size()) {
                    BtAdapter& a = g_adapters[g_adapterSel];
                    BtSvcSetUsbDevice(a.vid, a.pid);
                    g_detailUtf8.clear();
                }
                lastCount = -1;
            }
        }
    }
    ImGui::SameLine();
    {
        ImDrawList* dlBtn = ImGui::GetWindowDrawList();
        ImVec2 bpos = ImGui::GetCursorScreenPos();
        ImVec2 bsz(84.0f, ImGui::GetFrameHeight());
        if (scanning) {
            // 扫描中：按钮背景 = 缓冲进度动画（黄绿高亮块循环扫过）
            float t = (float)ImGui::GetTime();
            dlBtn->AddRectFilled(bpos, ImVec2(bpos.x + bsz.x, bpos.y + bsz.y),
                                 ImGui::GetColorU32(ImVec4(0.110f, 0.110f, 0.133f, 1.0f)));
            float blockW = bsz.x * 0.45f;
            float x = fmodf(t * 170.0f, bsz.x + blockW) - blockW;
            dlBtn->PushClipRect(bpos, ImVec2(bpos.x + bsz.x, bpos.y + bsz.y), true);
            dlBtn->AddRectFilled(ImVec2(bpos.x + x, bpos.y), ImVec2(bpos.x + x + blockW, bpos.y + bsz.y),
                                 ImGui::GetColorU32(ImVec4(0.816f, 1.000f, 0.000f, 0.30f)));
            dlBtn->PopClipRect();
            // 按钮本体透明，只留文字与边框
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0, 0, 0, 0));
            if (ImGui::Button(scanning ? "停止扫描" : "扫描", bsz)) ScanToggle();
            ImGui::PopStyleColor(3);
        } else {
            // 待机：品牌荧光黄绿实心方角（flap CTA 风格）
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.816f, 1.000f, 0.000f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.878f, 1.000f, 0.196f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.700f, 0.878f, 0.000f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.027f, 0.031f, 0.031f, 1.0f));
            if (ImGui::Button("扫描", bsz)) ScanToggle();
            ImGui::PopStyleColor(4);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("刷新", ImVec2(64, 0))) {
        unsigned cv = 0, cp = 0;
        BtSvcGetUsbDevice(cv, cp);
        g_adapters = AdapterEnumAll((uint16_t)cv, (uint16_t)cp);
    }
    ImGui::SameLine();
    {
        bool occupied = g_adapterSel >= 0 && g_adapterSel < (int)g_adapters.size()
                        && !g_adapters[g_adapterSel].winusb;
        ImGui::BeginDisabled(!occupied);
        if (ImGui::Button("切换驱动", ImVec2(100, 0))) {
            if (g_adapterSel >= 0 && g_adapterSel < (int)g_adapters.size()) {
                BtAdapter& a = g_adapters[g_adapterSel];
                BtSvcStop();
                std::wstring err;
                // 按节点精准切换：DiInstallDevice 只动选中实例，另一根不受影响
                if (AdapterSwitchDriver(a.instanceId, a.vid, a.pid, true, err)) {
                    Sleep(2500);   // 等 Windows 重新绑定完成再刷新
                    unsigned cv = 0, cp = 0;
                    BtSvcGetUsbDevice(cv, cp);
                    g_adapters = AdapterEnumAll((uint16_t)cv, (uint16_t)cp);
                    g_detailUtf8 = "驱动切换完成，确认适配器状态后重新连接设备。";
                } else {
                    g_detailUtf8 = WideToUtf8(err);
                }
            }
        }
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    {
        // 反向切换：选中的适配器当前是 WinUSB（归我们软件）→ 切回系统蓝牙
        bool revertible = g_adapterSel >= 0 && g_adapterSel < (int)g_adapters.size()
                          && g_adapters[g_adapterSel].winusb;
        ImGui::BeginDisabled(!revertible);
        if (ImGui::Button("切回系统蓝牙", ImVec2(120, 0))) {
            if (g_adapterSel >= 0 && g_adapterSel < (int)g_adapters.size()) {
                BtAdapter& a = g_adapters[g_adapterSel];
                BtSvcStop();
                std::wstring err;
                // 按节点切回：DiInstallDevice 用收件箱 bthusb.inf 只装选中实例，
                // 驱动包保持在库中不动，另一根 WinUSB dongle 不受影响
                if (AdapterSwitchDriver(a.instanceId, a.vid, a.pid, false, err)) {
                    Sleep(2500);   // 等 Windows 重新绑定完成再刷新
                    unsigned cv = 0, cp = 0;
                    BtSvcGetUsbDevice(cv, cp);
                    g_adapters = AdapterEnumAll((uint16_t)cv, (uint16_t)cp);
                    g_detailUtf8 = "已切回系统蓝牙。要点「切换驱动」才能再用本软件。";
                } else {
                    g_detailUtf8 = WideToUtf8(err);
                }
            }
        }
        ImGui::EndDisabled();
    }

    // 声音来源行（与适配器行同列对齐）
    DrawRowIcon(1);
    ImGui::SameLine(44.0f);
    ImGui::SetNextItemWidth(300);
    {
        static std::vector<std::string> srcItems;
        static int lastSrcCount = -1;
        if ((int)g_srcDevs.size() != lastSrcCount) {
            srcItems.clear();
            for (auto& d : g_srcDevs) srcItems.push_back(WideToUtf8(d.name));
            lastSrcCount = (int)g_srcDevs.size();
        }
        if (srcItems.empty()) {
            static const char* none = "（无输出设备）";
            ImGui::Combo("##src", &g_srcSel, &none, 1);
        } else {
            std::vector<const char*> ptrs;
            for (auto& s : srcItems) ptrs.push_back(s.c_str());
            if (ImGui::Combo("##src", &g_srcSel, ptrs.data(), (int)ptrs.size())) VolumeLoadUI();
        }
    }
    ImGui::SameLine();
    {
        // 播放/暂停图标按钮（U+F04B play / U+F04C pause）
        bool capOn = LoopCaptureRunning();
        if (!capOn) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.816f, 1.000f, 0.000f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.878f, 1.000f, 0.196f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.700f, 0.878f, 0.000f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.027f, 0.031f, 0.031f, 1.0f));
        }
        if (g_fontIcon) ImGui::PushFont(g_fontIcon);
        ImGui::SetNextItemWidth(46.0f);
        if (ImGui::Button(capOn ? "\xEF\x80\x8C" : "\xEF\x80\x8B", ImVec2(46, 0))) CopyToggle();
        if (g_fontIcon) ImGui::PopFont();
        if (!capOn) ImGui::PopStyleColor(4);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", capOn ? "暂停复制系统声音" : "复制系统声音到蓝牙");
    }

    // 推流音量行
    DrawRowIcon(2);
    ImGui::SameLine(44.0f);
    ImGui::SetNextItemWidth(300);
    bool edited = ImGui::SliderFloat("##vol", &g_volUi, 0.0f, 100.0f, "%.0f%%");
    ImGui::SameLine();
    if (ImGui::Checkbox("静音", &g_volMute)) VolumeApplyUI();
    if (edited) VolumeApplyUI();
    ImGui::PopStyleVar(2);
    ImGui::PopFont();

    ImGui::Spacing();
    ImGui::TextDisabled("设备");
    ImGui::Spacing();

    // 设备列表放进可滚动子区域：占满剩余高度，底部状态条始终可见（不随内容被裁掉）
    float footerH = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y + 12;
    float listH = ImGui::GetContentRegionAvail().y - footerH;
    if (listH < 60.0f) listH = 60.0f;
    // 滚动交给子区域（子区域不用 NoScrollbar）；表格内部不再用 ScrollY，
    // 否则表格右侧会预留滚动条宽度，导致自绘行高亮比列区宽出一截
    ImGui::BeginChild("##devlist", ImVec2(0, listH), ImGuiChildFlags_None);
    // 设备列表：类型图标 | 名称 | 状态
    if (ImGui::BeginTable("dev", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("##icon", ImGuiTableColumnFlags_WidthFixed, 34);
        ImGui::TableSetupColumn("名称", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("状态", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("##act", ImGuiTableColumnFlags_WidthFixed, 56);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        for (auto& kv : g_devs) {
            DevRec& d = kv.second;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char id[64];
            sprintf_s(id, "##dev%s", d.addrUtf8.c_str());
            ImGui::PushID(id);
            // 关键：行左上角坐标必须在 Selectable 之前取（之后游标已推到下一行）
            ImVec2 rowPos = ImGui::GetCursorScreenPos();
            float  rowH   = ImGui::GetFrameHeight();
            // 图标列：按 COD 细分类判断 耳机 / 音箱
            bool headphone = false;
            if ((d.cod >> 8 & 0x1F) == 0x04) {
                unsigned minor = (d.cod >> 2) & 0x3F;
                headphone = (minor == 0x01 || minor == 0x02 || minor == 0x07);
            }
            // 悬停高亮自绘：Selectable 自带高亮从单元格内容区开始，与整行错位；
            // 关掉它，改为覆盖完整行高（含上下 padding）的自绘底板，与文字/图标同心
            ImGui::PushStyleColor(ImGuiCol_Header,        ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImVec4(0, 0, 0, 0));
            bool rowStreaming = false;
            {
                const char* cur = BtSvcCurrentAddr();
                rowStreaming = cur[0] && _stricmp(cur, d.addrUtf8.c_str()) == 0 && streaming;
            }
            if (ImGui::Selectable("##row", false,
                                  ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
            {
                // 整行可点：推流中的行点击任意位置 = 断开（只断这台）；未连接的行 = 连接
                if (rowStreaming) BtSvcDisconnectAddr(d.addrUtf8.c_str());
                else              ConnectDev(d);
            }
            ImGui::PopStyleColor(3);
            if (ImGui::IsItemHovered()) {
                // 用 ImGui 的行背景通道画整行高亮：行矩形由表格内部精确计算，
                // 不会像手拼矩形那样把 CellPadding 算重而与相邻行重叠
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                       ImGui::GetColorU32(ImVec4(0.150f, 0.160f, 0.180f, 1.0f)));
            }
            if (ImGui::BeginPopupContextItem("devctx")) {
                if (ImGui::MenuItem("连接并推流")) ConnectDev(d);
                if (ImGui::MenuItem("配对"))       PairDev(d);
                {
                    bool ton = BtSvcTestToneAddr(d.addrUtf8.c_str());
                    if (ImGui::MenuItem(ton ? "关闭测试音" : "测试音"))
                        BtSvcSetTestToneAddr(d.addrUtf8.c_str(), !ton);
                }
                ImGui::Separator();
                bool isCur = BtSvcCurrentAddr()[0] &&
                             _stricmp(BtSvcCurrentAddr(), d.addrUtf8.c_str()) == 0;
                ImGui::BeginDisabled(!streaming);
                if (ImGui::MenuItem("断开")) BtSvcDisconnectAddr(d.addrUtf8.c_str());
                ImGui::EndDisabled();
                if (ImGui::MenuItem("忘记设备")) ForgetDev(d);
                ImGui::EndPopup();
            }
            // 在本行图标列内绘制（覆盖在 Selectable 之上）
            {
                // rowPos.y 已是内容区顶部（含上 CellPadding），图标中心 = 内容区中点
                float cy = rowPos.y + rowH * 0.5f;
                ImVec2 c(rowPos.x + 15.0f, cy);
                ImU32 col = ImGui::GetColorU32(ImVec4(0.816f, 1.000f, 0.000f, 0.9f));
                if (headphone) {
                    // 耳机：头带弧（上）+ 两个分离的耳罩块（下），小尺寸下轮廓清晰
                    dl->PathArcTo(c, 5.5f, (float)IM_PI * 1.02f, (float)IM_PI * 1.98f, 16);
                    dl->PathStroke(col, 0, 2.0f);
                    dl->AddRectFilled(ImVec2(c.x - 8.6f, c.y - 1.0f), ImVec2(c.x - 5.2f, c.y + 6.5f), col, 1.5f);
                    dl->AddRectFilled(ImVec2(c.x + 5.2f, c.y - 1.0f), ImVec2(c.x + 8.6f, c.y + 6.5f), col, 1.5f);
                } else {
                    // 音箱：锥盆 + 箱体 + 声波
                    dl->AddTriangleFilled(ImVec2(c.x - 8.0f, c.y - 3.5f), ImVec2(c.x - 3.0f, c.y - 8.0f), ImVec2(c.x - 3.0f, c.y + 8.0f), col);
                    dl->AddRectFilled(ImVec2(c.x - 3.0f, c.y - 4.5f), ImVec2(c.x + 0.5f, c.y + 4.5f), col, 1.0f);
                    dl->PathArcTo(ImVec2(c.x + 1.0f, c.y), 6.0f, -0.9f, 0.9f, 10);
                    dl->PathStroke(col, 0, 1.6f);
                }
            }
            // 名称/状态文字垂直居中（行高由 Selectable 决定，Text 默认顶部对齐会错位；
            // 每次切列后游标 Y 会重置，所以两个列都要偏移）
            float yOff = (rowH - ImGui::GetTextLineHeight()) * 0.5f;
            ImGui::TableNextColumn();
            if (yOff > 0) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + yOff);
            ImGui::TextUnformatted(d.nameUtf8.c_str());
            // 状态列：动画图标（连接中=spinner 推流中=均衡器 失败=红✕ 已连接=绿圈 其余=灰点）
            ImGui::TableNextColumn();
            if (yOff > 0) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + yOff);
            {
                // 双路：按地址独立状态（不再由全局 phase 推导）
                bool connecting = BtSvcIsConnectingAddr(d.addrUtf8.c_str());
                bool streaming  = BtSvcIsStreamingAddr(d.addrUtf8.c_str());
                bool failed     = false;
                float t = (float)ImGui::GetTime();
                ImVec2 tp = ImGui::GetCursorScreenPos();
                float lh = ImGui::GetTextLineHeight();
                ImVec2 sc2(tp.x + 10.0f, tp.y + lh * 0.5f);
                if (connecting) {
                    ImU32 ic = ImGui::GetColorU32(ImVec4(0.816f, 1.000f, 0.000f, 1.0f));
                    float a0 = t * 5.0f;
                    dl->PathArcTo(sc2, 6.0f, a0, a0 + 4.2f, 20);
                    dl->PathStroke(ic, 0, 2.0f);
                } else if (streaming) {
                    ImU32 ic = ImGui::GetColorU32(ImVec4(0.169f, 0.760f, 0.208f, 1.0f));
                    for (int k = 0; k < 3; ++k) {
                        float hgt = 3.0f + 5.0f * (0.5f + 0.5f * sinf(t * 7.0f + k * 2.1f));
                        float bx = sc2.x - 5.5f + k * 4.5f;
                        dl->AddRectFilled(ImVec2(bx, sc2.y + 5.0f - hgt), ImVec2(bx + 2.8f, sc2.y + 5.0f), ic, 1.0f);
                    }
                } else if (failed) {
                    ImU32 ic = ImGui::GetColorU32(ImVec4(1.000f, 0.290f, 0.333f, 1.0f));
                    dl->AddLine(ImVec2(sc2.x - 5, sc2.y - 5), ImVec2(sc2.x + 5, sc2.y + 5), ic, 2.0f);
                    dl->AddLine(ImVec2(sc2.x - 5, sc2.y + 5), ImVec2(sc2.x + 5, sc2.y - 5), ic, 2.0f);
                } else if (BtSvcIsPaired(d.addrUtf8.c_str())) {
                    dl->AddCircle(sc2, 4.5f, ImGui::GetColorU32(ImVec4(0.169f, 0.760f, 0.208f, 1.0f)), 16, 2.0f);
                } else {
                    ImU32 ic = ImGui::GetColorU32(ImVec4(0.627f, 0.639f, 0.655f, 0.7f));
                    dl->AddCircleFilled(sc2, 3.5f, ic, 12);
                }
            }

            // 操作列：整个单元格一块颜色背景，无按钮外框
            //   未连接/失败 = 黄绿底 + 深色链形（点击=连接/重试）
            //   推流中     = 红底 + 白色✕（点击=断开）
            //   连接中     = 灰底禁用
            ImGui::TableNextColumn();
            {
                // 双路：按地址独立状态
                bool connecting = BtSvcIsConnectingAddr(d.addrUtf8.c_str());
                bool streaming  = BtSvcIsStreamingAddr(d.addrUtf8.c_str());
                float pad2 = ImGui::GetStyle().CellPadding.y;
                ImVec2 cpos = ImGui::GetCursorScreenPos();
                float cx1 = cpos.x - ImGui::GetStyle().CellPadding.x;
                // 行真实范围：内容区上下一边一个 pad2（之前多算成 +pad2*2 导致下越界）
                float cy1 = rowPos.y - pad2, cy2 = rowPos.y + rowH + pad2;
                float cw = 56.0f;
                ImVec2 bmin(cx1, cy1), bmax(cx1 + cw, cy2);
                ImVec2 bc((bmin.x + bmax.x) * 0.5f, (bmin.y + bmax.y) * 0.5f);
                bool hov = !connecting && ImGui::IsMouseHoveringRect(bmin, bmax);
                ImU32 bg, ic;
                if (streaming) {
                    bg = ImGui::GetColorU32(ImVec4(1.000f, 0.290f, 0.333f, hov ? 1.0f : 0.85f));   // 红底
                    ic = ImGui::GetColorU32(ImVec4(1, 1, 1, 1));                                    // 白✕
                } else if (connecting) {
                    bg = ImGui::GetColorU32(ImVec4(0.25f, 0.25f, 0.28f, 1.0f));                     // 灰底
                    ic = ImGui::GetColorU32(ImVec4(0.627f, 0.639f, 0.655f, 0.6f));
                } else {
                    bg = ImGui::GetColorU32(ImVec4(0.816f, 1.000f, 0.000f, hov ? 1.0f : 0.85f));    // 黄绿底
                    ic = ImGui::GetColorU32(ImVec4(0.027f, 0.031f, 0.031f, 1.0f));                  // 深色链形
                }
                dl->AddRectFilled(bmin, bmax, bg);
                if (streaming) {
                    dl->AddLine(ImVec2(bc.x - 5, bc.y - 5), ImVec2(bc.x + 5, bc.y + 5), ic, 2.2f);
                    dl->AddLine(ImVec2(bc.x - 5, bc.y + 5), ImVec2(bc.x + 5, bc.y - 5), ic, 2.2f);
                } else if (!connecting) {
                    dl->AddLine(ImVec2(bc.x - 4.5f, bc.y + 4.5f), ImVec2(bc.x + 4.5f, bc.y - 4.5f), ic, 2.0f);
                    dl->AddCircle(ImVec2(bc.x - 5.5f, bc.y + 3.5f), 3.0f, ic, 12, 1.8f);
                    dl->AddCircle(ImVec2(bc.x + 5.5f, bc.y - 3.5f), 3.0f, ic, 12, 1.8f);
                }
                char bid[40];
                sprintf_s(bid, "##act%s", d.addrUtf8.c_str());
                ImGui::SetCursorScreenPos(bmin);
                ImVec2 actSz(bmax.x - bmin.x, bmax.y - bmin.y);
                if (!connecting) {
                    if (ImGui::InvisibleButton(bid, actSz)) {
                        if (streaming) BtSvcDisconnectAddr(d.addrUtf8.c_str());   // 只断这台
                        else           ConnectDev(d);       // 连接/重试
                    }
                } else {
                    ImGui::Dummy(actSz);   // 连接中：占位保持行高一致（否则行高塌陷）
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (g_devs.empty())
        ImGui::TextDisabled("列表为空：点「扫描」发现新设备（已配对设备启动时自动载入）");
    ImGui::EndChild();

    ImGui::Separator();

    // 适配器挂死横幅：控制器无响应，必须拔插 dongle（拔插后自动恢复）
    if (BtSvcThreadActive() && BtSvcAdapterDead()) {
        float t = (float)ImGui::GetTime();
        float pulse = 0.6f + 0.4f * sinf(t * 4.0f);
        ImVec4 bc(1.000f * pulse, 0.150f * pulse, 0.200f * pulse, 1.0f);
        ImGui::PushFont(g_fontSmall ? g_fontSmall : ImGui::GetFont());
        ImVec2 ts = ImGui::CalcTextSize("适配器无响应：请拔插 dongle（拔插后自动恢复）");
        ImVec2 cs = ImGui::GetCursorScreenPos();
        ImDrawList* dl2 = ImGui::GetWindowDrawList();
        dl2->AddRectFilled(cs, ImVec2(cs.x + ts.x + 16, cs.y + ts.y + 8),
                           ImGui::GetColorU32(ImVec4(0.30f, 0.05f, 0.07f, 1.0f)), 4.0f);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4);
        ImGui::TextColored(bc, "适配器无响应：请拔插 dongle（拔插后自动恢复）");
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, 4));
    }

    // 状态条：一行小字（状态 + 说明合并；flap 语义色）
    ImVec4 sc(0.627f, 0.639f, 0.655f, 1.0f);
    if (phase == 3)      sc = ImVec4(0.169f, 0.760f, 0.208f, 1.0f);
    else if (phase == 4) sc = ImVec4(1.000f, 0.290f, 0.333f, 1.0f);
    else if (phase == 1 || phase == 2) sc = ImVec4(0.816f, 1.000f, 0.000f, 1.0f);
    ImGui::PushFont(g_fontSmall ? g_fontSmall : ImGui::GetFont());
    ImGui::TextColored(sc, "%s", g_statusUtf8.c_str());
    if (!g_detailUtf8.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("· %s", g_detailUtf8.c_str());
    }
    ImGui::PopFont();

    ImGui::End();
    ImGui::PopStyleVar(3);
}

// ------------------------------------------------------------ 主循环

static DWORD g_lastUiTick2 = 0;

void ShowBtA2dpPanel(HWND parent)
{
    (void)parent;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"DualBltImGuiPanel";
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Dual Blt",
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 720, 640,
                             nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hwnd) return;

    // 标题栏跟随 flap 主题：深色模式 + 近黑标题栏 + 黄绿标题字 + 细边框（Win11 DWM）
    {
#pragma comment(lib, "dwmapi.lib")
        BOOL dark = TRUE;
        DwmSetWindowAttribute(g_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        COLORREF caption = 0x080807;  // #070808（COLORREF 是 0x00BBGGRR）
        COLORREF textcol = 0x00FFD0;  // #D0FF00
        COLORREF borderc = 0x313131;  // #313131
        DwmSetWindowAttribute(g_hwnd, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
        DwmSetWindowAttribute(g_hwnd, DWMWA_TEXT_COLOR, &textcol, sizeof(textcol));
        DwmSetWindowAttribute(g_hwnd, DWMWA_BORDER_COLOR, &borderc, sizeof(borderc));
    }

    // 窗口图标：加载 exe 同目录的 DualBlt.ico（标题栏 + 任务栏）
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

    if (!CreateDeviceD3D(g_hwnd)) {
        CleanupDeviceD3D();
        DestroyWindow(g_hwnd);
        return;
    }
    ShowWindow(g_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(g_hwnd);

    // 初始数据
    unsigned cv = 0, cp = 0;
    BtSvcGetUsbDevice(cv, cp);
    g_adapters = AdapterEnumAll((uint16_t)cv, (uint16_t)cp);
    for (size_t k = 0; k < g_adapters.size(); ++k)
        if (g_adapters[k].winusb) { g_adapterSel = (int)k; break; }
    if (g_adapterSel >= 0 && g_adapterSel < (int)g_adapters.size())
        BtSvcSetUsbDevice(g_adapters[g_adapterSel].vid, g_adapters[g_adapterSel].pid);
    g_srcDevs = LoopEnumRenderDevices();
    LoadPairedDevices();

    // ImGui 初始化
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    // —— Flap.sh 真实 Token（提取自 flap.sh/board 的 HTML/CSS）——
    // 品牌荧光黄绿 #D0FF00 / 近黑背景 #070808 / 行卡 #17191B / 全直角 / 细边框 #313131
    ImGuiStyle& st = ImGui::GetStyle();
    const ImVec4 bg      (0.027f, 0.031f, 0.031f, 1.0f);   // #070808 页面背景
    const ImVec4 card    (0.090f, 0.098f, 0.106f, 1.0f);   // #17191B 行/卡片
    const ImVec4 cardHi  (0.118f, 0.125f, 0.133f, 1.0f);   // hover 提亮
    const ImVec4 border  (0.192f, 0.192f, 0.192f, 1.0f);   // #313131 边框
    const ImVec4 lime    (0.816f, 1.000f, 0.000f, 1.0f);   // #D0FF00 品牌色
    const ImVec4 limeHi  (0.878f, 1.000f, 0.196f, 1.0f);
    const ImVec4 upGreen (0.169f, 0.760f, 0.208f, 1.0f);   // #2BC235 上涨绿
    const ImVec4 downRed (1.000f, 0.290f, 0.333f, 1.0f);   // #FF4A55 下跌红
    const ImVec4 gray    (0.627f, 0.639f, 0.655f, 1.0f);   // #A0A3A7 次要文字
    const ImVec4 sepGray (0.282f, 0.294f, 0.318f, 1.0f);   // #484B51 分隔

    // flap 全站直角、锐利
    st.WindowRounding    = 0;
    st.ChildRounding     = 0;
    st.FrameRounding     = 0;
    st.PopupRounding     = 0;
    st.GrabRounding      = 0;
    st.ScrollbarRounding = 0;
    st.FramePadding      = ImVec2(12, 7);
    st.ItemSpacing       = ImVec2(10, 9);
    st.WindowBorderSize  = 0;
    st.FrameBorderSize   = 1;
    st.ChildBorderSize   = 1;
    st.PopupBorderSize   = 1;
    st.ScrollbarSize     = 10;

    ImVec4* C = st.Colors;
    C[ImGuiCol_WindowBg]           = bg;
    C[ImGuiCol_PopupBg]            = ImVec4(0.067f, 0.071f, 0.075f, 0.98f);
    C[ImGuiCol_Border]             = border;
    C[ImGuiCol_Text]               = ImVec4(0.929f, 0.929f, 0.937f, 1.0f);
    C[ImGuiCol_TextDisabled]       = gray;
    C[ImGuiCol_FrameBg]            = card;
    C[ImGuiCol_FrameBgHovered]     = cardHi;
    C[ImGuiCol_FrameBgActive]      = ImVec4(0.140f, 0.150f, 0.160f, 1.0f);
    C[ImGuiCol_Button]             = card;
    C[ImGuiCol_ButtonHovered]      = cardHi;
    C[ImGuiCol_ButtonActive]       = ImVec4(0.160f, 0.170f, 0.180f, 1.0f);
    C[ImGuiCol_SliderGrab]         = lime;
    C[ImGuiCol_SliderGrabActive]   = limeHi;
    C[ImGuiCol_CheckMark]          = lime;
    C[ImGuiCol_Header]             = card;
    C[ImGuiCol_HeaderHovered]      = cardHi;
    C[ImGuiCol_HeaderActive]       = ImVec4(0.160f, 0.170f, 0.180f, 1.0f);
    C[ImGuiCol_Separator]          = border;
    C[ImGuiCol_TableHeaderBg]      = ImVec4(0.067f, 0.071f, 0.075f, 1.0f); // #111
    C[ImGuiCol_TableBorderStrong]  = border;
    C[ImGuiCol_TableBorderLight]   = ImVec4(0.188f, 0.196f, 0.212f, 1.0f); // #303236
    C[ImGuiCol_TableRowBg]         = ImVec4(0, 0, 0, 0);
    C[ImGuiCol_TableRowBgAlt]      = ImVec4(1.0f, 1.0f, 1.0f, 0.02f);
    C[ImGuiCol_ScrollbarBg]        = bg;
    C[ImGuiCol_ScrollbarGrab]      = cardHi;
    C[ImGuiCol_ScrollbarGrabHovered]  = sepGray;
    C[ImGuiCol_ScrollbarGrabActive]   = lime;
    C[ImGuiCol_TitleBg]            = bg;
    C[ImGuiCol_TitleBgActive]      = bg;
    C[ImGuiCol_TitleBgCollapsed]   = bg;
    C[ImGuiCol_NavCursor]          = lime;

    // 中文字体（主字体 17px + 控件区小字体 13.5px）
    {
        const char* fonts[] = { "C:\\Windows\\Fonts\\msyh.ttc", "C:\\Windows\\Fonts\\simhei.ttf" };
        bool loaded = false;
        for (const char* fp : fonts) {
            if (GetFileAttributesA(fp) != INVALID_FILE_ATTRIBUTES) {
                io.Fonts->AddFontFromFileTTF(fp, 17.0f, nullptr, io.Fonts->GetGlyphRangesChineseFull());
                g_fontSmall = io.Fonts->AddFontFromFileTTF(fp, 13.5f, nullptr, io.Fonts->GetGlyphRangesChineseFull());
                loaded = true;
                break;
            }
        }
        if (!loaded) io.Fonts->AddFontDefault();

        // Font Awesome 图标字体（按需加载用到的码位）
        {
            wchar_t exePath[MAX_PATH], faPath[MAX_PATH];
            GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            wchar_t* slash = wcsrchr(exePath, L'\\');
            if (slash) *(slash + 1) = L'\0';
            swprintf_s(faPath, MAX_PATH, L"%sfa-solid-900.ttf", exePath);
            int n = WideCharToMultiByte(CP_UTF8, 0, faPath, -1, nullptr, 0, nullptr, nullptr);
            std::string faU(n, 0);
            WideCharToMultiByte(CP_UTF8, 0, faPath, -1, &faU[0], n, nullptr, nullptr);
            static const ImWchar iconRanges[] = {
                0xf002, 0xf002,   // magnifying-glass
                0xf00d, 0xf00d,   // xmark
                0xf021, 0xf021,   // rotate
                0xf025, 0xf025,   // headphones
                0xf026, 0xf026,   // (volume)
                0xf027, 0xf027,   // volume-low
                0xf028, 0xf028,   // volume-high
                0xf04b, 0xf04c,   // play / pause
                0xf1e6, 0xf1e6,   // plug
                0xf287, 0xf287,   // usb
                0xf6a9, 0xf6a9,   // volume-xmark
                0
            };
            g_fontIcon = io.Fonts->AddFontFromFileTTF(faU.c_str(), 12.0f, nullptr, iconRanges);
        }
    }

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        // 500ms 周期任务：设备状态刷新 + 系统音量同步
        DWORD now = GetTickCount();
        if (now - g_lastUiTick2 >= 500) {
            g_lastUiTick2 = now;
            UpdateDeviceStatuses();
            VolumeSyncFromSystem();
            int ph = BtSvcThreadActive() ? BtSvcPhase() : -1;
            switch (ph) {
            case 1: g_statusUtf8 = "【扫描中】让设备进入配对模式"; break;
            case 2: g_statusUtf8 = "【连接中】正在配对并建立音频链路……"; break;
            case 3: g_statusUtf8 = "【推流中】正在发送音频 —— 可再选设备加入"; break;
            case 4: g_statusUtf8 = "【失败】详见下方信息，可重新扫描再试"; break;
            default:
                if (BtSvcThreadActive())
                    g_statusUtf8 = "【已就绪】选中设备后点「连接并推流」或右键操作";
                break;
            }
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawUI();
        ImGui::Render();

        const float clear[4] = { 0.075f, 0.080f, 0.095f, 1.0f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);   // 垂直同步
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(g_hwnd);
    g_hwnd = nullptr;
}
