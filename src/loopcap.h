#pragma once

#include <windows.h>
#include <string>
#include <vector>

// 系统音频回环采集：抓取某个输出设备正在播放的声音，喂给 BTstack 推流。
// 不需要虚拟声卡 —— 直接用 WASAPI loopback。

struct LoopDevice {
    std::wstring id;     // 端点 ID（空 = 系统默认输出）
    std::wstring name;   // 友好名
};

// 枚举所有输出（渲染）端点
std::vector<LoopDevice> LoopEnumRenderDevices();

// 开始回环采集指定设备（id 为空则用系统默认输出）。采集到的 PCM 自动喂给 BTstack。
bool LoopCaptureStart(const std::wstring& deviceId, std::wstring& err);

// 停止回环采集
void LoopCaptureStop();

bool LoopCaptureRunning();
