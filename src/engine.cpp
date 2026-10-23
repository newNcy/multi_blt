#include "engine.h"
#include "btsvc.h"

#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <math.h>
#include <algorithm>
#include <thread>
#include <memory>
#include <vector>
#include <cstring>
#include <cstdio>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "avrt.lib")

// ---------------------------------------------------------------- 小工具

static bool IsFloatFormat(const WAVEFORMATEX* f)
{
    if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
        return ((const WAVEFORMATEXTENSIBLE*)f)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    return false;
}

static WAVEFORMATEX* CloneFormat(const WAVEFORMATEX* f)
{
    size_t n = sizeof(WAVEFORMATEX) + f->cbSize;
    BYTE* p = (BYTE*)CoTaskMemAlloc(n);
    if (!p) return nullptr;
    memcpy(p, f, n);
    return (WAVEFORMATEX*)p;
}

// 把线程注册到 MMCSS "Pro Audio"，降低音频线程被抢占的概率
static void EnableMMCSS(const wchar_t* task)
{
    DWORD idx = 0;
    HANDLE h = AvSetMmThreadCharacteristicsW(task, &idx);
    if (h) {
        // 句柄需要保持到线程结束，这里故意泄漏，进程退出时由系统回收
    }
    (void)h;
}

static std::wstring GetFriendlyName(IMMDevice* dev)
{
    std::wstring name = L"(未命名设备)";
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

// ---------------------------------------------------------------- 设备枚举

std::vector<DeviceInfo> EnumDevices(EDataFlow flow)
{
    std::vector<DeviceInfo> out;
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&en))) || !en)
        return out;

    IMMDeviceCollection* col = nullptr;
    if (SUCCEEDED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col)) && col) {
        UINT n = 0;
        col->GetCount(&n);
        for (UINT i = 0; i < n; ++i) {
            IMMDevice* d = nullptr;
            if (SUCCEEDED(col->Item(i, &d)) && d) {
                LPWSTR id = nullptr;
                if (SUCCEEDED(d->GetId(&id)) && id) {
                    DeviceInfo di;
                    di.id = id;
                    di.name = GetFriendlyName(d);
                    out.push_back(di);
                    CoTaskMemFree(id);
                }
                d->Release();
            }
        }
        col->Release();
    }
    en->Release();
    return out;
}

// ---------------------------------------------------------------- 环形缓冲

// 单写多读：写入端单调递增，每个读取端各自维护小数读位置。
void DualEngine::Ring::Init(int ch, int capFrames)
{
    ch_ = ch;
    cap_ = capFrames;
    buf_.assign((size_t)cap_ * (size_t)ch_, 0.0f);
    write_.store(0, std::memory_order_relaxed);
}

int DualEngine::Ring::Channels() const { return ch_; }
int DualEngine::Ring::Capacity() const { return cap_; }

void DualEngine::Ring::Write(const float* s, int frames)
{
    uint64_t w = write_.load(std::memory_order_relaxed);
    for (int i = 0; i < frames; ++i) {
        size_t base = (size_t)((w + (uint64_t)i) % (uint64_t)cap_) * (size_t)ch_;
        for (int c = 0; c < ch_; ++c)
            buf_[base + c] = s[(size_t)i * ch_ + c];
    }
    write_.store(w + (uint64_t)frames, std::memory_order_release);
}

uint64_t DualEngine::Ring::WritePos() const { return write_.load(std::memory_order_acquire); }

// 取第 idx 帧第 c 声道；越界按边界钳制，超出写入端的部分返回静音
float DualEngine::Ring::Get(int64_t idx, int c) const
{
    uint64_t w = write_.load(std::memory_order_acquire);
    int64_t oldest = (int64_t)w - cap_;
    if (idx < oldest) idx = oldest;
    if (idx >= (int64_t)w) return 0.0f;
    int64_t m = (idx - oldest) % cap_;   // idx-oldest ∈ [0, cap_]
    return buf_[(size_t)m * (size_t)ch_ + c];
}

// ---------------------------------------------------------------- 重采样

// 4 点 Catmull-Rom 三次插值
static inline float Cubic(float y0, float y1, float y2, float y3, double t)
{
    double a = -0.5 * y0 + 1.5 * y1 - 1.5 * y2 + 0.5 * y3;
    double b =  y0 - 2.5 * y1 + 2.0 * y2 - 0.5 * y3;
    double c = -0.5 * y0 + 0.5 * y2;
    return (float)(((a * t + b) * t + c) * t + y1);
}

struct ReaderState {
    double   pos = 0.0;        // 小数输入帧位置
    double   piTarget = 0.0;   // 目标水位（输入帧）
    double   integral = 0.0;   // PI 控制器积分项
    int      underrun = 0;
    unsigned glitches = 0;
    bool     primed = false;
};

// 时钟补偿参数
static const double kKp = 0.02;     // 比例增益：水位偏差 100% → 速率变化 2%
static const double kKi = 0.0004;   // 积分增益
static const double kIntegralLimit = 25.0;
static const double kRateMin = 0.97;
static const double kRateMax = 1.03;

// 从环形缓冲按 consume（每输出帧消耗的输入帧数）重采样出 frames 帧
static bool Pull(const DualEngine::Ring* ring, ReaderState& st,
                 float* dst, int chOut, int chIn, int frames, double consume)
{
    const int cap = ring->Capacity();
    const uint64_t w = ring->WritePos();

    int64_t avail = (int64_t)w - (int64_t)floor(st.pos);

    // 写入端已绕圈覆盖掉尚未播放的数据（读取端严重落后）→ 强制重同步
    if (avail > cap - 4) {
        st.pos = (double)w - st.piTarget;
        st.integral = 0.0;
        st.glitches++;
        avail = (int64_t)w - (int64_t)floor(st.pos);
    }

    const double need = (double)frames * consume + 3.0;
    if ((double)avail < need) {
        // 欠载：本周期输出静音，且不推进读位置，等缓冲补回来
        memset(dst, 0, (size_t)frames * (size_t)chOut * sizeof(float));
        if (++st.underrun > 40) {
            // 长时间跟不上（输出设备时钟明显偏快），硬重同步一次
            st.pos = (double)w - st.piTarget;
            st.underrun = 0;
            st.integral = 0.0;
            st.glitches++;
        }
        return false;
    }
    st.underrun = 0;

    double p = st.pos;
    for (int i = 0; i < frames; ++i) {
        int64_t ip = (int64_t)floor(p);
        double frac = p - ip;
        for (int c = 0; c < chOut; ++c) {
            int ci = (c < chIn) ? c : 0;
            dst[(size_t)i * chOut + c] =
                Cubic(ring->Get(ip - 1, ci), ring->Get(ip, ci),
                      ring->Get(ip + 1, ci), ring->Get(ip + 2, ci), frac);
        }
        p += consume;
    }
    st.pos = p;
    return true;
}

// ---------------------------------------------------------------- 格式转换

static void ToFloat(const BYTE* src, const WAVEFORMATEX* f, UINT32 frames, float* dst, int ch)
{
    const int nch = f->nChannels;
    if (!src) { memset(dst, 0, (size_t)frames * ch * sizeof(float)); return; }

    if (IsFloatFormat(f)) {
        const float* s = (const float*)src;
        for (UINT32 i = 0; i < frames; ++i)
            for (int c = 0; c < ch; ++c)
                dst[(size_t)i * ch + c] = (c < nch) ? s[(size_t)i * nch + c] : 0.0f;
        return;
    }

    const int bytes = f->wBitsPerSample / 8;
    if (bytes != 2 && bytes != 3 && bytes != 4) {
        memset(dst, 0, (size_t)frames * ch * sizeof(float));
        return;
    }
    const float scale = 1.0f / (float)(1u << (f->wBitsPerSample - 1));
    for (UINT32 i = 0; i < frames; ++i) {
        for (int c = 0; c < ch; ++c) {
            if (c >= nch) { dst[(size_t)i * ch + c] = 0.0f; continue; }
            const BYTE* p = src + ((size_t)i * nch + c) * bytes;
            int32_t v = 0;
            if (bytes == 2)      v = (int16_t)(p[0] | (p[1] << 8));
            else if (bytes == 3) v = (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16));
            else                 v = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                                               ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
            if (bytes == 3 && (v & 0x800000)) v |= ~0xFFFFFF;
            dst[(size_t)i * ch + c] = (float)v * scale;
        }
    }
}

static void FromFloat(const float* src, int ch, UINT32 frames, const WAVEFORMATEX* f, BYTE* dst)
{
    const int nch = f->nChannels;
    if (IsFloatFormat(f)) {
        float* d = (float*)dst;
        for (UINT32 i = 0; i < frames; ++i)
            for (int c = 0; c < nch; ++c) {
                float x = (c < ch) ? src[(size_t)i * ch + c] : 0.0f;
                d[(size_t)i * nch + c] = (x > 1.0f) ? 1.0f : (x < -1.0f) ? -1.0f : x;
            }
        return;
    }

    const int bytes = f->wBitsPerSample / 8;
    const float m = (float)((1u << (f->wBitsPerSample - 1)) - 1);
    for (UINT32 i = 0; i < frames; ++i) {
        for (int c = 0; c < nch; ++c) {
            float x = (c < ch) ? src[(size_t)i * ch + c] : 0.0f;
            if (x > 1.0f) x = 1.0f;
            if (x < -1.0f) x = -1.0f;
            int32_t v = (int32_t)(x * (x < 0.0f ? m + 1.0f : m));
            BYTE* p = dst + ((size_t)i * nch + c) * bytes;
            p[0] = (BYTE)(v & 0xFF);
            p[1] = (BYTE)((v >> 8) & 0xFF);
            if (bytes >= 3) p[2] = (BYTE)((v >> 16) & 0xFF);
            if (bytes == 4) p[3] = (BYTE)((v >> 24) & 0xFF);
        }
    }
}

// ---------------------------------------------------------------- 端点初始化

void DualEngine::CloseEndpoint(Endpoint& ep)
{
    if (ep.ac) { ep.ac->Release(); ep.ac = nullptr; }
    if (ep.dev) { ep.dev->Release(); ep.dev = nullptr; }
    if (ep.ev) { CloseHandle(ep.ev); ep.ev = nullptr; }
    if (ep.fmt) { CoTaskMemFree(ep.fmt); ep.fmt = nullptr; }
}

bool DualEngine::OpenEndpoint(const std::wstring& id, bool isCapture, bool loopback,
                              REFERENCE_TIME reqDur, Endpoint& ep, std::wstring& err)
{
    IMMDeviceEnumerator* en = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&en));
    if (FAILED(hr) || !en) { err = L"无法创建 MMDeviceEnumerator"; return false; }

    hr = en->GetDevice(id.c_str(), &ep.dev);
    en->Release();
    if (FAILED(hr) || !ep.dev) { err = L"打不开指定的音频端点"; return false; }

    hr = ep.dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ep.ac);
    if (FAILED(hr) || !ep.ac) { err = L"无法激活 IAudioClient"; return false; }

    // Windows 10+：声明为媒体类流，帮助音频引擎选择更合适的缓冲策略
    IAudioClient2* ac2 = nullptr;
    if (SUCCEEDED(ep.ac->QueryInterface(__uuidof(IAudioClient2), (void**)&ac2)) && ac2) {
        AudioClientProperties props = {};
        props.cbSize = sizeof(props);
        props.eCategory = AudioCategory_Media;
        props.Options = AUDCLNT_STREAMOPTIONS_NONE;
        ac2->SetClientProperties(&props);
        ac2->Release();
    }

    WAVEFORMATEX* mix = nullptr;
    hr = ep.ac->GetMixFormat(&mix);
    if (FAILED(hr) || !mix) { err = L"无法获取混音格式"; return false; }

    DWORD baseFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    if (loopback) baseFlags |= AUDCLNT_STREAMFLAGS_LOOPBACK;

    ep.ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // 优先请求 float32，并让音频引擎自动做格式转换，这样内部一律按 float 处理
    WAVEFORMATEXTENSIBLE fx = {};
    fx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    fx.Format.nChannels = mix->nChannels;
    fx.Format.nSamplesPerSec = mix->nSamplesPerSec;
    fx.Format.wBitsPerSample = 32;
    fx.Format.nBlockAlign = (WORD)(mix->nChannels * 4);
    fx.Format.nAvgBytesPerSec = mix->nSamplesPerSec * fx.Format.nBlockAlign;
    fx.Format.cbSize = 22;
    fx.Samples.wValidBitsPerSample = 32;
    fx.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    fx.dwChannelMask = 0x3;

    DWORD convFlags = baseFlags | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM;
    hr = ep.ac->Initialize(AUDCLNT_SHAREMODE_SHARED, convFlags, reqDur, 0,
                           (WAVEFORMATEX*)&fx, nullptr);
    if (SUCCEEDED(hr)) {
        ep.fmt = CloneFormat((WAVEFORMATEX*)&fx);
        ep.float32 = true;
    } else {
        // 退回混音格式，自己做 PCM <-> float 转换
        hr = ep.ac->Initialize(AUDCLNT_SHAREMODE_SHARED, baseFlags, reqDur, 0, mix, nullptr);
        if (FAILED(hr) && reqDur != 0)
            hr = ep.ac->Initialize(AUDCLNT_SHAREMODE_SHARED, baseFlags, 0, 0, mix, nullptr);
        if (FAILED(hr)) {
            err = L"IAudioClient::Initialize 失败 (0x";
            wchar_t buf[16];
            swprintf(buf, 16, L"%08lX)", (unsigned long)hr);
            err += buf;
            CoTaskMemFree(mix);
            return false;
        }
        ep.fmt = CloneFormat(mix);
        ep.float32 = IsFloatFormat(mix);
    }

    ep.ac->GetBufferSize(&ep.bufFrames);
    ep.ac->GetStreamLatency(&ep.latency);
    ep.ac->SetEventHandle(ep.ev);

    CoTaskMemFree(mix);
    (void)isCapture;
    return true;
}

// ---------------------------------------------------------------- 线程

void DualEngine::CaptureLoop()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    EnableMMCSS(L"Pro Audio");

    if (FAILED(cap_.ac->Start())) {
        faulted_.store(true);
        running_.store(false);
        CoUninitialize();
        return;
    }

    std::vector<float> tmp;
    std::vector<short> tmp16;
    while (running_.load()) {
        if (WaitForSingleObject(cap_.ev, 200) == WAIT_TIMEOUT) continue;

        UINT32 packet = 0;
        while (running_.load() && SUCCEEDED(capClient_->GetNextPacketSize(&packet)) && packet > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(capClient_->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;

            float* dst = nullptr;
            if (cap_.float32 && data) {
                dst = (float*)data;
            } else {
                tmp.assign((size_t)frames * (size_t)inCh_, 0.0f);
                ToFloat(data, cap_.fmt, frames, tmp.data(), inCh_);
                dst = tmp.data();
            }
            ring_->Write(dst, (int)frames);

            // 喂给用户态蓝牙推流（BTstack A2DP）
            if (BtSvcThreadActive()) {
                tmp16.resize((size_t)frames * (size_t)inCh_);
                for (size_t i = 0; i < (size_t)frames * (size_t)inCh_; ++i) {
                    float s = dst[i];
                    if (s > 1.0f) s = 1.0f;
                    if (s < -1.0f) s = -1.0f;
                    tmp16[i] = (short)(s * 32767.0f);
                }
                BtSvcFeedAudio(tmp16.data(), (int)frames, inRate_);
            }

            capClient_->ReleaseBuffer(frames);
        }
    }

    cap_.ac->Stop();
    CoUninitialize();
}

void DualEngine::RenderLoop(int idx)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    EnableMMCSS(L"Pro Audio");

    Endpoint& ep = outs_[idx];
    RenderStats& st = stats_[idx];
    IAudioRenderClient* rc = renderClient_[idx];

    ReaderState rs;
    rs.piTarget = piTarget_[idx];

    if (FAILED(ep.ac->Start())) {
        faulted_.store(true);
        running_.store(false);
        CoUninitialize();
        return;
    }

    std::vector<float> tmp;
    while (running_.load()) {
        if (WaitForSingleObject(ep.ev, 200) == WAIT_TIMEOUT) continue;

        UINT32 padding = 0;
        if (FAILED(ep.ac->GetCurrentPadding(&padding))) { faulted_.store(true); break; }
        UINT32 frames = ep.bufFrames - padding;
        if (frames == 0) continue;

        const uint64_t wp = ring_->WritePos();

        // 预填充：等缓冲攒到目标水位再开始播放，避免起播就欠载
        if (!rs.primed) {
            if ((double)wp < rs.piTarget + 64.0) continue;
            rs.pos = (double)wp - rs.piTarget;
            rs.primed = true;
        }

        double avail = (double)wp - rs.pos;

        // PI 控制器：水位高于目标 → 加快消耗；低于目标 → 放慢消耗
        double err = (avail - rs.piTarget) / rs.piTarget;
        rs.integral = std::clamp(rs.integral + err, -kIntegralLimit, kIntegralLimit);
        double ratio = std::clamp(1.0 + kKp * err + kKi * rs.integral, kRateMin, kRateMax);
        double consume = ((double)inRate_ / (double)outRate_[idx]) * ratio;

        BYTE* data = nullptr;
        if (FAILED(rc->GetBuffer(frames, &data))) break;

        float* dst = nullptr;
        if (ep.float32) {
            dst = (float*)data;
        } else {
            tmp.resize((size_t)frames * outCh_[idx]);
            dst = tmp.data();
        }

        bool ok = Pull(ring_.get(), rs, dst, outCh_[idx], inCh_, (int)frames, consume);

        if (!ep.float32) FromFloat(dst, outCh_[idx], frames, ep.fmt, data);

        rc->ReleaseBuffer(frames, ok ? 0 : AUDCLNT_BUFFERFLAGS_SILENT);

        st.bufferedMs.store(avail * 1000.0 / (double)inRate_);
        st.ratio.store(ratio);
        st.glitches.store(rs.glitches);
    }

    ep.ac->Stop();
    CoUninitialize();
}

// ---------------------------------------------------------------- 启动 / 停止

DualEngine::~DualEngine()
{
    Stop();
}

bool DualEngine::Start(const EngineConfig& cfg, std::wstring& err)
{
    cfg_ = cfg;

    REFERENCE_TIME reqDur = (cfg.renderPeriodMs > 0.0)
        ? (REFERENCE_TIME)(cfg.renderPeriodMs * 10000.0) : 0;

    if (!OpenEndpoint(cfg.captureId, true, cfg.loopback, reqDur, cap_, err)) return false;
    if (FAILED(cap_.ac->GetService(__uuidof(IAudioCaptureClient), (void**)&capClient_)) || !capClient_) {
        err = L"无法获取 IAudioCaptureClient";
        return false;
    }

    inRate_ = (int)cap_.fmt->nSamplesPerSec;
    inCh_ = cap_.fmt->nChannels;

    const int n = (cfg.outCount >= 1 && cfg.outCount <= 2) ? cfg.outCount : 2;

    for (int i = 0; i < n; ++i) {
        if (!OpenEndpoint(cfg.outId[i], false, false, reqDur, outs_[i], err)) return false;
        if (FAILED(outs_[i].ac->GetService(__uuidof(IAudioRenderClient),
                                           (void**)&renderClient_[i])) || !renderClient_[i]) {
            err = L"无法获取 IAudioRenderClient";
            return false;
        }
        outRate_[i] = (int)outs_[i].fmt->nSamplesPerSec;
        outCh_[i] = outs_[i].fmt->nChannels;
        outName_[i] = GetFriendlyName(outs_[i].dev);
        stats_[i].outRate.store(outRate_[i]);
        stats_[i].streamLatencyMs.store((double)outs_[i].latency / 10000.0);
    }

    // 两耳机对齐：让总延迟（流延迟 + 蓝牙编解码延迟）较大的一端不额外延迟，
    // 较小的一端补上差值，使同一帧音频同时到达两只耳朵
    double total[2] = { 0.0, 0.0 };
    double base = 0.0;
    for (int i = 0; i < n; ++i)
        total[i] = (double)outs_[i].latency / 10000.0 + cfg.hwLatencyMs[i];
    if (cfg.syncPair) {
        base = total[0];
        for (int i = 1; i < n; ++i) if (total[i] > base) base = total[i];
    }

    for (int i = 0; i < n; ++i) {
        double extra = cfg.syncPair ? (base - total[i]) : 0.0;
        stats_[i].extraDelayMs.store(extra);
        piTarget_[i] = (cfg.targetBufferMs + extra) * (double)inRate_ / 1000.0;
    }

    ring_.reset(new Ring());
    ring_->Init(inCh_, inRate_ * 2);   // 2 秒容量

    running_.store(true);
    thCap_ = std::thread(&DualEngine::CaptureLoop, this);
    for (int i = 0; i < n; ++i)
        thOut_[i] = std::thread(&DualEngine::RenderLoop, this, i);
    return true;
}

void DualEngine::Stop()
{
    if (!running_.exchange(false)) return;

    if (thCap_.joinable()) thCap_.join();
    if (thOut_[0].joinable()) thOut_[0].join();
    if (thOut_[1].joinable()) thOut_[1].join();

    if (capClient_) { capClient_->Release(); capClient_ = nullptr; }
    for (int i = 0; i < 2; ++i) {
        if (renderClient_[i]) { renderClient_[i]->Release(); renderClient_[i] = nullptr; }
        CloseEndpoint(outs_[i]);
    }
    CloseEndpoint(cap_);
    ring_.reset();
}
