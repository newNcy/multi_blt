#pragma once

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#include <string>
#include <vector>
#include <atomic>
#include <memory>
#include <thread>

// 端点描述（用于命令行列出设备）
struct DeviceInfo {
    std::wstring id;
    std::wstring name;
};

// 枚举指定方向的活跃音频端点
std::vector<DeviceInfo> EnumDevices(EDataFlow flow);

// 单个输出端的实时统计
struct RenderStats {
    std::atomic<double>   bufferedMs{ 0.0 };      // 环形缓冲水位（毫秒）
    std::atomic<double>   ratio{ 1.0 };           // 当前重采样速率（时钟补偿量）
    std::atomic<unsigned> glitches{ 0 };          // 欠载/过冲导致的重同步次数
    std::atomic<double>   streamLatencyMs{ 0.0 }; // WASAPI 报告的流延迟
    std::atomic<double>   extraDelayMs{ 0.0 };    // 为对齐两耳机额外插入的延迟
    std::atomic<int>      outRate{ 0 };
};

struct EngineConfig {
    std::wstring captureId;          // 输入端点 ID
    bool         loopback = false;   // true 表示输入端点其实是渲染端点，用回环方式采集
    std::wstring outId[2];           // 输出端点 ID
    int          outCount = 2;       // 实际使用的输出端数量（1 或 2）
    double       targetBufferMs = 40.0;  // 环形缓冲目标水位（抗抖动，越小越跟手）
    double       renderPeriodMs = 0.0;   // 渲染端请求周期，0 表示交给音频引擎决定
    double       hwLatencyMs[2] = { 0.0, 0.0 }; // 蓝牙编解码器额外延迟，用于两耳机对齐
    bool         syncPair = true;    // 是否把两个耳机在时间上对齐
};

class DualEngine {
public:
    // 单写多读环形缓冲（实现在 engine.cpp）
    class Ring {
    public:
        void Init(int ch, int capFrames);
        int  Channels() const;
        int  Capacity() const;
        void Write(const float* s, int frames);
        uint64_t WritePos() const;
        float Get(int64_t idx, int c) const;
    private:
        int ch_ = 2;
        int cap_ = 0;
        std::vector<float> buf_;
        std::atomic<uint64_t> write_{ 0 };
    };

    ~DualEngine();

    bool Start(const EngineConfig& cfg, std::wstring& err);
    void Stop();

    const RenderStats& Stats(int i) const { return stats_[i]; }
    std::wstring OutName(int i) const { return outName_[i]; }
    int  CaptureRate() const { return inRate_; }
    bool IsRunning() const { return running_.load(); }
    // 采集或渲染线程非正常退出（例如耳机断连、设备被拔出）时置位
    bool Faulted() const { return faulted_.load(); }

private:
    struct Endpoint {
        IMMDevice*    dev = nullptr;
        IAudioClient* ac = nullptr;
        HANDLE        ev = nullptr;
        WAVEFORMATEX* fmt = nullptr;   // 实际使用的格式
        bool          float32 = false; // 数据是否为 IEEE float32
        UINT32        bufFrames = 0;
        REFERENCE_TIME latency = 0;
    };

    void CaptureLoop();
    void RenderLoop(int idx);

    bool OpenEndpoint(const std::wstring& id, bool isCapture, bool loopback,
                      REFERENCE_TIME reqDur, Endpoint& ep, std::wstring& err);
    void CloseEndpoint(Endpoint& ep);

    EngineConfig            cfg_;
    std::atomic<bool>       running_{ false };
    std::atomic<bool>       faulted_{ false };

    Endpoint                cap_;
    IAudioCaptureClient*    capClient_ = nullptr;
    Endpoint                outs_[2];
    IAudioRenderClient*     renderClient_[2] = { nullptr, nullptr };

    std::wstring            outName_[2];
    RenderStats             stats_[2];

    int                     inRate_ = 0;
    int                     inCh_ = 2;
    int                     outRate_[2] = { 0, 0 };
    int                     outCh_[2] = { 2, 2 };
    double                  piTarget_[2] = { 0, 0 };

    class Ring;
    std::unique_ptr<Ring>   ring_;

    std::thread             thCap_;
    std::thread             thOut_[2];
};
