// WASAPI 回环采集：抓系统输出的声音副本，喂给 BTstack A2DP 推流
#include "loopcap.h"
#include "btsvc.h"

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>

#include <atomic>
#include <thread>
#include <vector>

#pragma comment(lib, "ole32.lib")

static std::atomic<bool> g_run{ false };
static std::thread       g_th;
static std::wstring      g_devId;

std::vector<LoopDevice> LoopEnumRenderDevices()
{
    std::vector<LoopDevice> out;
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&en)) || !en)
        return out;

    // 第一项：系统默认输出
    out.push_back({ L"", L"（系统默认输出）" });

    IMMDeviceCollection* col = nullptr;
    if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col)) && col) {
        UINT cnt = 0;
        col->GetCount(&cnt);
        for (UINT i = 0; i < cnt; ++i) {
            IMMDevice* dev = nullptr;
            if (FAILED(col->Item(i, &dev)) || !dev) continue;
            LPWSTR id = nullptr;
            dev->GetId(&id);
            IPropertyStore* ps = nullptr;
            std::wstring name = L"(unknown)";
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps)) && ps) {
                PROPVARIANT v; PropVariantInit(&v);
                if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.pwszVal)
                    name = v.pwszVal;
                PropVariantClear(&v);
                ps->Release();
            }
            LoopDevice d;
            d.id = id ? id : L"";
            d.name = name;
            out.push_back(d);
            if (id) CoTaskMemFree(id);
            dev->Release();
        }
        col->Release();
    }
    en->Release();
    return out;
}

static void CaptureLoop()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IMMDeviceEnumerator* en = nullptr;
    IMMDevice*           dev = nullptr;
    IAudioClient*        ac = nullptr;
    IAudioCaptureClient* cap = nullptr;
    WAVEFORMATEX*        fmt = nullptr;

    do {
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    __uuidof(IMMDeviceEnumerator), (void**)&en)) || !en) break;

        if (g_devId.empty()) {
            if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) || !dev) break;
        } else {
            if (FAILED(en->GetDevice(g_devId.c_str(), &dev)) || !dev) break;
        }

        if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac)) || !ac) break;
        if (FAILED(ac->GetMixFormat(&fmt)) || !fmt) break;

        // loopback 模式：采集渲染端点正在播放的内容
        // 缓冲降到 40ms（原 200ms），减少采集端积累的延迟
        REFERENCE_TIME dur = 400000; // 40ms
        if (FAILED(ac->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                  AUDCLNT_STREAMFLAGS_LOOPBACK, dur, 0, fmt, nullptr))) break;
        if (FAILED(ac->GetService(__uuidof(IAudioCaptureClient), (void**)&cap)) || !cap) break;
        if (FAILED(ac->Start())) break;

        int srcRate = (int)fmt->nSamplesPerSec;
        int srcCh   = fmt->nChannels;
        bool isFloat = (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
                       (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                        ((WAVEFORMATEXTENSIBLE*)fmt)->SubFormat.Data1 == 3);
        int bits = fmt->wBitsPerSample;

        std::vector<short> pcm;

        while (g_run.load()) {
            UINT32 packet = 0;
            if (FAILED(cap->GetNextPacketSize(&packet)) ) { Sleep(2); continue; }
            if (packet == 0) { Sleep(2); continue; }

            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) { Sleep(2); continue; }

            pcm.resize((size_t)frames * 2);   // 输出固定立体声

            bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            for (UINT32 i = 0; i < frames; ++i) {
                short l = 0, r = 0;
                if (!silent && data) {
                    if (isFloat && bits == 32) {
                        const float* f = (const float*)(data + (size_t)i * srcCh * 4);
                        float fl = f[0];
                        float fr = (srcCh > 1) ? f[1] : f[0];
                        if (fl > 1) fl = 1; if (fl < -1) fl = -1;
                        if (fr > 1) fr = 1; if (fr < -1) fr = -1;
                        l = (short)(fl * 32767.0f);
                        r = (short)(fr * 32767.0f);
                    } else if (bits == 16) {
                        const short* s = (const short*)(data + (size_t)i * srcCh * 2);
                        l = s[0];
                        r = (srcCh > 1) ? s[1] : s[0];
                    }
                }
                pcm[2 * i]     = l;
                pcm[2 * i + 1] = r;
            }

            BtSvcFeedAudio(pcm.data(), (int)frames, srcRate);
            cap->ReleaseBuffer(frames);
        }

        ac->Stop();
    } while (0);

    if (fmt) CoTaskMemFree(fmt);
    if (cap) cap->Release();
    if (ac)  ac->Release();
    if (dev) dev->Release();
    if (en)  en->Release();
    CoUninitialize();
}

bool LoopCaptureStart(const std::wstring& deviceId, std::wstring& err)
{
    if (g_run.load()) return true;
    g_devId = deviceId;
    g_run.store(true);
    g_th = std::thread(CaptureLoop);
    return true;
}

void LoopCaptureStop()
{
    g_run.store(false);
    if (g_th.joinable()) g_th.join();
}

bool LoopCaptureRunning() { return g_run.load(); }
