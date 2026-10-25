#pragma once

// USB 蓝牙适配器枚举与驱动切换辅助
// 判定标准：USB 接口类 0xE0(无线控制器)/0x01(射频)/0x01(蓝牙)
// 状态：
//   WinUSB   —— 已绑定 WinUSB 驱动，本程序可直接使用
//   BTHUSB   —— 被 Windows 蓝牙栈占用，需要切换驱动后才能用

#include <windows.h>
#include <string>
#include <vector>

struct BtAdapter {
    std::wstring instanceId;   // 设备实例 ID（切换驱动时 remove-device 用）
    std::wstring path;         // 设备接口路径（WinUSB 打开用）
    std::wstring name;         // 友好名 / 设备描述
    std::wstring service;      // 当前驱动服务：BTHUSB / WinUSB / ...
    uint16_t vid = 0;
    uint16_t pid = 0;
    bool winusb = false;       // true = 可直接使用
    bool isBluetooth = false;  // 已确认是蓝牙适配器（E0/01/01）
};

// 枚举系统中所有 USB 蓝牙适配器（去重：同一设备多个接口只留一条，优先 WinUSB）
// curVid/curPid：当前被本程序占用（正在推流）的适配器——跳过描述符探测，直接视为蓝牙
std::vector<BtAdapter> AdapterEnumAll(uint16_t curVid = 0, uint16_t curPid = 0);

// 为指定 VID/PID 生成动态 INF（复制自模板并替换硬件 ID）
// 返回 false = 模板不存在
bool AdapterWriteInf(uint16_t vid, uint16_t pid, std::wstring& outInfPath);
