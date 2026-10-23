#ifndef UNICODE
#define UNICODE
#endif

#include "engine.h"
#include "btpanel.h"
#include "btpanel2.h"

#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <functiondiscoverykeys_devpkey.h>

#include <string>
#include <vector>
#include <algorithm>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// ------------------------------------------------- 设置默认输出设备用的私有接口

class __declspec(uuid("870af99c-171d-4f9e-af0d-e63df40c2bc9")) CPolicyConfigClient;

struct __declspec(uuid("f8679f50-850a-41cf-9c72-430f290290c8")) IPolicyConfig : public IUnknown {
    STDMETHOD(GetMixFormat)() = 0;
    STDMETHOD(GetDeviceFormat)() = 0;
    STDMETHOD(ResetDeviceFormat)() = 0;
    STDMETHOD(SetDeviceFormat)() = 0;
    STDMETHOD(GetProcessingPeriod)() = 0;
    STDMETHOD(SetProcessingPeriod)() = 0;
    STDMETHOD(GetShareMode)() = 0;
    STDMETHOD(SetShareMode)() = 0;
    STDMETHOD(GetPropertyValue)() = 0;
    STDMETHOD(SetPropertyValue)() = 0;
    STDMETHOD(SetDefaultEndpoint)(PCWSTR, ERole) = 0;
};

// ------------------------------------------------- 控件 ID

enum {
    ID_CBO_OUT = 1001,
    ID_BTN_DEFAULT,
    ID_CBO_CAP,
    ID_LIST_TGT,
    ID_EDT_BUF,
    ID_EDT_HWA,
    ID_EDT_HWB,
    ID_CHK_SYNC,
    ID_BTN_START,
    ID_BTN_STOP,
    ID_BTN_REFRESH,
    ID_BTN_BT,
    ID_BTN_BTA2DP,
    ID_ST_STATUS,
    ID_ST_A,
    ID_ST_B,
    ID_ST_HINT
};

// ------------------------------------------------- 全局状态

static HINSTANCE              g_inst = nullptr;
static HWND                   g_hwnd = nullptr;
static HWND                   g_cboOut = nullptr;
static HWND                   g_cboCap = nullptr;
static HWND                   g_list = nullptr;
static HWND                   g_status = nullptr;
static HWND                   g_stA = nullptr;
static HWND                   g_stB = nullptr;
static HWND                   g_hint = nullptr;
static HFONT                  g_font = nullptr;

static std::vector<DeviceInfo> g_rends;
static std::vector<DeviceInfo> g_caps;
static std::vector<int>        g_targetIdx;   // ListView 行 -> g_rends 下标
static DualEngine*             g_engine = nullptr;

static IMMDeviceEnumerator*    g_enum = nullptr;

// 端点变化通知：蓝牙耳机连上/断开时自动刷新列表
class DevNotify : public IMMNotificationClient {
public:
    HWND hwnd = nullptr;
    ULONG   STDMETHODCALLTYPE AddRef()  override { return 1; }
    ULONG   STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { Post(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { Post(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { Post(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override { Post(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
private:
    void Post() { if (hwnd) PostMessageW(hwnd, WM_APP + 1, 0, 0); }
};
static DevNotify g_notify;

// ------------------------------------------------- 小工具

static bool IsVirtualDevice(const std::wstring& n)
{
    std::wstring s = n;
    for (size_t i = 0; i < s.size(); ++i) s[i] = (wchar_t)towlower(s[i]);
    if (s.find(L"voicemeeter") != std::wstring::npos) return true;
    if (s.find(L"cable")       != std::wstring::npos) return true;
    if (s.find(L"virtual")     != std::wstring::npos) return true;
    if (n.find(L"虚拟")         != std::wstring::npos) return true;
    return false;
}

// 取名称末尾括号里的驱动名，用来把「输出端口」和「采集源」配对
static std::wstring DriverTag(const std::wstring& n)
{
    size_t p = n.rfind(L'(');
    if (p == std::wstring::npos) return std::wstring();
    return n.substr(p);
}

static void SetText(HWND h, const std::wstring& s) { SetWindowTextW(h, s.c_str()); }

static void Status(const std::wstring& s) { SetText(g_status, s); }

static std::wstring ReadDeviceName(IMMDevice* dev)
{
    std::wstring name = L"(未命名)";
    IPropertyStore* ps = nullptr;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps)) && ps) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
            name = v.pwszVal;
        PropVariantClear(&v);
        ps->Release();
    }
    return name;
}

static std::wstring CurrentDefaultRenderName()
{
    std::wstring r = L"(未知)";
    if (!g_enum) return r;
    IMMDevice* d = nullptr;
    if (SUCCEEDED(g_enum->GetDefaultAudioEndpoint(eRender, eConsole, &d)) && d) {
        r = ReadDeviceName(d);
        d->Release();
    }
    return r;
}

static bool SetDefaultEndpoint(const std::wstring& id)
{
    IPolicyConfig* pc = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(CPolicyConfigClient), nullptr, CLSCTX_ALL,
                                  __uuidof(IPolicyConfig), (void**)&pc);
    if (FAILED(hr) || !pc) return false;
    HRESULT a = pc->SetDefaultEndpoint(id.c_str(), eConsole);
    HRESULT b = pc->SetDefaultEndpoint(id.c_str(), eMultimedia);
    pc->Release();
    return SUCCEEDED(a) || SUCCEEDED(b);
}

// ------------------------------------------------- 列表填充

static int ComboAdd(HWND cbo, const std::wstring& text, int data)
{
    int idx = (int)SendMessageW(cbo, CB_ADDSTRING, 0, (LPARAM)text.c_str());
    if (idx >= 0) SendMessageW(cbo, CB_SETITEMDATA, idx, (LPARAM)data);
    return idx;
}

static int ComboSelData(HWND cbo)
{
    int i = (int)SendMessageW(cbo, CB_GETCURSEL, 0, 0);
    if (i < 0) return -1;
    return (int)SendMessageW(cbo, CB_GETITEMDATA, i, 0);
}

// 根据已选的虚拟输出端口，自动挑一个同驱动的采集源
static void AutoPickCapture()
{
    int ri = ComboSelData(g_cboOut);
    if (ri < 0 || ri >= (int)g_rends.size()) return;
    std::wstring tag = DriverTag(g_rends[ri].name);
    if (tag.empty()) return;

    int cnt = (int)SendMessageW(g_cboCap, CB_GETCOUNT, 0, 0);
    for (int i = 0; i < cnt; ++i) {
        int ci = (int)SendMessageW(g_cboCap, CB_GETITEMDATA, i, 0);
        if (ci < 0 || ci >= (int)g_caps.size()) continue;
        if (DriverTag(g_caps[ci].name) == tag) {
            SendMessageW(g_cboCap, CB_SETCURSEL, i, 0);
            return;
        }
    }
}

static void FillTargets()
{
    ListView_DeleteAllItems(g_list);
    g_targetIdx.clear();

    int exclude = ComboSelData(g_cboOut);

    for (size_t i = 0; i < g_rends.size(); ++i) {
        if ((int)i == exclude) continue;   // 排除采集源本身，避免回环
        LVITEMW it = {};
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = (int)g_targetIdx.size();
        it.iSubItem = 0;
        it.pszText = (LPWSTR)g_rends[i].name.c_str();
        it.lParam = (LPARAM)i;
        int row = (int)SendMessageW(g_list, LVM_INSERTITEMW, 0, (LPARAM)&it);
        g_targetIdx.push_back((int)i);
        (void)row;
    }
}

static void RefreshDevices()
{
    g_rends = EnumDevices(eRender);
    g_caps = EnumDevices(eCapture);

    SendMessageW(g_cboOut, CB_RESETCONTENT, 0, 0);
    SendMessageW(g_cboCap, CB_RESETCONTENT, 0, 0);

    // 虚拟设备排在前面
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < g_rends.size(); ++i) {
            bool virt = IsVirtualDevice(g_rends[i].name);
            if (virt != (pass == 0)) continue;
            std::wstring t = g_rends[i].name + (virt ? L"   [虚拟]" : L"");
            ComboAdd(g_cboOut, t, (int)i);
        }
    }
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < g_caps.size(); ++i) {
            bool virt = IsVirtualDevice(g_caps[i].name);
            if (virt != (pass == 0)) continue;
            std::wstring t = g_caps[i].name + (virt ? L"   [虚拟]" : L"");
            ComboAdd(g_cboCap, t, (int)i);
        }
    }

    if (SendMessageW(g_cboOut, CB_GETCOUNT, 0, 0) > 0) SendMessageW(g_cboOut, CB_SETCURSEL, 0, 0);
    if (SendMessageW(g_cboCap, CB_GETCOUNT, 0, 0) > 0) SendMessageW(g_cboCap, CB_SETCURSEL, 0, 0);

    AutoPickCapture();
    FillTargets();

    int nv = 0;
    for (size_t i = 0; i < g_rends.size(); ++i) if (IsVirtualDevice(g_rends[i].name)) ++nv;

    std::wstring hint;
    if (nv == 0) {
        hint = L"未检测到虚拟声卡。请用 VoiceMeeter（已安装）或安装免费的 VB-CABLE，"
               L"再把它的输出端口设为系统默认播放设备。软件无法凭空创建虚拟设备——那必须是已签名的内核驱动。";
    } else {
        hint = L"当前系统默认播放设备: " + CurrentDefaultRenderName();
    }
    SetText(g_hint, hint);
}

// ------------------------------------------------- 启动 / 停止

static void SetRunningUI(bool running)
{
    EnableWindow(g_cboOut, !running);
    EnableWindow(g_cboCap, !running);
    EnableWindow(g_list, !running);
    EnableWindow(GetDlgItem(g_hwnd, ID_EDT_BUF), !running);
    EnableWindow(GetDlgItem(g_hwnd, ID_EDT_HWA), !running);
    EnableWindow(GetDlgItem(g_hwnd, ID_EDT_HWB), !running);
    EnableWindow(GetDlgItem(g_hwnd, ID_CHK_SYNC), !running);
    EnableWindow(GetDlgItem(g_hwnd, ID_BTN_START), !running);
    EnableWindow(GetDlgItem(g_hwnd, ID_BTN_STOP), running);
    EnableWindow(GetDlgItem(g_hwnd, ID_BTN_REFRESH), !running);
    EnableWindow(GetDlgItem(g_hwnd, ID_BTN_DEFAULT), !running);
}

static void OnStart()
{
    if (g_engine) return;

    int capIdx = ComboSelData(g_cboCap);
    if (capIdx < 0 || capIdx >= (int)g_caps.size()) {
        Status(L"请先选择采集源。");
        return;
    }

    int n = (int)ListView_GetItemCount(g_list);
    std::vector<int> sel;
    for (int i = 0; i < n; ++i) {
        if (ListView_GetCheckState(g_list, i) && i < (int)g_targetIdx.size())
            sel.push_back(g_targetIdx[i]);
    }
    if (sel.empty()) { Status(L"请至少勾选一个输出设备。"); return; }
    if (sel.size() > 2) { Status(L"最多只能选两个输出设备。"); return; }

    EngineConfig cfg;
    cfg.captureId = g_caps[capIdx].id;
    cfg.loopback = false;
    cfg.outCount = (int)sel.size();
    cfg.outId[0] = g_rends[sel[0]].id;
    if (sel.size() > 1) cfg.outId[1] = g_rends[sel[1]].id;
    cfg.targetBufferMs = (double)GetDlgItemInt(g_hwnd, ID_EDT_BUF, nullptr, FALSE);
    cfg.hwLatencyMs[0] = (double)GetDlgItemInt(g_hwnd, ID_EDT_HWA, nullptr, FALSE);
    cfg.hwLatencyMs[1] = sel.size() > 1 ? (double)GetDlgItemInt(g_hwnd, ID_EDT_HWB, nullptr, FALSE) : 0.0;
    cfg.syncPair = (IsDlgButtonChecked(g_hwnd, ID_CHK_SYNC) == BST_CHECKED) && sel.size() > 1;

    g_engine = new DualEngine();
    std::wstring err;
    if (!g_engine->Start(cfg, err)) {
        Status(L"启动失败: " + err);
        delete g_engine;
        g_engine = nullptr;
        return;
    }

    std::wstring msg = L"运行中 → " + g_engine->OutName(0);
    if (sel.size() > 1) msg += L" + " + g_engine->OutName(1);
    Status(msg);
    SetRunningUI(true);
    SetTimer(g_hwnd, 1, 500, nullptr);
}

static void OnStop()
{
    if (!g_engine) return;
    KillTimer(g_hwnd, 1);
    g_engine->Stop();
    delete g_engine;
    g_engine = nullptr;
    SetText(g_stA, L"");
    SetText(g_stB, L"");
    SetRunningUI(false);
    Status(L"已停止。");
}

static void UpdateStats()
{
    if (!g_engine) return;
    if (g_engine->Faulted()) {
        OnStop();
        Status(L"设备已断开，输出已停止。请重新连接耳机后再次启动。");
        return;
    }
    wchar_t buf[256];
    for (int i = 0; i < 2; ++i) {
        const RenderStats& s = g_engine->Stats(i);
        if (s.outRate.load() == 0) { SetText(i == 0 ? g_stA : g_stB, L""); continue; }
        swprintf(buf, 256, L"%c  %-28s  水位 %6.1f ms   速率 %.4f   重同步 %u",
                 L'A' + i, g_engine->OutName(i).c_str(),
                 s.bufferedMs.load(), s.ratio.load(), s.glitches.load());
        SetText(i == 0 ? g_stA : g_stB, buf);
    }
}

// ------------------------------------------------- 窗口

static HWND MakeStatic(HWND p, const wchar_t* text, int id, int x, int y, int w, int h)
{
    HWND ctl = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                               x, y, w, h, p, (HMENU)(INT_PTR)id, g_inst, nullptr);
    SendMessageW(ctl, WM_SETFONT, (WPARAM)g_font, TRUE);
    return ctl;
}

static HWND MakeButton(HWND p, const wchar_t* text, int id, int x, int y, int w, int h)
{
    HWND ctl = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                               x, y, w, h, p, (HMENU)(INT_PTR)id, g_inst, nullptr);
    SendMessageW(ctl, WM_SETFONT, (WPARAM)g_font, TRUE);
    return ctl;
}

static HWND MakeEdit(HWND p, const wchar_t* text, int id, int x, int y, int w, int h)
{
    HWND ctl = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text,
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER,
                               x, y, w, h, p, (HMENU)(INT_PTR)id, g_inst, nullptr);
    SendMessageW(ctl, WM_SETFONT, (WPARAM)g_font, TRUE);
    return ctl;
}

static void CreateControls(HWND hwnd)
{
    const int x0 = 16, W = 572;

    MakeStatic(hwnd, L"DualBlt — 把一路声音同时送到 1~2 个蓝牙设备", 0, x0, 14, W, 20);
    MakeStatic(hwnd, L"① 让系统声音进入本软件", 0, x0, 44, W, 18);

    MakeStatic(hwnd, L"虚拟输出端口（点右边按钮把它设为系统默认播放设备）", 0, x0, 66, 380, 18);
    g_cboOut = CreateWindowExW(0, L"COMBOBOX", L"",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                               x0, 86, 400, 260, hwnd, (HMENU)(INT_PTR)ID_CBO_OUT, g_inst, nullptr);
    MakeButton(hwnd, L"设为系统默认输出", ID_BTN_DEFAULT, x0 + 412, 85, 160, 26);
    SendMessageW(g_cboOut, WM_SETFONT, (WPARAM)g_font, TRUE);

    MakeStatic(hwnd, L"采集源（本软件从这里取声音）", 0, x0, 120, 380, 18);
    g_cboCap = CreateWindowExW(0, L"COMBOBOX", L"",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                               x0, 140, W, 260, hwnd, (HMENU)(INT_PTR)ID_CBO_CAP, g_inst, nullptr);
    SendMessageW(g_cboCap, WM_SETFONT, (WPARAM)g_font, TRUE);

    g_hint = MakeStatic(hwnd, L"", ID_ST_HINT, x0, 170, W, 34);

    MakeStatic(hwnd, L"② 目标输出设备（勾选 1~2 个，通常就是你的蓝牙耳机）", 0, x0, 210, W, 18);
    g_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT |
                             LVS_NOCOLUMNHEADER | LVS_SHOWSELALWAYS | LVS_SINGLESEL,
                             x0, 232, W, 150, hwnd, (HMENU)(INT_PTR)ID_LIST_TGT, g_inst, nullptr);
    ListView_SetExtendedListViewStyle(g_list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT |
                                              LVS_EX_DOUBLEBUFFER | LVS_EX_FLATSB);
    LVCOLUMNW col = {};
    col.mask = LVCF_WIDTH | LVCF_TEXT;
    col.cx = W - 24;
    col.pszText = (LPWSTR)L"设备";
    ListView_InsertColumn(g_list, 0, &col);
    SendMessageW(g_list, WM_SETFONT, (WPARAM)g_font, TRUE);

    MakeStatic(hwnd, L"③ 选项", 0, x0, 392, W, 18);
    MakeStatic(hwnd, L"缓冲水位(ms)", 0, x0, 414, 90, 18);
    MakeEdit(hwnd, L"40", ID_EDT_BUF, x0 + 96, 412, 60, 22);
    MakeStatic(hwnd, L"耳机A 编解码延迟(ms)", 0, x0 + 180, 414, 130, 18);
    MakeEdit(hwnd, L"0", ID_EDT_HWA, x0 + 316, 412, 56, 22);
    MakeStatic(hwnd, L"耳机B 编解码延迟(ms)", 0, x0 + 390, 414, 130, 18);
    MakeEdit(hwnd, L"0", ID_EDT_HWB, x0 + 526, 412, 56, 22);

    HWND chk = CreateWindowExW(0, L"BUTTON", L"对齐两只耳机（补偿两端的延迟差）",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX | BST_CHECKED,
                               x0, 442, W, 20, hwnd, (HMENU)(INT_PTR)ID_CHK_SYNC, g_inst, nullptr);
    SendMessageW(chk, WM_SETFONT, (WPARAM)g_font, TRUE);

    MakeButton(hwnd, L"启动", ID_BTN_START, x0, 472, 100, 28);
    MakeButton(hwnd, L"停止", ID_BTN_STOP, x0 + 112, 472, 100, 28);
    MakeButton(hwnd, L"刷新设备", ID_BTN_REFRESH, x0 + 224, 472, 100, 28);
    MakeButton(hwnd, L"蓝牙适配器...", ID_BTN_BT, x0 + 336, 472, 120, 28);
    MakeButton(hwnd, L"BTstack 推流...", ID_BTN_BTA2DP, x0 + 468, 472, 130, 28);

    g_status = MakeStatic(hwnd, L"就绪。", ID_ST_STATUS, x0, 512, W, 20);
    g_stA = MakeStatic(hwnd, L"", ID_ST_A, x0, 536, W, 18);
    g_stB = MakeStatic(hwnd, L"", ID_ST_B, x0, 556, W, 18);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        CreateControls(hwnd);
        return 0;

    case WM_APP + 1:
        if (!g_engine) RefreshDevices();
        return 0;

    case WM_TIMER:
        UpdateStats();
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_BTN_REFRESH:
            RefreshDevices();
            Status(L"设备列表已刷新。");
            break;
        case ID_BTN_DEFAULT: {
            int ri = ComboSelData(g_cboOut);
            if (ri < 0 || ri >= (int)g_rends.size()) break;
            if (SetDefaultEndpoint(g_rends[ri].id)) {
                Status(L"已把「" + g_rends[ri].name + L"」设为系统默认播放设备。");
            } else {
                Status(L"自动设置失败，已打开系统声音设置，请手动选择。");
                ShellExecuteW(nullptr, L"open", L"ms-settings:sound", nullptr, nullptr, SW_SHOWNORMAL);
            }
            RefreshDevices();
            break;
        }
        case ID_BTN_START: OnStart(); break;
        case ID_BTN_STOP:  OnStop();  break;
        case ID_BTN_BT:    ShowBtPanel(hwnd); break;
        case ID_BTN_BTA2DP: ShowBtA2dpPanel(hwnd); break;
        case ID_CBO_OUT:
            if (HIWORD(wp) == CBN_SELCHANGE) { AutoPickCapture(); FillTargets(); }
            break;
        }
        return 0;

    case WM_NOTIFY: {
        NMHDR* nm = (NMHDR*)lp;
        if (nm->idFrom == ID_LIST_TGT && nm->code == LVN_ITEMCHANGED) {
            NM_LISTVIEW* lv = (NM_LISTVIEW*)lp;
            if (lv->iItem < 0) return 0;
            if (lv->uChanged & LVIF_STATE) {
                BOOL nowChecked = ListView_GetCheckState(g_list, lv->iItem);
                if (nowChecked) {
                    int cnt = 0;
                    int n = (int)ListView_GetItemCount(g_list);
                    for (int i = 0; i < n; ++i) if (ListView_GetCheckState(g_list, i)) ++cnt;
                    if (cnt > 2) {
                        ListView_SetCheckState(g_list, lv->iItem, FALSE);
                        Status(L"最多只能选两个输出设备。");
                    } else {
                        Status(L"就绪。");
                    }
                }
            }
        }
        return 0;
    }

    case WM_DESTROY:
        OnStop();
        if (g_enum && g_notify.hwnd) g_enum->UnregisterEndpointNotificationCallback(&g_notify);
        if (g_enum) { g_enum->Release(); g_enum = nullptr; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int)
{
    g_inst = hInst;

    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   IID_PPV_ARGS(&g_enum))) && g_enum) {
        g_notify.hwnd = nullptr;   // 窗口建好后再注册
    }

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"DualBltMainWindow";
    if (!RegisterClassW(&wc)) { CoUninitialize(); return 1; }

    g_hwnd = CreateWindowExW(0, L"DualBltMainWindow", L"DualBlt 双路音频输出",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             CW_USEDEFAULT, CW_USEDEFAULT, 620, 640,
                             nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) { CoUninitialize(); return 1; }

    g_notify.hwnd = g_hwnd;
    if (g_enum) g_enum->RegisterEndpointNotificationCallback(&g_notify);

    RefreshDevices();
    SetRunningUI(false);
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    return 0;
}
