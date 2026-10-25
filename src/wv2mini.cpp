// WebView2 最小复现：判断崩溃是否与本程序其他模块相关
#define UNICODE
#include <windows.h>
#include <wrl/client.h>
#include <wrl/event.h>
#include <WebView2.h>
#include <stdio.h>

using namespace Microsoft::WRL;

static void Log(const wchar_t* s)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, L"G:\\multi_blt\\build\\mini.log", L"a, ccs=UTF-8") != 0 || !f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fwprintf(f, L"[%02d:%02d:%02d.%03d] %s\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, s);
    fclose(f);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int)
{
    Log(L"start");
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    wchar_t b[64]; swprintf_s(b, 64, L"CoInitializeEx hr=0x%08X", hrCo); Log(b);

    WNDCLASSEXW wc = {}; wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = hInst; wc.lpszClassName = L"WV2Mini";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, L"WV2Mini", L"mini", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                100, 100, 600, 400, nullptr, nullptr, hInst, nullptr);
    Log(L"window created");

    auto envDone = Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
        [](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
        wchar_t b2[64]; swprintf_s(b2, 64, L"env callback hr=0x%08X", hr); Log(b2);
        if (FAILED(hr) || !env) return hr;
        return env->CreateCoreWebView2Controller(nullptr,
            Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [](HRESULT hr2, ICoreWebView2Controller* ctrl) -> HRESULT {
            wchar_t b3[64]; swprintf_s(b3, 64, L"ctrl callback hr=0x%08X", hr2); Log(b3);
            if (FAILED(hr2) || !ctrl) return hr2;
            ComPtr<ICoreWebView2> web; ctrl->get_CoreWebView2(&web);
            web->Navigate(L"about:blank");
            Log(L"navigate called");
            return S_OK;
        }).Get());
    }).Get();

    HRESULT hrE = CreateCoreWebView2EnvironmentWithOptions(nullptr,
        L"G:\\multi_blt\\build\\WebView2Data", nullptr, envDone);
    swprintf_s(b, 64, L"CreateEnvironment returned hr=0x%08X", hrE); Log(b);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}
