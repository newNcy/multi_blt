// 系统媒体播放控制（Windows SMTC / WinRT）。
// 用 GlobalSystemMediaTransportControlsSessionManager 找到当前媒体会话
// （音乐/视频应用），对其下发真正的 Play/Pause —— 不是盲发的媒体键，
// 避免"玩家本来是暂停的,补一刀变成播放"的状态反转问题。
// SMTC 不可用时退化为 VK_MEDIA_PLAY_PAUSE 媒体键切换。
//
// 需 C++17（cppwinrt），在 build.bat 中单独以 /std:c++17 编译本文件。

#include "mediactrl.h"

#include <windows.h>
#include <roapi.h>
#include <roerrorapi.h>
#include <thread>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Media.Control.h>

// ---- cppwinrt fast-forward 垫片 ----
// base.h 以 dllimport 声明 WINRT_* 五个符号，正常由 NuGet 的
// cppwinrt_fast_fwd 提供；无 NuGet 环境自行实现，转发到 combase 原函数。
// 签名必须与 base.h 的声明完全一致（含 winrt::guid）。
extern "C" {
int32_t __stdcall WINRT_RoInitialize(uint32_t type) noexcept { return RoInitialize((RO_INIT_TYPE)type); }
void    __stdcall WINRT_RoUninitialize() noexcept { RoUninitialize(); }
int32_t __stdcall WINRT_RoGetActivationFactory(void* classId, winrt::guid const& iid, void** factory) noexcept
    { return RoGetActivationFactory((HSTRING)classId, reinterpret_cast<GUID const&>(iid), factory); }
int32_t __stdcall WINRT_RoOriginateLanguageException(int32_t error, void* message, void* exception) noexcept
    { return RoOriginateLanguageException(error, (HSTRING)message, (IUnknown*)exception); }
int32_t __stdcall WINRT_GetRestrictedErrorInfo(void** info) noexcept { return GetRestrictedErrorInfo((IRestrictedErrorInfo**)info); }
int32_t __stdcall WINRT_SetRestrictedErrorInfo(void* info) noexcept { return SetRestrictedErrorInfo((IRestrictedErrorInfo*)info); }
}

namespace winrt
{
    using namespace Windows::Foundation;
    using namespace Windows::Media;
    using namespace Windows::Media::Control;
}

static void FallbackKey();   // 媒体键兜底（定义见下）

// 取"当前"媒体会话：优先 GetCurrentSession，没有就取会话列表第一个
static winrt::GlobalSystemMediaTransportControlsSession GetCurSession(
    winrt::GlobalSystemMediaTransportControlsSessionManager const& mgr)
{
    try {
        auto s = mgr.GetCurrentSession();
        if (s) return s;
        auto list = mgr.GetSessions();
        if (list.Size() > 0) return list.GetAt(0);
    } catch (...) { }
    return nullptr;
}

static void MediaExec(bool wantPlay, bool toggle)
{
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        auto mgr = winrt::GlobalSystemMediaTransportControlsSessionManager::
            RequestAsync().get();
        if (!mgr) { FallbackKey(); return; }
        auto s = GetCurSession(mgr);
        if (!s) { FallbackKey(); return; }
        if (toggle) {
            auto st = s.GetPlaybackInfo().PlaybackStatus();
            if (st == winrt::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing)
                s.TryPauseAsync().get();
            else
                s.TryPlayAsync().get();
        } else {
            if (wantPlay) s.TryPlayAsync().get();
            else          s.TryPauseAsync().get();
        }
    } catch (...) {
        // WinRT 层任何异常 → 退化为媒体键
        keybd_event(VK_MEDIA_PLAY_PAUSE, 0, 0, 0);
        keybd_event(VK_MEDIA_PLAY_PAUSE, 0, KEYEVENTF_KEYUP, 0);
    }
}

static void FallbackKey()
{
    keybd_event(VK_MEDIA_PLAY_PAUSE, 0, 0, 0);
    keybd_event(VK_MEDIA_PLAY_PAUSE, 0, KEYEVENTF_KEYUP, 0);
}

void MediaPlay()   { std::thread([] { MediaExec(true,  false); }).detach(); }
void MediaPause()  { std::thread([] { MediaExec(false, false); }).detach(); }
void MediaToggle() { std::thread([] { MediaExec(false, true);  }).detach(); }
