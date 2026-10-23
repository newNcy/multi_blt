#define UNICODE
#ifndef UNICODE
#endif

#include "btpanel2.h"
#include "btsvc.h"
#include "loopcap.h"

#include <windows.h>
#include <commctrl.h>

#include <string>
#include <map>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

enum {
    ID_A2_LIST = 2301,
    ID_A2_SCAN,
    ID_A2_CONNECT,
    ID_A2_WAITPAIR,
    ID_A2_DISCONNECT,
    ID_A2_TEST,
    ID_A2_SRC,
    ID_A2_COPY,
    ID_A2_FORGET,
    ID_A2_STATUS
};

#define WM_A2_STATUS (WM_APP + 30)
#define WM_A2_DEVICE (WM_APP + 31)

static HWND g_p = nullptr;
static HINSTANCE g_inst = nullptr;
static HWND g_list = nullptr;
static HWND g_stStatus = nullptr;
static HWND g_stDetail = nullptr;
static HWND g_cboSrc = nullptr;
static std::vector<LoopDevice> g_srcDevs;
static HFONT g_font = nullptr;

// 扫描结果（去重）
struct DevRec {
    std::wstring addr;
    std::string  addrUtf8;
    std::wstring name;
    unsigned     cod;
};
static std::map<std::wstring, DevRec> g_devs;

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

static std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

// BTstack 线程回调：发现设备 → 投递到 GUI 线程
static void DeviceCb(const char* addr, const char* name, unsigned cod, void* user)
{
    HWND h = (HWND)user;
    if (!h) return;

    // 打包成一个 UTF-16 缓冲：addr\0name\0cod
    std::wstring a = Utf8ToWide(addr);
    std::wstring n = Utf8ToWide(name);
    size_t need = (a.size() + 1 + n.size() + 1 + 2) * sizeof(wchar_t);
    wchar_t* buf = (wchar_t*)HeapAlloc(GetProcessHeap(), 0, need);
    if (!buf) return;
    wcscpy_s(buf, a.size() + 1, a.c_str());
    wcscpy_s(buf + a.size() + 1, n.size() + 1, n.c_str());
    buf[a.size() + 1 + n.size() + 1] = (wchar_t)cod;
    if (!PostMessageW(h, WM_A2_DEVICE, (WPARAM)buf, 0)) HeapFree(GetProcessHeap(), 0, buf);
}

static HWND Ctrl(const wchar_t* cls, const wchar_t* text, int id, DWORD style,
                 int x, int y, int w, int h, DWORD ex = 0)
{
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style,
                             x, y, w, h, g_p, (HMENU)(INT_PTR)id, g_inst, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

static int SelIndex()
{
    return (int)SendMessageW(g_list, LVM_GETNEXTITEM, -1, LVNI_SELECTED);
}

static void AddDevice(const DevRec& d)
{
    LVITEMW it = {};
    it.mask = LVIF_TEXT | LVIF_PARAM;
    it.iItem = (int)SendMessageW(g_list, LVM_GETITEMCOUNT, 0, 0);
    it.pszText = (LPWSTR)(d.name.empty() ? (const wchar_t*)L"(未知名)" : d.name.c_str());
    it.lParam = (LPARAM)g_devs.size();
    ListView_InsertItem(g_list, &it);
    ListView_SetItemText(g_list, it.iItem, 1, (LPWSTR)d.addr.c_str());
    const wchar_t* kind = ((d.cod >> 8) & 0x1F) == 0x04 ? L"音频设备" : L"";
    ListView_SetItemText(g_list, it.iItem, 2, (LPWSTR)kind);
}

// 状态文件统一放 exe 所在目录（与 btsvc.cpp 的 AppFile 一致，避免分目录读写分裂）
static std::wstring AppFileW(const wchar_t* name)
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    size_t p = dir.find_last_of(L'\\');
    if (p == std::wstring::npos) p = dir.find_last_of(L'/');
    dir = dir.substr(0, p + 1);
    return dir + name;
}

// 保存已配对设备名（配对成功后调用）
static void SavePairedName(const std::wstring& addr, const std::wstring& name)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, AppFileW(L"paired_names.txt").c_str(), L"a, ccs=UTF-8") != 0 || !f) return;
    fwprintf(f, L"%s\t%s\n", addr.c_str(), name.c_str());
    fclose(f);
}

// 启动时加载已配对设备，直接填进列表（不用再扫描）
static void LoadPairedDevices()
{
    // 读名字映射
    std::map<std::wstring, std::wstring> names;
    FILE* nf = nullptr;
    if (_wfopen_s(&nf, AppFileW(L"paired_names.txt").c_str(), L"r, ccs=UTF-8") == 0 && nf) {
        wchar_t line[256];
        while (fgetws(line, 256, nf)) {
            wchar_t* tab = wcschr(line, L'\t');
            if (!tab) continue;
            *tab = 0;
            wchar_t* nm = tab + 1;
            wchar_t* nl = wcschr(nm, L'\n');
            if (nl) *nl = 0;
            names[line] = nm;        // 后写的覆盖，取最新名字
        }
        fclose(nf);
    }

    // 读已配对地址（linkkeys.txt 每行开头是地址）
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
        int nb = WideCharToMultiByte(CP_UTF8, 0, waddr, -1, nullptr, 0, nullptr, nullptr);
        d.addrUtf8.resize(nb, 0);
        WideCharToMultiByte(CP_UTF8, 0, waddr, -1, &d.addrUtf8[0], nb, nullptr, nullptr);
        auto itn = names.find(key);
        d.name = (itn != names.end()) ? itn->second : L"(已配对设备)";
        d.cod = 0x240404;   // 当音频设备显示
        g_devs[key] = d;
        AddDevice(d);
    }
    fclose(kf);
}

static void CreateControls(HWND hwnd)
{
    const int x0 = 14, W = 592;

    Ctrl(L"STATIC", L"① 扫描耳机（先让耳机进入配对模式）", 0, 0, x0, 12, W, 18);
    g_list = Ctrl(WC_LISTVIEWW, L"", ID_A2_LIST,
                  LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                  x0, 32, W, 150, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    const wchar_t* cols[3] = { L"名称", L"地址", L"类型" };
    int cw[3] = { 280, 170, 120 };
    for (int i = 0; i < 3; ++i) {
        LVCOLUMNW c = {};
        c.mask = LVCF_WIDTH | LVCF_TEXT | LVCF_SUBITEM;
        c.cx = cw[i];
        c.iSubItem = i;
        c.pszText = (LPWSTR)cols[i];
        ListView_InsertColumn(g_list, i, &c);
    }

    Ctrl(L"BUTTON", L"扫描", ID_A2_SCAN, 0, x0, 190, 70, 26);
    Ctrl(L"BUTTON", L"连接并推流", ID_A2_CONNECT, 0, x0 + 76, 190, 100, 26);
    Ctrl(L"BUTTON", L"配对", ID_A2_WAITPAIR, 0, x0 + 182, 190, 90, 26);
    Ctrl(L"BUTTON", L"断开", ID_A2_DISCONNECT, 0, x0 + 278, 190, 60, 26);
    Ctrl(L"BUTTON", L"忘记", ID_A2_FORGET, 0, x0 + 344, 190, 60, 26);
    Ctrl(L"BUTTON", L"测试音", ID_A2_TEST, 0, x0 + 410, 190, 70, 26);

    // ② 声音来源：回环采集系统输出，复制到蓝牙
    Ctrl(L"STATIC", L"② 声音来源（复制系统声音到蓝牙，无需 Voicemeeter）", 0, 0, x0, 226, W, 18);
    g_cboSrc = Ctrl(L"COMBOBOX", L"", ID_A2_SRC, CBS_DROPDOWNLIST | WS_VSCROLL,
                    x0, 248, 300, 200);
    Ctrl(L"BUTTON", L"开始复制声音", ID_A2_COPY, 0, x0 + 310, 247, 120, 26);

    // 填充输出设备列表
    g_srcDevs = LoopEnumRenderDevices();
    for (auto& d : g_srcDevs)
        SendMessageW(g_cboSrc, CB_ADDSTRING, 0, (LPARAM)d.name.c_str());
    SendMessageW(g_cboSrc, CB_SETCURSEL, 0, 0);   // 默认：系统默认输出

    g_stStatus = Ctrl(L"STATIC", L"【待机】点「扫描」查找耳机", ID_A2_STATUS, 0, x0, 286, W, 24);
    HFONT big = CreateFontW(-16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei");
    SendMessageW(g_stStatus, WM_SETFONT, (WPARAM)big, TRUE);

    // 详细消息（错误码等）
    g_stDetail = Ctrl(L"STATIC", L"", 0, 0, x0, 314, W, 40);

    // 启动即载入已配对设备，免扫描直接可连
    LoadPairedDevices();
    if (SendMessageW(g_list, LVM_GETITEMCOUNT, 0, 0) > 0)
        SetWindowTextW(g_stStatus, L"【已就绪】已载入配对过的设备，选中后点「连接并推流」即可");

    SetTimer(hwnd, 1, 500, nullptr);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_p = hwnd;
        CreateControls(hwnd);
        return 0;

    case WM_TIMER: {
        int phase = BtSvcThreadActive() ? BtSvcPhase() : -1;
        if (BtSvcThreadActive()) {
            wchar_t buf[256];
            switch (phase) {
            case 1: swprintf_s(buf, 256, L"【扫描中】已发现 %u 个设备 —— 让耳机进入配对模式", (unsigned)g_devs.size()); break;
            case 2: swprintf_s(buf, 256, L"【连接中】正在配对并建立音频链路……"); break;
            case 3: swprintf_s(buf, 256, L"【推流中】正在发送音频 —— 可再选一副耳机点「连接并推流」加入"); break;
            case 4: swprintf_s(buf, 256, L"【失败】详见下方错误信息，可重新扫描再试"); break;
            default: swprintf_s(buf, 256, L"【已就绪】发现 %u 个设备，选中后点「连接并推流」", (unsigned)g_devs.size()); break;
            }
            SetWindowTextW(g_stStatus, buf);
        }

        // 按钮防呆：忙碌（扫描中/连接中）时禁用操作按钮，避免重复点击把栈搞乱
        bool scanning = (phase == 1);
        bool connecting = (phase == 2);
        bool streaming = (phase == 3);
        // Windows 风格：扫描中也能直接点设备连接/配对（内部会自动停扫描再操作）
        EnableWindow(GetDlgItem(hwnd, ID_A2_CONNECT),    !connecting);
        EnableWindow(GetDlgItem(hwnd, ID_A2_WAITPAIR),   !connecting);
        EnableWindow(GetDlgItem(hwnd, ID_A2_DISCONNECT),  streaming);
        EnableWindow(GetDlgItem(hwnd, ID_A2_FORGET),      !connecting);
        SetWindowTextW(GetDlgItem(hwnd, ID_A2_SCAN), scanning ? L"停止扫描" : L"扫描");
        return 0;
    }

    case WM_A2_STATUS: {
        wchar_t* p = (wchar_t*)wp;
        if (p) {
            SetWindowTextW(g_stDetail, p);
            HeapFree(GetProcessHeap(), 0, p);
        }
        return 0;
    }

    case WM_A2_DEVICE: {
        wchar_t* p = (wchar_t*)wp;
        if (!p) return 0;

        // 打包格式：addr\0 name\0 cod(wchar)
        const wchar_t* addrW = p;
        const wchar_t* nameW = addrW + wcslen(addrW) + 1;
        unsigned cod = (unsigned)*(nameW + wcslen(nameW) + 1);

        DevRec d;
        d.addr = addrW;
        d.name = nameW;
        d.cod  = cod;

        // 转成 UTF-8 给 BTstack（连接时用）
        int nb = WideCharToMultiByte(CP_UTF8, 0, d.addr.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string au(nb, 0);
        WideCharToMultiByte(CP_UTF8, 0, d.addr.c_str(), -1, &au[0], nb, nullptr, nullptr);
        d.addrUtf8 = au;

        HeapFree(GetProcessHeap(), 0, p);

        if (g_devs.find(d.addr) == g_devs.end()) {
            g_devs[d.addr] = d;
            AddDevice(d);
        }
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_A2_SCAN:
            // 扫描中再点 = 停止扫描
            if (BtSvcThreadActive() && BtSvcScanActive()) {
                BtSvcScanStop();
                SetWindowTextW(g_stStatus, L"已停止扫描。");
                break;
            }
            if (!BtSvcThreadActive()) {
                if (!BtSvcStart(nullptr, StatusCb, DeviceCb, hwnd)) {
                    SetWindowTextW(g_stStatus, L"启动失败。");
                    break;
                }
                Sleep(300);
            }
            // 保留已配对设备，清掉非配对的扫描结果后重扫
            ListView_DeleteAllItems(g_list);
            g_devs.clear();
            LoadPairedDevices();
            BtSvcScanStart();
            SetWindowTextW(g_stStatus, L"扫描中……让耳机进入配对模式。");
            break;

        case ID_A2_CONNECT: {
            int i = SelIndex();
            if (i < 0) { SetWindowTextW(g_stStatus, L"请先在列表里选中一个设备。"); break; }
            wchar_t addr[64] = L"";
            ListView_GetItemText(g_list, i, 1, addr, 64);
            if (!addr[0]) break;

            // 从缓存里取该设备的 UTF-8 地址
            std::wstring key = addr;
            auto it = g_devs.find(key);
            std::string a;
            if (it != g_devs.end()) a = it->second.addrUtf8;
            else {
                int nb = WideCharToMultiByte(CP_UTF8, 0, addr, -1, nullptr, 0, nullptr, nullptr);
                a.resize(nb, 0);
                WideCharToMultiByte(CP_UTF8, 0, addr, -1, &a[0], nb, nullptr, nullptr);
            }

            // Windows 风格：未配对 -> 自动先配对（配对成功后自动接续连接推流）
            if (!BtSvcIsPaired(a.c_str())) {
                if (!BtSvcThreadActive()) {
                    if (!BtSvcStart(nullptr, StatusCb, DeviceCb, hwnd)) break;
                    Sleep(300);
                }
                // 记住设备名（只存真实名字）
                {
                    wchar_t nm[128] = L"";
                    ListView_GetItemText(g_list, i, 0, nm, 128);
                    std::wstring sn = nm;
                    if (!sn.empty() && sn != L"(已配对设备)" && sn != L"(识别中...)"
                        && sn != L"(未知名)" && sn != L"(未知设备)")
                        SavePairedName(key, sn);
                }
                BtSvcPair(a.c_str());
                SetWindowTextW(g_stStatus, L"该设备未配对，正在自动配对（像 Windows 添加设备）……配对成功后自动连接推流");
                break;
            }

            if (!BtSvcThreadActive()) {
                if (!BtSvcStart(a.c_str(), StatusCb, DeviceCb, hwnd)) break;
            } else {
                BtSvcConnect(a.c_str());
            }
            SetWindowTextW(g_stStatus, L"连接中……（已配对，免配对直连）");
            break;
        }

        case ID_A2_WAITPAIR: {
            int i = SelIndex();
            if (i < 0) { SetWindowTextW(g_stStatus, L"请先在列表里选中一个设备。"); break; }
            wchar_t addr[64] = L"";
            ListView_GetItemText(g_list, i, 1, addr, 64);
            if (!addr[0]) break;
            std::wstring key = addr;
            auto it = g_devs.find(key);
            std::string a;
            if (it != g_devs.end()) a = it->second.addrUtf8;
            else {
                int nb = WideCharToMultiByte(CP_UTF8, 0, addr, -1, nullptr, 0, nullptr, nullptr);
                a.resize(nb, 0);
                WideCharToMultiByte(CP_UTF8, 0, addr, -1, &a[0], nb, nullptr, nullptr);
            }
            if (!BtSvcThreadActive()) {
                if (!BtSvcStart(nullptr, StatusCb, DeviceCb, hwnd)) break;
                Sleep(300);
            }
            // 记住设备名（只存真实名字，不存占位符）
            {
                wchar_t nm[128] = L"";
                ListView_GetItemText(g_list, i, 0, nm, 128);
                std::wstring sn = nm;
                if (!sn.empty() && sn != L"(已配对设备)" && sn != L"(识别中...)"
                    && sn != L"(未知名)" && sn != L"(未知设备)")
                    SavePairedName(key, sn);
            }
            BtSvcPair(a.c_str());
            SetWindowTextW(g_stStatus, L"配对中……（复刻 Windows 添加设备：删旧密钥+全新 SSP）请把耳机放进配对模式");
            break;
        }

        case ID_A2_COPY:
            if (LoopCaptureRunning()) {
                LoopCaptureStop();
                BtSvcSetTestTone(false);
                SetWindowTextW(g_stDetail, L"已停止复制声音。");
            } else {
                int si = (int)SendMessageW(g_cboSrc, CB_GETCURSEL, 0, 0);
                if (si < 0 || si >= (int)g_srcDevs.size()) si = 0;
                BtSvcSetTestTone(false);     // 关测试音，改推真实系统声音
                std::wstring err;
                if (LoopCaptureStart(g_srcDevs[si].id, err)) {
                    SetWindowTextW(g_stDetail,
                        L"正在复制系统声音到蓝牙（在电脑上播放音乐即可听到）。再点一次停止。");
                } else {
                    SetWindowTextW(g_stDetail, (L"复制失败：" + err).c_str());
                }
            }
            break;

        case ID_A2_FORGET: {
            int i = SelIndex();
            if (i < 0) { SetWindowTextW(g_stStatus, L"请先在列表里选中一个设备。"); break; }
            wchar_t addr[64] = L"";
            ListView_GetItemText(g_list, i, 1, addr, 64);
            if (!addr[0]) break;
            std::wstring key = addr;

            // 先断开链路
            BtSvcDisconnect();
            Sleep(300);

            // 删除密钥记录
            std::string a;
            auto it = g_devs.find(key);
            if (it != g_devs.end()) a = it->second.addrUtf8;
            else {
                int nb = WideCharToMultiByte(CP_UTF8, 0, addr, -1, nullptr, 0, nullptr, nullptr);
                a.resize(nb, 0);
                WideCharToMultiByte(CP_UTF8, 0, addr, -1, &a[0], nb, nullptr, nullptr);
            }
            BtSvcForgetDevice(a.c_str());

            // 删除名字记录（重写 paired_names.txt）
            {
                FILE* f = nullptr;
                if (_wfopen_s(&f, AppFileW(L"paired_names.txt").c_str(), L"r, ccs=UTF-8") == 0 && f) {
                    std::vector<std::wstring> lines;
                    wchar_t line[256];
                    while (fgetws(line, 256, f)) {
                        if (wcsncmp(line, key.c_str(), key.size()) == 0 && line[key.size()] == L'\t')
                            continue;   // 跳过要删的
                        lines.push_back(line);
                    }
                    fclose(f);
                    if (_wfopen_s(&f, AppFileW(L"paired_names.txt").c_str(), L"w, ccs=UTF-8") == 0 && f) {
                        for (auto& l : lines) fputws(l.c_str(), f);
                        fclose(f);
                    }
                }
            }

            // 从列表移除
            g_devs.erase(key);
            ListView_DeleteItem(g_list, i);

            SetWindowTextW(g_stStatus, L"已忘记该设备。选中后点「连接并推流」将重新配对（耳机需在配对模式）。");
            break;
        }

        case ID_A2_DISCONNECT:
            BtSvcDisconnect();            // 只断流，保留服务以便重连
            SetWindowTextW(g_stStatus, L"已断开（可再次连接）。");
            break;

        case ID_A2_TEST:
            BtSvcSetTestTone(!BtSvcTestTone());
            SetWindowTextW(g_stStatus, BtSvcTestTone() ? L"测试音：开（441Hz 正弦波）"
                                                       : L"测试音：关（使用系统音频）");
            break;
        }
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        LoopCaptureStop();    // 停止回环采集
        BtSvcStop();          // 关面板时停止服务、释放 dongle
        g_p = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ShowBtA2dpPanel(HWND parent)
{
    if (g_p) { SetForegroundWindow(g_p); return; }

    g_inst = GetModuleHandleW(nullptr);
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"DualBltA2dpPanel";
        if (!RegisterClassW(&wc)) return;
        registered = true;
    }

    g_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

    g_p = CreateWindowExW(0, L"DualBltA2dpPanel", L"BTstack 推流（用户态蓝牙栈）",
                          WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                          CW_USEDEFAULT, CW_USEDEFAULT, 636, 410,
                          parent, nullptr, g_inst, nullptr);
    if (!g_p) return;
    ShowWindow(g_p, SW_SHOW);
    UpdateWindow(g_p);
}
