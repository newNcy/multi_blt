#pragma once

// BTstack A2DP 推流服务：独立线程跑 BTstack，GUI 通过本接口控制
// 音频来源：调用方通过 BtSvcFeedAudio 喂 PCM（来自系统音频采集），
//          没喂的时候可选测试音。

#include <windows.h>

typedef void (*BtSvcStatusFn)(const wchar_t* msg, void* user);

// 扫描发现设备时回调（addr/name 为 UTF-8，cod 为设备类别）
typedef void (*BtSvcDeviceFn)(const char* addr, const char* name, unsigned cod, void* user);

// 启动服务（创建线程）。addr 非空则直接连它；为空则进入扫描状态等用户选。
bool BtSvcStart(const char* addr, BtSvcStatusFn cb, BtSvcDeviceFn devFn, void* user);

// 停止服务（结束线程、关断 A2DP）
void BtSvcStop();

// 开始扫描（结果通过 devFn 回调）
void BtSvcScanStart();

// 停止扫描
void BtSvcScanStop();

// 连接指定设备并推流
void BtSvcConnect(const char* addr);

// 断开当前 A2DP 流（保留服务，可再次连接）
void BtSvcDisconnect();

// 进入被动等待配对模式：保持可发现可连接，等耳机主动连入（SSP 更易成功）
void BtSvcWaitPairing();

// 主动发起专门配对（复刻 Windows「添加设备」：删旧密钥 + 全新 SSP）
void BtSvcPair(const char* addr);

// 该地址是否已配对（有链路密钥）
bool BtSvcIsPaired(const char* addr);

// 忘记设备：删除其链路密钥，下次连接将重新配对
void BtSvcForgetDevice(const char* addr);

bool BtSvcRunning();

// 是否正在扫描
bool BtSvcScanActive();

// 当前阶段：0=待机 1=扫描中 2=连接中 3=推流中 4=失败
int BtSvcPhase();

// BTstack 线程是否存活
bool BtSvcThreadActive();

// 喂 PCM：交错立体声 int16，feedRate 为采样率（如 48000）。
// 内部会重采样到协商出的编码采样率。
void BtSvcFeedAudio(const short* pcm, int frames, int feedRate);

// 是否用内置测试音代替真实音频（验证链路用）
void BtSvcSetTestTone(bool on);
bool BtSvcTestTone();

// 推流音量（软件增益，作用于编码前的 PCM，1.0 = 原始电平，0 = 静音）
void BtSvcSetGain(float gain);
float BtSvcGetGain();

// 当前连接/推流中的设备地址（无连接返回空串）
const char* BtSvcCurrentAddr();

// 指定要使用的 USB 蓝牙适配器（VID/PID）。需重启服务线程后生效。
void BtSvcSetUsbDevice(unsigned vid, unsigned pid);

// 当前配置使用的适配器 VID/PID
void BtSvcGetUsbDevice(unsigned& vid, unsigned& pid);
