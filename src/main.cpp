#include "engine.h"

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <algorithm>

static HANDLE g_stop = nullptr;

static BOOL WINAPI CtrlHandler(DWORD type)
{
    switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
        SetEvent(g_stop);
        return TRUE;
    default:
        return FALSE;
    }
}

static std::string Utf8(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static void Out(const std::string& s)
{
    fputs(s.c_str(), stdout);
    fflush(stdout);
}

static void Usage()
{
    Out(
        "DualBlt - 双路蓝牙音频同时输出（纯用户态，无需驱动签名）\n"
        "\n"
        "用法:\n"
        "  DualBlt.exe --list                      列出所有音频端点\n"
        "  DualBlt.exe --in N --out N M [选项]     启动双路输出\n"
        "  DualBlt.exe                             交互式选择设备\n"
        "\n"
        "  --in N        输入端点序号，取自「录制端点」列表\n"
        "                （例如 VB-CABLE Input、立体声混音）\n"
        "  --loopback    --in 的序号改为取自「渲染端点」列表（回环采集系统输出）\n"
        "  --out N M     两个输出端点序号，取自「渲染端点」列表（两副蓝牙耳机）\n"
        "\n"
        "选项:\n"
        "  --buf MS      环形缓冲目标水位，默认 40。越小越跟手，越小越容易爆音\n"
        "  --period MS   渲染端请求周期，默认 0（交给音频引擎决定）\n"
        "  --hw-a MS     输出端 A 的蓝牙编解码额外延迟，默认 0\n"
        "  --hw-b MS     输出端 B 的蓝牙编解码额外延迟，默认 0\n"
        "                填这两个值可以让两副耳机在时间上对齐\n"
        "  --no-sync     关闭两耳机自动对齐\n"
        "  --quiet       不打印周期统计\n"
        "\n"
        "运行时按 Ctrl+C 或直接回车停止。\n"
        "\n"
        "推荐接法：装 VB-CABLE，把系统默认播放设备设为 CABLE Output，\n"
        "然后 --in 选 CABLE Input，--out 选两副耳机。\n"
        "这样音频只做一次转换，延迟最低，也不会和回环采集互相干扰。\n"
        "\n");
}

static void PrintList(const std::vector<DeviceInfo>& v, const char* title)
{
    char buf[1024];
    snprintf(buf, sizeof(buf), "\n%s:\n", title);
    Out(buf);
    if (v.empty()) { Out("  (无)\n"); return; }
    for (size_t i = 0; i < v.size(); ++i) {
        snprintf(buf, sizeof(buf), "  [%d] %s\n", (int)i, Utf8(v[i].name).c_str());
        Out(buf);
    }
}

static bool ParseIdx(const wchar_t* s, int& v)
{
    if (!s || !*s) return false;
    wchar_t* end = nullptr;
    long n = wcstol(s, &end, 10);
    if (end == s) return false;
    v = (int)n;
    return true;
}

static bool ParseNum(const wchar_t* s, double& v)
{
    if (!s || !*s) return false;
    wchar_t* end = nullptr;
    double n = wcstod(s, &end);
    if (end == s) return false;
    v = n;
    return true;
}

// 从 stdin 读一个序号（只接受 ASCII 数字，规避控制台代码页问题）
static bool AskIndex(const char* prompt, int maxVal, int& out)
{
    char line[64];
    for (int attempt = 0; attempt < 3; ++attempt) {
        Out(prompt);
        if (!fgets(line, sizeof(line), stdin)) return false;
        char* end = nullptr;
        long n = strtol(line, &end, 10);
        if (end != line && n >= 0 && n < maxVal) { out = (int)n; return true; }
        Out("  序号无效，请重输。\n");
    }
    return false;
}

int wmain(int argc, wchar_t** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        Out("CoInitializeEx 失败\n");
        return 1;
    }

    std::vector<DeviceInfo> caps = EnumDevices(eCapture);
    std::vector<DeviceInfo> rends = EnumDevices(eRender);

    bool listOnly = false, loopback = false, quiet = false, syncPair = true;
    bool haveIn = false, haveOut = false;
    int inIdx = -1, outIdx[2] = { -1, -1 };
    double bufMs = 40.0, periodMs = 0.0, hwA = 0.0, hwB = 0.0;

    for (int i = 1; i < argc; ++i) {
        if      (!_wcsicmp(argv[i], L"--list"))     listOnly = true;
        else if (!_wcsicmp(argv[i], L"--loopback")) loopback = true;
        else if (!_wcsicmp(argv[i], L"--no-sync"))  syncPair = false;
        else if (!_wcsicmp(argv[i], L"--quiet"))    quiet = true;
        else if (!_wcsicmp(argv[i], L"--in"))       { if (i + 1 < argc) haveIn = ParseIdx(argv[++i], inIdx); }
        else if (!_wcsicmp(argv[i], L"--out")) {
            if (i + 2 < argc) {
                bool a = ParseIdx(argv[++i], outIdx[0]);
                bool b = ParseIdx(argv[++i], outIdx[1]);
                haveOut = a && b;
            }
        }
        else if (!_wcsicmp(argv[i], L"--buf"))    { if (i + 1 < argc) ParseNum(argv[++i], bufMs); }
        else if (!_wcsicmp(argv[i], L"--period")) { if (i + 1 < argc) ParseNum(argv[++i], periodMs); }
        else if (!_wcsicmp(argv[i], L"--hw-a"))   { if (i + 1 < argc) ParseNum(argv[++i], hwA); }
        else if (!_wcsicmp(argv[i], L"--hw-b"))   { if (i + 1 < argc) ParseNum(argv[++i], hwB); }
        else { Usage(); CoUninitialize(); return 1; }
    }

    if (listOnly) {
        PrintList(caps, "录制端点（用于 --in）");
        PrintList(rends, "渲染端点（用于 --out，加 --loopback 时也可用于 --in）");
        Out("\n");
        CoUninitialize();
        return 0;
    }

    // 交互式选择
    if (!haveIn || !haveOut) {
        if (argc <= 1) Usage();
        if (!haveIn) {
            if (loopback) {
                PrintList(rends, "输入：请选一个渲染端点（回环采集）");
                if (!AskIndex("输入端点序号 > ", (int)rends.size(), inIdx)) { CoUninitialize(); return 1; }
            } else {
                PrintList(caps, "输入：请选一个录制端点");
                if (caps.empty()) {
                    Out("\n没有可用的录制端点。\n"
                        "请安装 VB-CABLE 并把系统默认播放设备设为 CABLE Output，\n"
                        "然后选 CABLE Input；或者用 --loopback 回环采集。\n");
                    CoUninitialize();
                    return 1;
                }
                if (!AskIndex("输入端点序号 > ", (int)caps.size(), inIdx)) { CoUninitialize(); return 1; }
            }
            haveIn = true;
        }
        if (!haveOut) {
            PrintList(rends, "输出：请选两个渲染端点（两副耳机）");
            if (rends.size() < 2) {
                Out("\n渲染端点不足 2 个。两个蓝牙适配器都必须在设备管理器里正常工作。\n");
                CoUninitialize();
                return 1;
            }
            int a = -1, b = -1;
            if (!AskIndex("输出端 A 序号 > ", (int)rends.size(), a)) { CoUninitialize(); return 1; }
            if (!AskIndex("输出端 B 序号 > ", (int)rends.size(), b)) { CoUninitialize(); return 1; }
            outIdx[0] = a; outIdx[1] = b;
            haveOut = true;
        }
    }

    const std::vector<DeviceInfo>& inList = loopback ? rends : caps;
    if (inIdx < 0 || inIdx >= (int)inList.size()) { Out("--in 序号越界\n"); CoUninitialize(); return 1; }
    for (int i = 0; i < 2; ++i) {
        if (outIdx[i] < 0 || outIdx[i] >= (int)rends.size()) { Out("--out 序号越界\n"); CoUninitialize(); return 1; }
    }
    if (outIdx[0] == outIdx[1]) { Out("两个输出端不能是同一个设备\n"); CoUninitialize(); return 1; }

    EngineConfig cfg;
    cfg.captureId = inList[inIdx].id;
    cfg.loopback = loopback;
    cfg.outId[0] = rends[outIdx[0]].id;
    cfg.outId[1] = rends[outIdx[1]].id;
    cfg.targetBufferMs = bufMs;
    cfg.renderPeriodMs = periodMs;
    cfg.hwLatencyMs[0] = hwA;
    cfg.hwLatencyMs[1] = hwB;
    cfg.syncPair = syncPair;

    DualEngine engine;
    std::wstring err;
    if (!engine.Start(cfg, err)) {
        Out("启动失败: " + Utf8(err) + "\n");
        CoUninitialize();
        return 1;
    }

    char line[256];
    snprintf(line, sizeof(line),
             "\n已启动\n  输入: %s%s\n  输出 A: %s\n  输出 B: %s\n"
             "  采样率: 采集 %d Hz\n  目标缓冲: %.0f ms\n",
             Utf8(inList[inIdx].name).c_str(), loopback ? " (回环)" : "",
             Utf8(engine.OutName(0)).c_str(),
             Utf8(engine.OutName(1)).c_str(),
             engine.CaptureRate(), bufMs);
    Out(line);

    double total = bufMs + std::max(engine.Stats(0).streamLatencyMs.load() + hwA,
                                    engine.Stats(1).streamLatencyMs.load() + hwB);
    snprintf(line, sizeof(line),
             "  端到端延迟约: %.0f ms（另加蓝牙编解码延迟，需在播放器里设音频延迟补偿）\n"
             "  两耳机对齐偏移: A +%.1f ms / B +%.1f ms\n"
             "\n运行中。按 Ctrl+C 或回车停止。\n\n",
             total,
             engine.Stats(0).extraDelayMs.load(),
             engine.Stats(1).extraDelayMs.load());
    Out(line);

    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    // 回车退出（阻塞在 stdin 的辅助线程）
    std::thread stopper([]() {
        char buf[64];
        if (fgets(buf, sizeof(buf), stdin)) SetEvent(g_stop);
    });

    if (!quiet) {
        Out("  水位(ms) / 速率 / 重同步次数\n");
        while (WaitForSingleObject(g_stop, 1000) == WAIT_TIMEOUT) {
            for (int i = 0; i < 2; ++i) {
                const RenderStats& s = engine.Stats(i);
                snprintf(line, sizeof(line), "  %c %7.1f  %6.4f  %u\n",
                         'A' + i, s.bufferedMs.load(), s.ratio.load(), s.glitches.load());
                Out(line);
            }
        }
    } else {
        WaitForSingleObject(g_stop, INFINITE);
    }

    Out("\n停止中...\n");
    engine.Stop();
    if (stopper.joinable()) stopper.detach();
    CloseHandle(g_stop);
    CoUninitialize();
    return 0;
}
