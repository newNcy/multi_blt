// BTstack A2DP Source 服务实现：线程化 + 真实音频输入 + 双路推流
// 双路：同一个 dongle 同时向两副耳机推流（同一份音频，各自独立编码/发送）
#include "btsvc.h"

#include <Windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <atomic>
#include <thread>
#include <vector>
#include <map>
#include <string>

#include "btstack_config.h"
#include "btstack_event.h"
#include "btstack_memory.h"
#include "btstack_run_loop.h"
#include "btstack_run_loop_windows.h"
#include "classic/a2dp_source.h"
#include "classic/avdtp.h"
#include "classic/avdtp_util.h"
#include "classic/avrcp.h"
#include "classic/avrcp_controller.h"
#include "classic/avrcp_target.h"
#include "classic/btstack_sbc_bluedroid.h"
#include "classic/device_id_server.h"
#include "classic/sdp_server.h"
#include "bluetooth_company_id.h"
#include "hci.h"
#include "hci_dump.h"
#include "hci_dump_windows_fs.h"
#include "hci_transport.h"
#include "hci_transport_usb.h"

#define NUM_CHANNELS            2
#define NUM_STREAMS             6      // 多路推流（受无线电带宽限制，实际建议 ≤4 路）
#define AUDIO_TIMEOUT_MS        10
#define SBC_STORAGE_SIZE        1030
#define PREFERRED_SAMPLING_RATE 48000
#define RING_FRAMES             32768

// ------------------------------------------------------------ 状态

static std::thread            g_thread;
static std::atomic<bool>      g_running{ false };
static std::atomic<bool>      g_exitReq{ false };
static BtSvcStatusFn          g_cb = nullptr;
static BtSvcDeviceFn          g_devFn = nullptr;
static void*                  g_cbUser = nullptr;
static char                   g_addr[32] = "";
static std::atomic<bool>      g_testTone{ false };
static std::atomic<float>     g_gain{ 1.0f };   // 推流音量（软件增益）
static unsigned               g_usbVid = 0x0A12;
static unsigned               g_usbPid = 0x0001;
static std::atomic<int>       g_phaseIdx{ 0 };
static std::atomic<bool>      g_restartReq{ false };
static volatile bool          g_pairing = false;
static volatile bool          g_inAutoRepair = false;  // 0x05 自动修复配对进行中
static volatile bool          g_passiveMode = false;   // true=等耳机主动连（SSP 更易成功）
static char                   g_connectAddr[32] = "";
static std::atomic<bool>      g_cmdScan{ false };
static std::atomic<bool>      g_cmdConnect{ false };
static std::atomic<bool>      g_cmdPair{ false };
static std::atomic<bool>      g_cmdDisconnect{ false };
static std::atomic<DWORD>     g_disconnectTick{ 0 };   // 断开冷却：防止断开后立即误重连
static char                   g_rejectAddr[32] = "";   // 用户刚断开的地址：拒绝其自动重连
static std::atomic<DWORD>     g_rejectUntil{ 0 };
static volatile bool          g_pairForce = false;
static int                    g_pairTicks = 0;
static volatile bool          g_pairDelayConnect = false;
static int                    g_pairDelayTicks = 0;
static char                   g_pendingAddr[8][32] = {};  // 串行化排队：等当前路建流完成后再连下一路
static int                    g_pendingCount = 0;
static volatile bool          g_scanActive = false;
static volatile bool          g_btUp = false;
static std::atomic<bool>      g_runLoopUp{ false };   // run loop 是否存活（防跨线程误调用）
static int                    app_state = 0;    // 0=扫描选设备 1=已连接/建流

static DWORD WINAPI BtThread(LPVOID);

// 喂入的 PCM 环形缓冲（SPSC，int16 交错立体声，feedRate 采样率）—— 两路共享
static std::vector<short>     g_ring;
static std::atomic<uint64_t>  g_wpos{ 0 };      // 已写入的源帧数
static std::atomic<int>       g_feedRate{ 48000 };

static std::string AppFile(const char* name);   // 前置声明（定义在下方）
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

static void Post(const wchar_t* fmt, ...)
{
    wchar_t buf[256];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, 256, _TRUNCATE, fmt, ap);
    va_end(ap);

    // 同时写日志文件（诊断用，不会卡 UI）
    FILE* f = nullptr;
    if (_wfopen_s(&f, AppFileW(L"btsvc.log").c_str(), L"a, ccs=UTF-8") == 0 && f) {
        fwprintf(f, L"%s\n", buf);
        fclose(f);
    }

    if (!g_cb) return;
    g_cb(buf, g_cbUser);
}

// ------------------------------------------------------------ A2DP 双路实现

typedef struct {
    uint16_t a2dp_cid;
    char     addr[32];               // 对端地址
    uint8_t  local_seid;
    uint8_t  stream_opened;
    uint32_t time_audio_data_sent;
    uint32_t acc_num_missed_samples;
    uint32_t samples_ready;
    btstack_timer_source_t audio_timer;
    uint8_t  streaming;
    int      max_media_payload_size;
    uint32_t rtp_timestamp;
    uint8_t  sbc_storage[SBC_STORAGE_SIZE];
    uint16_t sbc_storage_count;
    uint8_t  sbc_ready_to_send;
    uint16_t avrcp_cid;
    double   phase;                  // 本路的消费位置（源帧单位）
    int      negRate;                // 本路协商采样率
    uint32_t pktCount;
    uint32_t byteCount;
    uint32_t dropCount;              // 发送未完成（缓冲满等）次数，保留数据待重试
} sending_ctx_t;

static sending_ctx_t            media[NUM_STREAMS];
static btstack_sbc_encoder_bluedroid_t sbc_encoder_state[NUM_STREAMS];
static const btstack_sbc_encoder_t * sbc_encoder_instance[NUM_STREAMS];
static btstack_packet_callback_registration_t hci_event_callback_registration;
static avdtp_stream_endpoint_t * local_stream_endpoint[NUM_STREAMS];
static hci_con_handle_t g_targetConHandle = HCI_CON_HANDLE_INVALID;

static uint8_t sdp_a2dp_source_service_buffer[150];
static uint8_t device_id_sdp_service_buffer[100];
static uint8_t sdp_avrcp_target_service_buffer[200];
static uint8_t sdp_avrcp_controller_service_buffer[200];

// ------------------------------------------------------------ 流索引辅助

static int FindStreamByCid(uint16_t cid)
{
    for (int i = 0; i < NUM_STREAMS; ++i)
        if (media[i].a2dp_cid == cid) return i;
    return -1;
}
static int FindStreamByAddr(const char* addr)
{
    for (int i = 0; i < NUM_STREAMS; ++i)
        if (media[i].addr[0] && _stricmp(media[i].addr, addr) == 0) return i;
    return -1;
}
static int FindFreeStream()
{
    for (int i = 0; i < NUM_STREAMS; ++i)
        if (media[i].a2dp_cid == 0 && !media[i].stream_opened) return i;
    return -1;
}
static void ResetStream(int i)
{
    // 注意：不能在这里调 btstack_run_loop_remove_timer —— 本函数会在
    // UI 线程（BtSvcStart）里被调用，此时 run loop 可能尚未初始化（断言崩溃）。
    // 重启时 run loop 是全新的、无残留定时器，纯清零即可。
    memset(&media[i], 0, sizeof(sending_ctx_t));
    media[i].negRate = PREFERRED_SAMPLING_RATE;
}
static bool AnyStreamActive()
{
    for (int i = 0; i < NUM_STREAMS; ++i)
        if (media[i].a2dp_cid != 0 || media[i].stream_opened) return true;
    return false;
}
// 是否有流还在建立中（信令已建但媒体流未开）—— btstack 的 SEP 发现一次只能跑一个，
// 两副耳机必须串行建立：第一路完全就绪（进入推流）后才发起第二路
static bool AnyStreamInSetup()
{
    for (int i = 0; i < NUM_STREAMS; ++i)
        if (media[i].a2dp_cid != 0 && !media[i].stream_opened) return true;
    return false;
}
static bool AnyStreamStreaming()
{
    for (int i = 0; i < NUM_STREAMS; ++i)
        if (media[i].stream_opened && media[i].streaming) return true;
    return false;
}

// ------------------------------------------------------------ AVRCP
// 耳机（TWS 尤其）常依赖 AVRCP：音量同步 + 播放状态。没有它可能保持静音。

typedef struct {
    uint32_t song_length_ms;
    uint32_t song_position_ms;
    avrcp_playback_status_t status;
} avrcp_play_status_info_t;
static avrcp_play_status_info_t play_info;

static void avrcp_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    (void)channel; (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) return;

    switch (packet[2]) {
    case AVRCP_SUBEVENT_CONNECTION_ESTABLISHED: {
        uint8_t st = avrcp_subevent_connection_established_get_status(packet);
        if (st != ERROR_CODE_SUCCESS) {
            Post(L"AVRCP: 连接失败 status=0x%02x", st);
            return;
        }
        uint16_t acid = avrcp_subevent_connection_established_get_avrcp_cid(packet);
        bd_addr_t addr;
        avrcp_subevent_connection_established_get_bd_addr(packet, addr);
        const char* astr = bd_addr_to_str(addr);

        // 找到对应的流（按地址匹配；找不到就挂到空闲流上）
        int i = FindStreamByAddr(astr);
        if (i < 0) i = FindFreeStream();
        if (i >= 0) {
            media[i].avrcp_cid = acid;
            if (!media[i].addr[0])
                strcpy_s(media[i].addr, sizeof(media[i].addr), astr);
        }
        Post(L"AVRCP: 已连接 [%d] cid=0x%04x", i, acid);
        avrcp_target_support_event(acid, AVRCP_NOTIFICATION_EVENT_PLAYBACK_STATUS_CHANGED);
        avrcp_target_support_event(acid, AVRCP_NOTIFICATION_EVENT_TRACK_CHANGED);
        avrcp_target_support_event(acid, AVRCP_NOTIFICATION_EVENT_NOW_PLAYING_CONTENT_CHANGED);
        // 关键：主动设置耳机绝对音量（有些 TWS 没收到这个命令就静音）
        avrcp_controller_set_absolute_volume(acid, 127);   // 100%
        avrcp_controller_enable_notification(acid, AVRCP_NOTIFICATION_EVENT_VOLUME_CHANGED);
        Post(L"AVRCP: 控制通道已建立 cid=0x%04x，已下发绝对音量 127", acid);
        break;
    }
    case AVRCP_SUBEVENT_CONNECTION_RELEASED: {
        uint16_t acid = avrcp_subevent_connection_released_get_avrcp_cid(packet);
        Post(L"AVRCP: 通道释放 cid=0x%04x", acid);
        for (int i = 0; i < NUM_STREAMS; ++i)
            if (media[i].avrcp_cid == acid) media[i].avrcp_cid = 0;
        break;
    }
    default:
        Post(L"AVRCP[?]: 未处理子事件 0x%02x", packet[2]);
        break;
    }
}

static void avrcp_target_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    (void)channel; (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) return;

    switch (packet[2]) {
    case AVRCP_SUBEVENT_PLAY_STATUS_QUERY: {
        // 查询方未指明是哪路；用任一活跃的 avrcp_cid 应答
        for (int i = 0; i < NUM_STREAMS; ++i) {
            if (media[i].avrcp_cid) {
                avrcp_target_play_status(media[i].avrcp_cid, play_info.song_length_ms,
                                         play_info.song_position_ms, play_info.status);
                break;
            }
        }
        break;
    }
    default:
        Post(L"AVRCP[target]: 未处理子事件 0x%02x", packet[2]);
        break;
    }
}

static void avrcp_controller_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    (void)channel; (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_AVRCP_META) return;
    switch (packet[2]) {
    case AVRCP_SUBEVENT_NOTIFICATION_VOLUME_CHANGED:
        Post(L"AVRCP: 耳机音量 %d%%",
             avrcp_subevent_notification_volume_changed_get_absolute_volume(packet) * 100 / 127);
        break;
    default:
        Post(L"AVRCP[ctrl]: 未处理子事件 0x%02x", packet[2]);
        break;
    }
}

static uint8_t media_sbc_codec_capabilities[] = {
    (AVDTP_SBC_44100 << 4) | AVDTP_SBC_48000 | AVDTP_SBC_JOINT_STEREO | AVDTP_SBC_STEREO,
    0xFF,
    2, 53
};
static uint8_t media_sbc_codec_configuration[4];

typedef struct {
    int num_channels, sampling_frequency, block_length, subbands;
    int min_bitpool_value, max_bitpool_value;
    btstack_sbc_channel_mode_t      channel_mode;
    btstack_sbc_allocation_method_t allocation_method;
} sbc_cfg_t;

static sbc_cfg_t sbc_configuration[NUM_STREAMS];

// ------------------------------------------------------------ 音频生产（按流）

static int RingAvailable() { return (int)(g_wpos.load() - (uint64_t)media[0].phase); }
static int RingAvailableIdx(int i) { return (int)(g_wpos.load() - (uint64_t)media[i].phase); }

static short RingSample(int64_t idx, int ch)
{
    int64_t slot = idx % RING_FRAMES;
    if (slot < 0) slot += RING_FRAMES;
    return g_ring[(size_t)slot * NUM_CHANNELS + ch];
}

// 从环形缓冲取一帧（线性插值重采样），不足返回 false
static bool PullFrameIdx(int i, short out[2])
{
    if (RingAvailableIdx(i) < 2) return false;
    int64_t i0 = (int64_t)media[i].phase;
    double frac = media[i].phase - (double)i0;
    for (int c = 0; c < NUM_CHANNELS; ++c) {
        short s0 = RingSample(i0, c);
        short s1 = RingSample(i0 + 1, c);
        out[c] = (short)(s0 + (s1 - s0) * frac);
    }
    media[i].phase += (double)g_feedRate.load() / (double)media[i].negRate;
    return true;
}

static void ProduceAudioIdx(int i, int16_t * pcm, int num_frames)
{
    // 防止积累延迟：超过 ~200ms 才跳到 ~20ms 处（阈值太紧会因喂入抖动频繁跳变，
    // 每次跳变 = 重采样位置瞬移 = 可闻咔哒声；200ms 内的波动让它自然流过）
    // 必须放在最前：测试音期间回环采集若仍在喂 ring，不跳转的话水位会积压到几十秒
    int rate = media[i].negRate > 0 ? media[i].negRate : 44100;
    int hi = rate / 5;     // ~200ms
    int lo = rate / 50;    // ~20ms
    if (RingAvailableIdx(i) > hi) {
        media[i].phase = (double)(g_wpos.load() - (uint64_t)lo);
    }

    if (g_testTone.load()) {
        static double phase = 0.0;
        static bool logged = false;
        int rate = media[i].negRate > 0 ? media[i].negRate : 44100;
        double step = 2.0 * 3.14159265358979 * 441.0 / rate;
        int16_t mx = 0;
        for (int k = 0; k < num_frames; ++k) {
            int16_t v = (int16_t)(sin(phase) * 20000.0);
            pcm[2 * k] = v;
            pcm[2 * k + 1] = v;
            int16_t a = v > 0 ? v : (int16_t)-v;
            if (a > mx) mx = a;
            phase += step;
            if (phase > 2.0 * 3.14159265358979) phase -= 2.0 * 3.14159265358979;
        }
        if (!logged && num_frames > 0) {
            logged = true;
            Post(L"测试音：样本幅度峰值 %d", mx);
        }
        return;
    }

    short f[2];
    for (int k = 0; k < num_frames; ++k) {
        if (PullFrameIdx(i, f)) {
            pcm[2 * k]     = f[0];
            pcm[2 * k + 1] = f[1];
        } else {
            pcm[2 * k] = 0;          // 欠载：静音
            pcm[2 * k + 1] = 0;
        }
    }
}

// ------------------------------------------------------------ 发送（按流）

static void send_media_packet(int i)
{
    int frame_len = sbc_encoder_instance[i]->sbc_buffer_length(&sbc_encoder_state[i]);
    uint8_t nframes = (uint8_t)(media[i].sbc_storage_count / frame_len);
    media[i].sbc_storage[0] = nframes;
    int r = a2dp_source_stream_send_media_payload_rtp(media[i].a2dp_cid, media[i].local_seid, 0,
                                              media[i].rtp_timestamp,
                                              media[i].sbc_storage, media[i].sbc_storage_count + 1);
    if (r != 0) {
        // 关键修复：包没发出去（如 BTSTACK_ACL_BUFFERS_FULL）时保留帧数据待重试。
        // 之前这里直接清空 = 丢帧；连续丢帧会让部分音箱（如 K3）停止输出。
        media[i].dropCount++;
        if ((media[i].dropCount % 20) == 1)
            Post(L"发送[%d]：发送未完成 ret=%d，保留 %u 字节待重试", i, r, media[i].sbc_storage_count);
        return;
    }
    media[i].rtp_timestamp += nframes * sbc_encoder_instance[i]->num_audio_frames(&sbc_encoder_state[i]);
    media[i].pktCount++;
    media[i].byteCount += media[i].sbc_storage_count + 1;
    if ((media[i].pktCount % 200) == 1)
        Post(L"发送[%d]：第 %u 包, ret=%d, 累计 %u 字节", i, media[i].pktCount, r, media[i].byteCount);
    if (media[i].pktCount <= 3) {
        // 诊断：RTP 头应为 0x80 0x60（PT=96），SBC 帧首字节应为 0x9C（同步字）
        wchar_t hex[200] = L"";
        int n = media[i].sbc_storage_count + 1;
        if (n > 24) n = 24;
        for (int k = 0; k < n; ++k)
            swprintf_s(hex + k * 3, 200 - k * 3, L"%02X ", media[i].sbc_storage[k]);
        Post(L"媒体包[%d]#%u: 帧长=%u 帧数=%u 总长=%u mtu=%u 前%d字节: %s",
             i, media[i].pktCount, frame_len, nframes,
             media[i].sbc_storage_count + 1, media[i].max_media_payload_size,
             n, hex);
    }
    media[i].sbc_storage_count = 0;
    media[i].sbc_ready_to_send = 0;
}

static void fill_sbc_buffer(int i)
{
    unsigned int spb = sbc_encoder_instance[i]->num_audio_frames(&sbc_encoder_state[i]);
    uint16_t flen = sbc_encoder_instance[i]->sbc_buffer_length(&sbc_encoder_state[i]);
    while (media[i].samples_ready >= spb
        && (media[i].max_media_payload_size - media[i].sbc_storage_count) >= flen) {
        int16_t pcm[256 * NUM_CHANNELS];
        ProduceAudioIdx(i, pcm, spb);
        // 软件增益：编码前缩放 PCM（推流音量，与系统音量无关，保证可控）
        float gain = g_gain.load();
        if (gain != 1.0f) {
            for (int k = 0; k < spb * NUM_CHANNELS; ++k) {
                int v = (int)(pcm[k] * gain);
                if (v > 32767) v = 32767;
                if (v < -32768) v = -32768;
                pcm[k] = (int16_t)v;
            }
        }
        sbc_encoder_instance[i]->encode_signed_16(&sbc_encoder_state[i], pcm,
                                               &media[i].sbc_storage[1 + media[i].sbc_storage_count]);
        media[i].sbc_storage_count += flen;
        media[i].samples_ready     -= spb;
    }
}

static void audio_timeout(btstack_timer_source_t * ts)
{
    int i = (int)(intptr_t)btstack_run_loop_get_timer_context(ts);
    if (i < 0 || i >= NUM_STREAMS) return;
    btstack_run_loop_set_timer(ts, AUDIO_TIMEOUT_MS);
    btstack_run_loop_add_timer(ts);

    uint32_t now = btstack_run_loop_get_time_ms();
    uint32_t period = AUDIO_TIMEOUT_MS;
    if (media[i].time_audio_data_sent > 0) period = now - media[i].time_audio_data_sent;
    uint32_t n = (period * media[i].negRate) / 1000;
    media[i].acc_num_missed_samples += (period * media[i].negRate) % 1000;
    while (media[i].acc_num_missed_samples >= 1000) {
        n++;
        media[i].acc_num_missed_samples -= 1000;
    }
    media[i].time_audio_data_sent = now;
    media[i].samples_ready += n;

    // 每 5 秒记录一次（诊断）
    static uint32_t lastLog = 0;
    if (now - lastLog >= 5000) {
        lastLog = now;
        static uint32_t lastPkt[NUM_STREAMS] = { 0, 0 };
        uint32_t dpkt = media[i].pktCount - lastPkt[i];
        lastPkt[i] = media[i].pktCount;
        int rate = g_feedRate.load();
        SYSTEMTIME st; GetLocalTime(&st);
        Post(L"[%02d:%02d:%02d][%d] 水位 %d ms | 包速率 %u/s",
             st.wHour, st.wMinute, st.wSecond, i,
             (int)((int64_t)RingAvailableIdx(i) * 1000 / (rate > 0 ? rate : 48000)),
             dpkt / 5);
    }

    if (media[i].sbc_ready_to_send) {
        // 有保留的帧等待发送（上次因缓冲满未完成）：每 10ms 重新请求一次 CAN_SEND
        a2dp_source_stream_endpoint_request_can_send_now(media[i].a2dp_cid, media[i].local_seid);
        return;
    }
    // 关键修复：不要发“编码后的静音”。流刚建立就持续发全零 SBC 帧，会让部分音箱
    // （如 K3）判定无信号而静音输出，之后即使真实音频到来也保持静音。
    // 没有测试音且 ring 里没有数据时保持静默不发，等真实音频到达再开始推流。
    if (!g_testTone.load() && RingAvailableIdx(i) <= 0) {
        media[i].time_audio_data_sent = btstack_run_loop_get_time_ms();  // 防止补样累积
        return;
    }
    fill_sbc_buffer(i);
    // 有帧就发（低延迟）：不再攒满大包，每 10ms 发一次当前帧
    if (media[i].sbc_storage_count > 0) {
        media[i].sbc_ready_to_send = 1;
        a2dp_source_stream_endpoint_request_can_send_now(media[i].a2dp_cid, media[i].local_seid);
    }
}

// ------------------------------------------------------------ 自愈引擎
// 目标地址一经设定，引擎全自动推进，永不停：
//   直连失败/密钥失效/PIN 陷阱 → 升级为全新配对 → 配对成功自动建流
//   配对超时（耳机不在）→ 每 15 秒重试配对（耳机进配对模式即自动接续）
// 用户无需任何手动恢复操作。

static volatile int  g_strategy = 0;     // 0=直连 1=全新配对
static int           g_lastBondTick = 0; // 两次配对间隔（防止狂刷）
static int           g_engineTick = 0;
static btstack_timer_source_t g_retryTimer;
static int                    g_retries = 0;
static char                   g_retryAddr[32] = "";

// 失败升级：切换到全新配对策略，断开残留链路，引擎会在下个周期自动执行
static void schedule_retry(const char* addr)
{
    if (addr && addr[0]) strcpy_s(g_retryAddr, sizeof(g_retryAddr), addr);
    g_strategy = 1;
    if (g_targetConHandle != HCI_CON_HANDLE_INVALID) {
        gap_disconnect(g_targetConHandle);
        g_targetConHandle = HCI_CON_HANDLE_INVALID;
    }
    Post(L"BTstack: 连接失败，已切换策略：全新配对（自动）");
}

static void start_streaming(int i)
{
    media[i].max_media_payload_size = btstack_min(a2dp_max_media_payload_size(media[i].a2dp_cid, media[i].local_seid),
                                               SBC_STORAGE_SIZE);
    media[i].sbc_storage_count = 0;
    media[i].sbc_ready_to_send = 0;
    media[i].streaming = 1;
    btstack_run_loop_remove_timer(&media[i].audio_timer);
    btstack_run_loop_set_timer_handler(&media[i].audio_timer, audio_timeout);
    btstack_run_loop_set_timer_context(&media[i].audio_timer, (void*)(intptr_t)i);
    btstack_run_loop_set_timer(&media[i].audio_timer, AUDIO_TIMEOUT_MS);
    btstack_run_loop_add_timer(&media[i].audio_timer);
}

// ------------------------------------------------------------ 事件

static void a2dp_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_A2DP_META) return;

    uint8_t status;
    uint16_t cid;

    switch (hci_event_a2dp_meta_get_subevent_code(packet)) {

    case A2DP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED:
        status = a2dp_subevent_signaling_connection_established_get_status(packet);
        cid    = a2dp_subevent_signaling_connection_established_get_a2dp_cid(packet);
        if (status != ERROR_CODE_SUCCESS) {
            Post(L"BTstack: A2DP 信令连接失败 status=0x%02x", status);
            // 0x0B = ACL 链路已存在：dongle 重启后残留了旧链路，先断开再重连
            if (status == ERROR_CODE_ACL_CONNECTION_ALREADY_EXISTS
                && g_targetConHandle != HCI_CON_HANDLE_INVALID) {
                Post(L"BTstack: 检测到残留链路，断开后重连");
                gap_disconnect(g_targetConHandle);
                g_targetConHandle = HCI_CON_HANDLE_INVALID;
            }
            int i = FindStreamByCid(cid);
            if (i >= 0) media[i].a2dp_cid = 0;
            schedule_retry(nullptr);
            break;
        }
        {
            bd_addr_t addr;
            a2dp_subevent_signaling_connection_established_get_bd_addr(packet, addr);
            const char* addrS = bd_addr_to_str(addr);
            // 用户刚手动断开的地址：拒绝其自动重连（音箱会自己重连建流）
            if (g_rejectAddr[0] && GetTickCount() < g_rejectUntil.load() &&
                _stricmp(addrS, g_rejectAddr) == 0) {
                Post(L"BTstack: 已拒绝 %S 的自动重连（用户刚断开）", addrS);
                a2dp_source_disconnect(cid);
                break;
            }
            int i = FindStreamByCid(cid);
            if (i < 0) i = FindFreeStream();
            if (i >= 0) {
                media[i].a2dp_cid = cid;
                strcpy_s(media[i].addr, sizeof(media[i].addr), addrS);
                // 主动发起 AVRCP：很多音箱（如 K3）不主动建 AVRCP，绝对音量/播放状态就同步不了
                if (media[i].avrcp_cid == 0) {
                    int r = avrcp_connect(addr, &media[i].avrcp_cid);
                    Post(L"BTstack: 主动发起 AVRCP 连接 %S ret=%d", media[i].addr, r);
                }
            }
        }
        g_pairing = false;
        g_phaseIdx.store(2);
        Post(L"BTstack: A2DP 信令已连接（%s）",
             g_passiveMode ? L"耳机主动连入" : L"我方发起");
        break;

    case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CONFIGURATION:
        cid = a2dp_subevent_signaling_media_codec_sbc_configuration_get_a2dp_cid(packet);
        {
            int i = FindStreamByCid(cid);
            if (i < 0) return;
            sbc_cfg_t & c = sbc_configuration[i];
            c.num_channels       = a2dp_subevent_signaling_media_codec_sbc_configuration_get_num_channels(packet);
            c.sampling_frequency = a2dp_subevent_signaling_media_codec_sbc_configuration_get_sampling_frequency(packet);
            c.block_length       = a2dp_subevent_signaling_media_codec_sbc_configuration_get_block_length(packet);
            c.subbands           = a2dp_subevent_signaling_media_codec_sbc_configuration_get_subbands(packet);
            c.min_bitpool_value  = a2dp_subevent_signaling_media_codec_sbc_configuration_get_min_bitpool_value(packet);
            c.max_bitpool_value  = a2dp_subevent_signaling_media_codec_sbc_configuration_get_max_bitpool_value(packet);
            c.allocation_method  = (btstack_sbc_allocation_method_t)
                (a2dp_subevent_signaling_media_codec_sbc_configuration_get_allocation_method(packet) - 1);
            switch (a2dp_subevent_signaling_media_codec_sbc_configuration_get_channel_mode(packet)) {
            case AVDTP_CHANNEL_MODE_JOINT_STEREO: c.channel_mode = SBC_CHANNEL_MODE_JOINT_STEREO; break;
            case AVDTP_CHANNEL_MODE_STEREO:       c.channel_mode = SBC_CHANNEL_MODE_STEREO; break;
            case AVDTP_CHANNEL_MODE_DUAL_CHANNEL: c.channel_mode = SBC_CHANNEL_MODE_DUAL_CHANNEL; break;
            default:                              c.channel_mode = SBC_CHANNEL_MODE_MONO; break;
            }
            media[i].negRate = c.sampling_frequency;
            const wchar_t* cm = L"?";
            switch (c.channel_mode) {
            case SBC_CHANNEL_MODE_JOINT_STEREO: cm = L"joint_stereo"; break;
            case SBC_CHANNEL_MODE_STEREO:       cm = L"stereo";       break;
            case SBC_CHANNEL_MODE_DUAL_CHANNEL: cm = L"dual";         break;
            default:                            cm = L"mono";         break;
            }
            Post(L"AVDTP[%d]: SBC 已协商 %uHz %s 块%d 子带%d 分配%d bitpool%d->%d -> 建立媒体通道…",
                 i, c.sampling_frequency, cm, c.block_length, (int)a2dp_subevent_signaling_media_codec_sbc_configuration_get_subbands(packet),
                 (int)a2dp_subevent_signaling_media_codec_sbc_configuration_get_allocation_method(packet) - 1,
                 (int)a2dp_subevent_signaling_media_codec_sbc_configuration_get_min_bitpool_value(packet),
                 c.max_bitpool_value);
            sbc_encoder_instance[i] = btstack_sbc_encoder_bluedroid_init_instance(&sbc_encoder_state[i]);
            sbc_encoder_instance[i]->configure(&sbc_encoder_state[i], SBC_MODE_STANDARD,
                                            c.block_length, c.subbands,
                                            c.allocation_method,
                                            c.sampling_frequency,
                                            c.max_bitpool_value,
                                            c.channel_mode);
        }
        break;

    case A2DP_SUBEVENT_STREAM_ESTABLISHED:
        status = a2dp_subevent_stream_established_get_status(packet);
        cid    = a2dp_subevent_stream_established_get_a2dp_cid(packet);
        {
            int i = FindStreamByCid(cid);
            if (i < 0) return;
            if (status != ERROR_CODE_SUCCESS) {
                Post(L"BTstack[%d]: 建流失败 status=0x%02x", i, status);
                schedule_retry(media[i].addr);
                break;
            }
            media[i].stream_opened = 1;
            a2dp_source_start_stream(media[i].a2dp_cid, media[i].local_seid);
        }
        break;

    case A2DP_SUBEVENT_STREAM_STARTED:
        cid = a2dp_subevent_stream_started_get_a2dp_cid(packet);
        {
            int i = FindStreamByCid(cid);
            if (i < 0) return;
            g_running.store(true);
            g_phaseIdx.store(3);
            g_retries = 0;
            g_strategy = 0;
            Post(L"BTstack[%d]: 正在推流", i);
            int nStreaming = 0;
            for (int k = 0; k < NUM_STREAMS; ++k)
                if (media[k].stream_opened && media[k].streaming) nStreaming++;
            if (nStreaming >= 3)
                Post(L"BTstack: 已有 %d 路推流 —— 多路共享无线带宽，路数越多越可能卡顿", nStreaming);
            play_info.status = AVRCP_PLAYBACK_STATUS_PLAYING;
            if (media[i].avrcp_cid) {
                avrcp_target_set_playback_status(media[i].avrcp_cid, AVRCP_PLAYBACK_STATUS_PLAYING);
                avrcp_controller_set_absolute_volume(media[i].avrcp_cid, 127);   // 流启动后重发音量
            }
            start_streaming(i);
        }
        break;

    case A2DP_SUBEVENT_STREAM_SUSPENDED:
        cid = a2dp_subevent_stream_suspended_get_a2dp_cid(packet);
        {
            int i = FindStreamByCid(cid);
            if (i < 0) return;
            Post(L"BTstack[%d]: 暂停", i);
            play_info.status = AVRCP_PLAYBACK_STATUS_PAUSED;
            if (media[i].avrcp_cid)
                avrcp_target_set_playback_status(media[i].avrcp_cid, AVRCP_PLAYBACK_STATUS_PAUSED);
            btstack_run_loop_remove_timer(&media[i].audio_timer);
            media[i].streaming = 0;
        }
        break;

    case A2DP_SUBEVENT_STREAM_RELEASED:
        cid = a2dp_subevent_stream_released_get_a2dp_cid(packet);
        {
            int i = FindStreamByCid(cid);
            if (i < 0) return;
            Post(L"BTstack[%d]: 流已释放", i);
            media[i].stream_opened = 0;
            media[i].streaming = 0;
            btstack_run_loop_remove_timer(&media[i].audio_timer);
            if (!AnyStreamStreaming()) g_running.store(false);
        }
        break;

    case A2DP_SUBEVENT_STREAMING_CAN_SEND_MEDIA_PACKET_NOW:
        cid = a2dp_subevent_streaming_can_send_media_packet_now_get_a2dp_cid(packet);
        {
            int i = FindStreamByCid(cid);
            if (i < 0) return;
            if (media[i].sbc_storage_count > 0) send_media_packet(i);
            if (media[i].streaming && media[i].stream_opened) {
                fill_sbc_buffer(i);
                if (media[i].sbc_storage_count > 0) {
                    media[i].sbc_ready_to_send = 1;
                    a2dp_source_stream_endpoint_request_can_send_now(media[i].a2dp_cid, media[i].local_seid);
                }
            }
        }
        break;

    case A2DP_SUBEVENT_SIGNALING_CONNECTION_RELEASED: {
        // 信令通道被对端关闭（或我方断开）：清流状态，否则残留 cid 影响重连
        cid = a2dp_subevent_signaling_connection_released_get_a2dp_cid(packet);
        int i = FindStreamByCid(cid);
        Post(L"BTstack[%d]: A2DP 信令断开 cid=0x%04x", i, cid);
        if (i >= 0) {
            media[i].a2dp_cid   = 0;
            media[i].stream_opened = 0;
            media[i].streaming  = 0;
            btstack_run_loop_remove_timer(&media[i].audio_timer);
            if (!AnyStreamStreaming()) g_running.store(false);
        }
        break;
    }

    default:
        // 诊断：把所有未处理子事件打出来，避免错误被静默吞掉
        Post(L"A2DP[?]: 未处理子事件 0x%02x", hci_event_a2dp_meta_get_subevent_code(packet));
        break;
    }
}

// ------------------------------------------------------------ 链路密钥库（持久化到文件）
// 配对成功一次就把密钥存盘；之后连接直接用密钥，不再配对 —— 和 Windows 一样秒连。

struct LkEntry { link_key_t key; link_key_type_t type; };
static std::map<std::string, LkEntry> g_lkMap;     // key: "XX:XX:XX:XX:XX:XX"
static const char * k_lkFile = "linkkeys.txt";

// 状态文件统一放 exe 所在目录（避免从不同工作目录启动时读写分裂）
static std::string AppFile(const char* name)
{
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir(exe);
    size_t p = dir.find_last_of("\\/");
    dir = dir.substr(0, p + 1);
    return dir + name;
}
static const std::string k_lkFilePath = AppFile("linkkeys.txt");

static void LkLoad()
{
    g_lkMap.clear();
    // 密钥完全来自持久化文件：首次用 SSP 全新配对生成并保存，之后免配对直连。
    // （不内置 Windows 注册表导出的密钥 —— 那是加密 blob，不是明文，用不了。）

    FILE* f = nullptr;
    if (fopen_s(&f, k_lkFilePath.c_str(), "r") != 0 || !f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char addr[24]; unsigned k[16]; unsigned t = 0;
        int n = sscanf_s(line, "%23s %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %u",
                         addr, (unsigned)sizeof(addr),
                         &k[0],&k[1],&k[2],&k[3],&k[4],&k[5],&k[6],&k[7],
                         &k[8],&k[9],&k[10],&k[11],&k[12],&k[13],&k[14],&k[15], &t);
        if (n < 18) continue;
        LkEntry e;
        for (int i = 0; i < 16; ++i) e.key[i] = (uint8_t)k[i];
        e.type = (link_key_type_t)t;
        g_lkMap[addr] = e;
    }
    fclose(f);
    char buf[64];
    snprintf(buf, sizeof(buf), "loaded %u link keys", (unsigned)g_lkMap.size());
    Post(L"BTstack: 密钥库 %S", buf);
}

static void LkSave()
{
    FILE* f = nullptr;
    if (fopen_s(&f, k_lkFilePath.c_str(), "w") != 0 || !f) return;
    for (auto& kv : g_lkMap) {
        fprintf(f, "%s", kv.first.c_str());
        for (int i = 0; i < 16; ++i) fprintf(f, " %02x", kv.second.key[i]);
        fprintf(f, " %u\n", (unsigned)kv.second.type);
    }
    fclose(f);
}

static std::string LkKeyStr(const bd_addr_t addr)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
    return std::string(buf);
}

static int lk_get(bd_addr_t addr, link_key_t key, link_key_type_t * type)
{
    auto it = g_lkMap.find(LkKeyStr(addr));
    if (it == g_lkMap.end()) return 0;
    memcpy(key, it->second.key, 16);
    *type = it->second.type;
    Post(L"BTstack: 使用已存密钥 %S（免配对）", LkKeyStr(addr).c_str());
    return 1;
}
static void lk_put(bd_addr_t addr, link_key_t key, link_key_type_t type)
{
    LkEntry e;
    memcpy(e.key, key, 16);
    e.type = type;
    g_lkMap[LkKeyStr(addr)] = e;
    LkSave();
    Post(L"BTstack: 与 %S 配对成功，密钥已保存", LkKeyStr(addr).c_str());
}
static void lk_del(bd_addr_t addr) { g_lkMap.erase(LkKeyStr(addr)); LkSave(); }
static void lk_open(void) {}
static void lk_close(void) {}
static void lk_set_local(bd_addr_t a) {}
static int  lk_iter_init(btstack_link_key_iterator_t * it) { (void)it; return 0; }
static int  lk_iter_next(btstack_link_key_iterator_t * it, bd_addr_t a, link_key_t k, link_key_type_t * t) { (void)it;(void)a;(void)k;(void)t; return 0; }
static void lk_iter_done(btstack_link_key_iterator_t * it) { (void)it; }

static const btstack_link_key_db_t lk_db = {
    lk_open, lk_set_local, lk_close, lk_get, lk_put, lk_del,
    lk_iter_init, lk_iter_next, lk_iter_done,
};

// ------------------------------------------------------------ HCI 事件

static void hci_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    if (packet_type != HCI_EVENT_PACKET) return;
    uint8_t evt = hci_event_packet_get_type(packet);

    // 栈状态：置位/复位 g_btUp，接续用户在就绪前发出的扫描/连接请求
    if (evt == BTSTACK_EVENT_STATE) {
        if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING) {
            if (g_btUp) {
                g_btUp = false;
                Post(L"BTstack: 协议栈已停止（USB 移除或异常）");
            }
            return;
        }
        g_btUp = true;
        bd_addr_t a;
        gap_local_bd_addr(a);
        Post(L"BTstack: 已启动 %S", bd_addr_to_str(a));

        if (g_addr[0]) {
            int k = FindFreeStream();
            if (k >= 0) {
                strcpy_s(media[k].addr, sizeof(media[k].addr), g_addr);
                bd_addr_t target;
                sscanf_bd_addr(g_addr, target);
                a2dp_source_establish_stream(target, &media[k].a2dp_cid);
            }
        } else if (g_scanActive) {
            gap_inquiry_start(8);
            Post(L"BTstack: 扫描已开始");
        } else {
            Post(L"BTstack: 就绪，点「扫描」查找耳机");
        }
        return;
    }

    // 记录与目标耳机的 ACL 连接句柄（0x0B=链路已存在时用它断开残留链路）
    if (evt == HCI_EVENT_CONNECTION_COMPLETE) {
        bd_addr_t addr;
        hci_event_connection_complete_get_bd_addr(packet, addr);
        uint8_t st = hci_event_connection_complete_get_status(packet);
        if (st == ERROR_CODE_SUCCESS && g_connectAddr[0]
            && _stricmp(bd_addr_to_str(addr), g_connectAddr) == 0) {
            g_targetConHandle = hci_event_connection_complete_get_connection_handle(packet);
        }
        return;
    }
    if (evt == HCI_EVENT_DISCONNECTION_COMPLETE) {
        g_targetConHandle = HCI_CON_HANDLE_INVALID;
        return;
    }

    if (evt == HCI_EVENT_PIN_CODE_REQUEST) {
        bd_addr_t addr;
        hci_event_pin_code_request_get_bd_addr(packet, addr);
        // PIN 陷阱：耳机存有旧绑但我们对不上 → 传统 PIN 必失败。
        // 照常应答 0000（让链路正常失败），同时升级策略为全新配对。
        g_strategy = 1;
        g_pairing = true;
        Post(L"BTstack: PIN 配对请求（耳机有旧绑定）-> 升级为全新配对");
        gap_pin_code_response(addr, "0000");
        return;
    }
    if (evt == HCI_EVENT_LINK_KEY_NOTIFICATION) {
        g_pairing = false;
        Post(L"BTstack: 收到链路密钥（配对完成）");
        return;
    }
    if (evt == HCI_EVENT_AUTHENTICATION_COMPLETE) {
        uint8_t st = packet[2];
        Post(L"BTstack: 鉴权完成 status=0x%02x", st);
        if (st == 0x05 && g_connectAddr[0] && !g_pairing) {
            // 密钥失效（耳机侧密钥被更新，常见于耳机刚用 Windows/手机连过）。
            // 像 Windows 一样自动修复：重新配对 -> 成功后自动接续连接推流。
            Post(L"BTstack: 密钥失效，自动重新配对修复…");
            g_inAutoRepair = true;
            g_retries = 99;          // 停掉重试循环，避免与配对抢链路
            g_scanActive = false;    // 停扫描，避免与配对抢控制器
            gap_inquiry_stop();
            g_pairForce = true;      // 触发配对流程（备份->删密钥->SSP->自动接续）
            g_pairTicks = 0;
        } else if (st != 0) {
            g_pairing = false;       // 鉴权失败，解冻以便重试
        }
        return;
    }
    if (evt == HCI_EVENT_SIMPLE_PAIRING_COMPLETE) {
        g_pairing = false;
        Post(L"BTstack: SSP 配对完成");
        return;
    }
    if (evt == GAP_EVENT_DEDICATED_BONDING_COMPLETED) {
        uint8_t st = gap_event_dedicated_bonding_completed_get_status(packet);
        g_pairing = false;
        if (st == ERROR_CODE_SUCCESS) {
            g_inAutoRepair = false;
            // dedicated bonding 完成后会断开那条 ACL，必须等链路彻底断干净
            // 再建流，否则用残留链路认证会 0x05 失败。延迟 2.4 秒。
            Post(L"BTstack: 配对成功！等链路断开后自动建流…");
            g_pairDelayConnect = true;
            g_pairDelayTicks = 0;
        } else if (g_inAutoRepair) {
            // 自动修复失败：旧密钥已确认失效，删掉它（恢复它只会永远 0x05），
            // 并停止自动循环，等用户重新触发
            g_inAutoRepair = false;
            Post(L"BTstack: 自动修复失败 status=0x%02x，已删除失效密钥", st);
            BtSvcForgetDevice(g_connectAddr);
            g_connectAddr[0] = 0;
            g_phaseIdx.store(AnyStreamStreaming() ? 3 : 4);
        } else {
            Post(L"BTstack: 配对失败 status=0x%02x，已恢复原密钥", st);
            // 恢复配对前的密钥，避免失败把密钥弄丢。
            // 耳机不在可连接状态（page 超时）时不放弃：引擎 15 秒后自动再试，
            // 耳机一旦进入配对模式即自动接续。
            CopyFileA(AppFile("linkkeys.bak").c_str(), k_lkFilePath.c_str(), FALSE);
            LkLoad();
            g_phaseIdx.store(AnyStreamStreaming() ? 3 : 0);
        }
        return;
    }

    if (evt == GAP_EVENT_INQUIRY_RESULT) {
        bd_addr_t addr;
        gap_event_inquiry_result_get_bd_addr(packet, addr);
        uint32_t cod = gap_event_inquiry_result_get_class_of_device(packet);
        char name[240] = "";
        if (gap_event_inquiry_result_get_name_available(packet)) {
            int nl = gap_event_inquiry_result_get_name_len(packet);
            memcpy(name, gap_event_inquiry_result_get_name(packet), nl);
            name[nl] = 0;
        }
        if (g_devFn) g_devFn(bd_addr_to_str(addr), name, cod, g_cbUser);
        {
            FILE* f = nullptr;
            if (_wfopen_s(&f, AppFileW(L"btsvc.log").c_str(), L"a, ccs=UTF-8") == 0 && f) {
                fwprintf(f, L"[scan] %S cod=0x%06x %S\n",
                         bd_addr_to_str(addr), cod, name[0] ? name : "");
                fclose(f);
            }
        }
        return;
    }

    if (evt == GAP_EVENT_INQUIRY_COMPLETE) {
        if (g_scanActive && app_state == 0) {
            gap_inquiry_start(8);   // 循环扫描
        } else {
            if (g_scanActive) g_phaseIdx.store(AnyStreamStreaming() ? 3 : 0);
            Post(L"BTstack: 扫描结束");
        }
        return;
    }

    if (evt == 0x18) {   // HCI Hardware Error：控制器/USB 异常
        Post(L"BTstack: 收到硬件错误事件（控制器或 USB 异常）");
        return;
    }
}

// ------------------------------------------------------------ 命令轮询（GUI → BTstack 线程）

static btstack_timer_source_t g_pollTimer;

// 撤销悬空的经典蓝牙寻呼（HCI_Create_Connection_Cancel）。
// 本 btstack 版本的 gap_connect_cancel() 只覆盖 LE 且包在 ENABLE_LE_CENTRAL 里，
// 经典蓝牙的 pending page 只能直接发 HCI 命令撤（l2cap.c 已处理其 Command Complete）。
// 必须在 btstack 线程内调用（poll_timer_handler / packet_handler 上下文）。
static void ClassicConnectCancel(const char* addr)
{
    if (!addr || !addr[0]) return;
    bd_addr_t target;
    sscanf_bd_addr(addr, target);
    hci_send_cmd(&hci_create_connection_cancel, target);
}

static void poll_timer_handler(btstack_timer_source_t * ts)
{
    // 看门狗：上电后 15 秒仍未就绪（USB 打开失败后 btstack 不会自动重试），
    // 重启整个栈（线程循环会重建传输层再试）。就绪后本计数停止累计。
    // 注意：无论是否触发，都必须把定时器挂回去，否则轮询链断掉、看门狗失效。
    if (!g_btUp && !g_exitReq.load()) {
        static int bootTicks = 0;
        if (++bootTicks >= 75) {          // 75 * 200ms = 15s
            bootTicks = 0;
            Post(L"BTstack: 15 秒未就绪（USB 打开失败），自动重试…");
            btstack_run_loop_set_timer(ts, 200);
            btstack_run_loop_add_timer(ts);
            g_restartReq.store(true);
            btstack_run_loop_trigger_exit();
            return;
        }
    }
    // 配对成功后延迟建流（等 dedicated bonding 的链路彻底断开）
    if (g_pairDelayConnect) {
        if (++g_pairDelayTicks >= 12) {   // 12 * 200ms = 2.4s
            g_pairDelayConnect = false;
            g_cmdConnect.store(true);
        }
    }

    // 自愈引擎：目标已设且完全空闲时，按策略每 5 秒自动推进（永不停，无需人工干预）
    {
        if (++g_engineTick >= 25) {       // 25 * 200ms = 5s
            g_engineTick = 0;
            if (g_connectAddr[0] && !AnyStreamActive() && !g_pairing && !g_pairForce
                && !g_pairDelayConnect && !g_inAutoRepair) {
                g_phaseIdx.store(2);
                if (g_strategy == 1) {
                    // 策略1：全新配对（配对失败=耳机不在，冷却 15s 后自动再试；
                    // 耳机一旦进入配对模式即自动接续）
                    static int bondCd = 0;
                    if (bondCd > 0) { bondCd--; }
                    else {
                        bondCd = 75;      // 15s 冷却
                        Post(L"BTstack[引擎]: 全新配对 %S（耳机进配对模式即自动接续）", g_connectAddr);
                        g_pairForce = true;
                        g_pairTicks = 0;
                    }
                } else {
                    // 策略0：直连（复用统一连接入口，包含端点懒创建等处理）
                    Post(L"BTstack[引擎]: 尝试直连 %S", g_connectAddr);
                    g_cmdConnect.store(true);
                }
            }
        }
    }

    if (g_cmdScan.exchange(false)) {
        g_scanActive = true;
        app_state = 0;
        g_phaseIdx.store(1);
        if (g_btUp) {
            // 关键：撤销悬空的连接请求。上一次失败可能留下未完成的 create_connection
            // （ACL 永远不完成，handle=0xffff），BR/EDR 控制器 inquiry 与 page 共用基带，
            // pending page 不撤销，inquiry 实际执行不了 → 扫描结果恒为空
            g_pairing = false;
            g_pairForce = false;
            ClassicConnectCancel(g_connectAddr);
            gap_inquiry_start(8);
            Post(L"BTstack: 扫描已开始");
            if (AnyStreamStreaming())
                Post(L"BTstack: 推流中扫描 —— 已连接的设备不会出现在结果里（它们不可发现）");
        } else {
            Post(L"BTstack: 扫描请求已排队，等待适配器就绪…");
        }
    }
    if (g_cmdDisconnect.exchange(false)) {
        for (int i = 0; i < NUM_STREAMS; ++i) {
            if (media[i].a2dp_cid != 0) {
                Post(L"BTstack[%d]: 断开 A2DP cid=0x%04x", i, media[i].a2dp_cid);
                // 先告诉对端播放已停止，否则音箱会趁链路未断自动重开 AVDTP 流（假断开）
                if (media[i].avrcp_cid)
                    avrcp_target_set_playback_status(media[i].avrcp_cid, AVRCP_PLAYBACK_STATUS_STOPPED);
                a2dp_source_disconnect(media[i].a2dp_cid);
            }
            // 连 ACL 链路一起断开：只关 AVDTP 时音箱会立刻重开流
            if (media[i].addr[0]) {
                bd_addr_t daddr;
                sscanf_bd_addr(media[i].addr, daddr);
                hci_connection_t* conn = hci_connection_for_bd_addr_and_type(daddr, BD_ADDR_TYPE_ACL);
                if (conn) {
                    Post(L"BTstack[%d]: 断开 ACL 链路 handle=0x%04x", i, conn->con_handle);
                    gap_disconnect(conn->con_handle);
                }
            }
            btstack_run_loop_remove_timer(&media[i].audio_timer);
            // 记入拒绝名单：该设备（已配对）会自动重连建流，8 秒内一律拒绝
            if (media[i].addr[0]) {
                strcpy_s(g_rejectAddr, sizeof(g_rejectAddr), media[i].addr);
                g_rejectUntil.store(GetTickCount() + 8000);
            }
            media[i].a2dp_cid = 0;
            media[i].stream_opened = 0;
            media[i].streaming = 0;
            media[i].addr[0] = 0;
        }
        if (!AnyStreamStreaming()) g_running.store(false);
        // 用户主动断开 = 停止引擎对当前目标的一切尝试
        g_connectAddr[0] = 0;
        g_pendingCount = 0;
        g_strategy = 0;
        g_phaseIdx.store(0);
    }
    if (g_cmdPair.exchange(false)) {
        if (g_connectAddr[0]) {
            g_scanActive = false;
            g_passiveMode = false;
            gap_inquiry_stop();
            g_pairForce = true;         // 标记：inquiry 停稳后再发起配对
            g_pairTicks = 0;
        }
    }
    // inquiry 停止后延迟几轮再发起配对，避免控制器忙导致 page 发不出
    if (g_pairForce) {
        // 先断开与目标的残留 ACL（0x0b：链路已存在会直接导致配对失败）
        if (g_targetConHandle != HCI_CON_HANDLE_INVALID) {
            gap_disconnect(g_targetConHandle);
            g_targetConHandle = HCI_CON_HANDLE_INVALID;
            g_pairTicks = 0;            // 断开后再等一轮，让链路干净
            btstack_run_loop_set_timer(ts, 200);
            btstack_run_loop_add_timer(ts);
            return;
        }
        if (++g_pairTicks >= 3) {       // 约 600ms 后
            g_pairForce = false;
            g_pairing = true;
            g_scanActive = false;       // 强制停扫描，避免与配对抢控制器
            gap_inquiry_stop();
            g_phaseIdx.store(2);
            // 备份密钥：dedicated bonding 会先删密钥，若配对失败则恢复，避免密钥反复丢失
            CopyFileA(k_lkFilePath.c_str(), AppFile("linkkeys.bak").c_str(), FALSE);
            bd_addr_t target;
            sscanf_bd_addr(g_connectAddr, target);
            int r = gap_dedicated_bonding(target, 0);   // mitm=0 -> Just Works
            Post(L"BTstack: 发起配对 %S（删旧密钥->全新SSP）ret=%d", g_connectAddr, r);
        }
    }
    // 串行化：上一路完全就绪后，自动发起排队的下一路
    if (g_pendingCount > 0 && !AnyStreamInSetup() && !g_pairing && !g_pairForce) {
        char pend[32];
        strcpy_s(pend, sizeof(pend), g_pendingAddr[0]);
        for (int k = 1; k < g_pendingCount; ++k)
            strcpy_s(g_pendingAddr[k - 1], 32, g_pendingAddr[k]);
        g_pendingCount--;
        strcpy_s(g_connectAddr, sizeof(g_connectAddr), pend);
        g_cmdConnect.store(true);
        Post(L"BTstack: 上一路已就绪，开始连接下一路 %S", pend);
    }

    // 建流卡死看门狗：有流在建立中超过 45 秒（耳机对媒体通道不应答等），
    // 强制断开该流并自动重试，专治耳机抽风
    static int setupTicks = 0;
    if (AnyStreamInSetup() && !g_pairing) {
        if (++setupTicks >= 225) {       // 225 * 200ms = 45s
            setupTicks = 0;
            for (int i = 0; i < NUM_STREAMS; ++i) {
                if (media[i].a2dp_cid != 0 && !media[i].stream_opened) {
                    Post(L"BTstack[%d]: 建流 45 秒无进展，断开重试…", i);
                    // 先撤销底层 create_connection：超时常因为 ACL 一直没完成
                    // （对端不应答 page），不撤销的话悬空 page 会占住基带，
                    // 之后的重试连不上、扫描也扫不出任何设备
                    ClassicConnectCancel(media[i].addr);
                    a2dp_source_disconnect(media[i].a2dp_cid);
                    media[i].a2dp_cid = 0;
                    schedule_retry(media[i].addr);
                }
            }
        }
    } else {
        setupTicks = 0;
    }

    if (g_cmdConnect.exchange(false)) {
        if (g_connectAddr[0]) {
            // 断开冷却：断开后 1 秒内的连接请求忽略（防双击/时序竞态误重连）
            DWORD dt = GetTickCount() - g_disconnectTick.load();
            if (dt < 1000) {
                Post(L"BTstack: 断开冷却中（%ums），忽略连接 %S", dt, g_connectAddr);
                g_phaseIdx.store(0);
                btstack_run_loop_set_timer(ts, 200);
                btstack_run_loop_add_timer(ts);
                return;
            }
            g_scanActive = false;
            g_passiveMode = false;
            gap_inquiry_stop();
            app_state = 1;
            g_phaseIdx.store(2);
            g_pairing = false;
            g_rejectAddr[0] = 0;            // 用户主动连接：撤销拒绝名单
            g_rejectUntil.store(0);

            // 多路：已有此地址的流 → 复用；否则找空闲流
            int i = FindStreamByAddr(g_connectAddr);
            if (i >= 0 && media[i].stream_opened) {
                // 该地址已在推流：不重复建流，仅恢复状态显示
                Post(L"BTstack[%d]: %S 已在推流，忽略重复连接", i, g_connectAddr);
                g_phaseIdx.store(3);
            } else {
            if (i < 0) i = FindFreeStream();
            if (i < 0) {
                Post(L"BTstack: 所有推流通道已占用（上限 %d 路，断开一些再连）", NUM_STREAMS);
            } else if (AnyStreamInSetup() && FindStreamByAddr(g_connectAddr) != i) {
                // 串行化：有流正在建立中，后续路排队等待（避免 btstack SEP 发现并发断言）
                if (g_pendingCount < 8) {
                    strcpy_s(g_pendingAddr[g_pendingCount], 32, g_connectAddr);
                    g_pendingCount++;
                    Post(L"BTstack: 有流正在建立中，%S 已排队，就绪后自动连接", g_connectAddr);
                } else {
                    Post(L"BTstack: 排队已满，%S 的连接请求被忽略", g_connectAddr);
                }
            } else {
                // 若该流还有残留信令连接，先清掉再建，避免 establish 返回 cid=0
                if (media[i].a2dp_cid != 0 && !media[i].stream_opened) {
                    a2dp_source_disconnect(media[i].a2dp_cid);
                    media[i].a2dp_cid = 0;
                }
                // 懒创建该路的 stream endpoint（第 0 路在 app_init 创建，其余首次连接时创建）
                bool epOk = true;
                if (i > 0 && !local_stream_endpoint[i]) {
                    local_stream_endpoint[i] = a2dp_source_create_stream_endpoint(AVDTP_AUDIO, AVDTP_CODEC_SBC,
                            media_sbc_codec_capabilities, sizeof(media_sbc_codec_capabilities),
                            media_sbc_codec_configuration, sizeof(media_sbc_codec_configuration));
                    if (!local_stream_endpoint[i]) {
                        Post(L"BTstack: 创建推流端点失败");
                        epOk = false;
                    } else {
                        avdtp_set_preferred_sampling_frequency(local_stream_endpoint[i], PREFERRED_SAMPLING_RATE);
                        media[i].local_seid = avdtp_local_seid(local_stream_endpoint[i]);
                        avdtp_source_register_delay_reporting_category(media[i].local_seid);
                        Post(L"BTstack: 已创建推流端点[%d] seid=%u", i, media[i].local_seid);
                    }
                }
                if (epOk) {
                    strcpy_s(media[i].addr, sizeof(media[i].addr), g_connectAddr);
                    Post(L"BTstack[%d]: 开始连接 %S", i, g_connectAddr);
                    bd_addr_t target;
                    sscanf_bd_addr(g_connectAddr, target);
                    uint8_t st = a2dp_source_establish_stream(target, &media[i].a2dp_cid);
                    Post(L"BTstack[%d]: establish_stream status=0x%02x cid=0x%04x", i, st, media[i].a2dp_cid);
                }
            }
            }
        }
    }

    btstack_run_loop_set_timer(ts, 200);
    btstack_run_loop_add_timer(ts);
}

// ------------------------------------------------------------ app 初始化

static int app_init(void)
{
    hci_set_master_slave_policy(0);
    // 显式启用 SSP + NoInputNoOutput（Just Works），避免回退到传统 PIN 配对
    gap_ssp_set_enable(1);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    gap_ssp_set_authentication_requirement(SSP_IO_AUTHREQ_MITM_PROTECTION_NOT_REQUIRED_GENERAL_BONDING);
    gap_ssp_set_auto_accept(1);
    // 默认 LEVEL_2 要求认证密钥，Just Works 只能提供 level 1 → 建流 0x66。降到 LEVEL_1。
    gap_set_security_level(LEVEL_1);
    hci_set_inquiry_mode(INQUIRY_MODE_RSSI_AND_EIR);

    l2cap_init();
    hci_set_link_key_db(&lk_db);

    a2dp_source_init();
    a2dp_source_register_packet_handler(&a2dp_packet_handler);

    // 双路：只预注册第一个 stream endpoint；第二个在真正连接第二副耳机时
    // 懒创建（个别耳机固件对多 SEP 的 DISCOVER 响应处理有问题，预注册两个
    // 会导致其媒体通道无响应）
    memset(&media[0], 0, sizeof(sending_ctx_t));
    media[0].negRate = PREFERRED_SAMPLING_RATE;
    local_stream_endpoint[0] = a2dp_source_create_stream_endpoint(AVDTP_AUDIO, AVDTP_CODEC_SBC,
            media_sbc_codec_capabilities, sizeof(media_sbc_codec_capabilities),
            media_sbc_codec_configuration, sizeof(media_sbc_codec_configuration));
    if (!local_stream_endpoint[0]) return 1;
    avdtp_set_preferred_sampling_frequency(local_stream_endpoint[0], PREFERRED_SAMPLING_RATE);
    media[0].local_seid = avdtp_local_seid(local_stream_endpoint[0]);
    avdtp_source_register_delay_reporting_category(media[0].local_seid);

    sdp_init();
    memset(sdp_a2dp_source_service_buffer, 0, sizeof(sdp_a2dp_source_service_buffer));
    a2dp_source_create_sdp_record(sdp_a2dp_source_service_buffer, sdp_create_service_record_handle(),
                                  AVDTP_SOURCE_FEATURE_MASK_PLAYER, NULL, NULL);
    sdp_register_service(sdp_a2dp_source_service_buffer);

    // AVRCP：音量同步 + 播放状态（耳机端常依赖它才出声）
    avrcp_target_init();
    avrcp_target_register_packet_handler(&avrcp_target_packet_handler);
    avrcp_controller_init();
    avrcp_controller_register_packet_handler(&avrcp_controller_packet_handler);
    avrcp_register_packet_handler(&avrcp_packet_handler);
    memset(sdp_avrcp_target_service_buffer, 0, sizeof(sdp_avrcp_target_service_buffer));
    avrcp_target_create_sdp_record(sdp_avrcp_target_service_buffer, sdp_create_service_record_handle(),
                                   AVRCP_FEATURE_MASK_CATEGORY_PLAYER_OR_RECORDER, NULL, NULL);
    sdp_register_service(sdp_avrcp_target_service_buffer);
    memset(sdp_avrcp_controller_service_buffer, 0, sizeof(sdp_avrcp_controller_service_buffer));
    avrcp_controller_create_sdp_record(sdp_avrcp_controller_service_buffer, sdp_create_service_record_handle(),
                                       AVRCP_FEATURE_MASK_CATEGORY_MONITOR_OR_AMPLIFIER, NULL, NULL);
    sdp_register_service(sdp_avrcp_controller_service_buffer);

    memset(device_id_sdp_service_buffer, 0, sizeof(device_id_sdp_service_buffer));
    device_id_create_sdp_record(device_id_sdp_service_buffer, sdp_create_service_record_handle(),
                                DEVICE_ID_VENDOR_ID_SOURCE_BLUETOOTH,
                                BLUETOOTH_COMPANY_ID_BLUEKITCHEN_GMBH, 1, 1);
    sdp_register_service(device_id_sdp_service_buffer);

    gap_set_local_name("DualBlt A2DP 00:00:00:00:00:00");
    gap_discoverable_control(1);
    gap_set_class_of_device(0x200408);

    hci_event_callback_registration.callback = &hci_packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);
    return 0;
}

// ------------------------------------------------------------ 对外接口

void BtSvcScanStart()
{
    if (!g_thread.joinable()) return;
    g_cmdScan.store(true);
}

void BtSvcScanStop()
{
    if (!g_thread.joinable()) return;
    g_scanActive = false;
    g_phaseIdx.store(AnyStreamStreaming() ? 3 : 0);
}

void BtSvcConnect(const char* addr)
{
    if (!g_thread.joinable() || !addr) return;
    strcpy_s(g_connectAddr, sizeof(g_connectAddr), addr);
    g_cmdConnect.store(true);
}

void BtSvcDisconnect()
{
    if (!g_thread.joinable()) return;
    g_phaseIdx.store(0);   // 立即反馈：UI 马上退出"推流中"动画，不等线程 tick（线程内会再清一次状态）
    g_disconnectTick.store(GetTickCount());
    g_cmdDisconnect.store(true);
}

void BtSvcPair(const char* addr)
{
    if (!g_thread.joinable() || !addr) return;
    strcpy_s(g_connectAddr, sizeof(g_connectAddr), addr);
    g_cmdPair.store(true);
}

bool BtSvcIsPaired(const char* addr)
{
    if (!addr || !addr[0]) return false;
    FILE* f = nullptr;
    if (fopen_s(&f, k_lkFilePath.c_str(), "r") != 0 || !f) return false;
    char line[256];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        char a[32] = "";
        if (sscanf_s(line, "%31s", a, (unsigned)sizeof(a)) == 1
            && _stricmp(a, addr) == 0) { found = true; break; }
    }
    fclose(f);
    return found;
}

void BtSvcForgetDevice(const char* addr)
{
    if (!addr || !addr[0]) return;
    // 从 linkkeys.txt 删除该地址
    FILE* f = nullptr;
    if (fopen_s(&f, k_lkFilePath.c_str(), "r") == 0 && f) {
        std::vector<std::string> keep;
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char a[32] = "";
            if (sscanf_s(line, "%31s", a, (unsigned)sizeof(a)) == 1
                && _stricmp(a, addr) == 0) continue;   // 跳过要删的
            keep.push_back(line);
        }
        fclose(f);
        if (fopen_s(&f, k_lkFilePath.c_str(), "w") == 0 && f) {
            for (auto& l : keep) fputs(l.c_str(), f);
            fclose(f);
        }
    }
    // 内存里的也删
    g_lkMap.erase(addr);
    // 清掉自动重连目标，否则会继续无密钥重连并干扰扫描
    if (g_connectAddr[0] && _stricmp(g_connectAddr, addr) == 0) {
        g_connectAddr[0] = 0;
        g_phaseIdx.store(AnyStreamStreaming() ? 3 : 0);
    }
    Post(L"BTstack: 已忘记设备 %S（下次连接将重新配对）", addr);
}

void BtSvcSetTestTone(bool on)
{
    g_testTone.store(on);
    Post(L"BTstack: 测试音 = %s", on ? L"开" : L"关");
}

void BtSvcSetGain(float gain)
{
    if (gain < 0) gain = 0;
    if (gain > 2) gain = 2;
    float old = g_gain.exchange(gain);
    // 轮询同步会高频调用，值未变化时打日志会刷屏
    if (old != gain)
        Post(L"BTstack: 推流音量 = %d%%", (int)(gain * 100 + 0.5f));
}

void BtSvcSetUsbDevice(unsigned vid, unsigned pid)
{
    g_usbVid = vid;
    g_usbPid = pid;
    Post(L"BTstack: 适配器 = VID_%04X PID_%04X", vid, pid);
}

void BtSvcGetUsbDevice(unsigned& vid, unsigned& pid)
{
    vid = g_usbVid;
    pid = g_usbPid;
}

float BtSvcGetGain()
{
    return g_gain.load();
}

const char* BtSvcCurrentAddr()
{
    for (int i = 0; i < NUM_STREAMS; ++i)
        if (media[i].stream_opened || media[i].streaming || media[i].a2dp_cid)
            return media[i].addr;
    return g_connectAddr[0] ? g_connectAddr : "";
}

bool BtSvcStart(const char* addr, BtSvcStatusFn cb, BtSvcDeviceFn devFn, void* user)
{
    if (g_thread.joinable()) return false;
    g_cb = cb;
    g_devFn = devFn;
    g_cbUser = user;
    g_exitReq.store(false);
    if (addr) strcpy_s(g_addr, sizeof(g_addr), addr);
    else g_addr[0] = 0;
    g_ring.assign((size_t)RING_FRAMES * NUM_CHANNELS, 0);   // 初始化共享环形缓冲（双路共享）
    g_wpos.store(0);
    for (int i = 0; i < NUM_STREAMS; ++i) { ResetStream(i); }
    g_thread = std::thread([]{ BtThread(nullptr); });
    return true;
}

void BtSvcStop()
{
    if (!g_thread.joinable()) return;
    g_exitReq.store(true);
    if (g_runLoopUp.load())
        btstack_run_loop_trigger_exit();   // 仅当 run loop 存活时才触发
    // 限时等待：线程若卡在 USB 层，超时分离，避免 UI 冻结
    HANDLE h = g_thread.native_handle();
    if (WaitForSingleObject(h, 3000) == WAIT_OBJECT_0) {
        g_thread.join();
    } else {
        g_thread.detach();             // 放任后台自行退出
    }
    g_thread = {};
    g_running.store(false);
    g_phaseIdx.store(0);
}

bool BtSvcRunning() { return g_running.load(); }
bool BtSvcScanActive() { return g_scanActive; }
int  BtSvcPhase() { return g_phaseIdx.load(); }
bool BtSvcThreadActive() { return g_thread.joinable(); }
bool BtSvcTestTone() { return g_testTone.load(); }

void BtSvcFeedAudio(const short* pcm, int frames, int feedRate)
{
    if (!g_thread.joinable()) return;
    if (g_ring.empty()) return;          // 未初始化保护（防越界崩溃）
    g_feedRate.store(feedRate);

    uint64_t w = g_wpos.load();
    for (int i = 0; i < frames; ++i) {
        uint64_t slot = (w + i) % RING_FRAMES;
        g_ring[(size_t)slot * NUM_CHANNELS + 0] = pcm[2 * i + 0];
        g_ring[(size_t)slot * NUM_CHANNELS + 1] = pcm[2 * i + 1];
    }
    g_wpos.store(w + frames);
}

// ------------------------------------------------------------ 线程主体

static DWORD WINAPI BtThread(LPVOID)
{
    while (!g_exitReq.load()) {
        LkLoad();                    // 每次栈重建都重新加载持久化密钥
        btstack_memory_init();
        btstack_run_loop_init(btstack_run_loop_windows_get_instance());
        // btsnoop 抓包：完整记录 HCI 命令/事件/ACL，便于离线分析 L2CAP 交互
        if (hci_dump_windows_fs_open("g:\\multi_blt\\dualblt.snoop", HCI_DUMP_BTSNOOP) != 0)
            Post(L"BTstack: btsnoop 打开失败（不影响功能）");
        hci_dump_init(hci_dump_windows_fs_get_instance());

        Post(L"BTstack: 打开 USB 传输…");
        hci_init(hci_transport_usb_instance(), NULL);
        hci_transport_usb_add_device((uint16_t)g_usbVid, (uint16_t)g_usbPid);

        Post(L"BTstack: 初始化协议栈…");
        if (app_init()) {
            Post(L"BTstack: 初始化失败");
            return 1;
        }
        // app_init() 内部会把 hci_dump 覆盖成 stdout 实例，这里必须重新指回文件实例，
        // 否则 snoop 文件只有 0 字节（之前抓包一直为空就是这个原因）
        hci_dump_init(hci_dump_windows_fs_get_instance());
        Post(L"BTstack: 协议栈就绪，上电…");

        // 轮询 GUI 指令（每 200ms）
        btstack_run_loop_set_timer_handler(&g_pollTimer, poll_timer_handler);
        btstack_run_loop_set_timer(&g_pollTimer, 200);
        btstack_run_loop_add_timer(&g_pollTimer);

        hci_power_control(HCI_POWER_ON);
        g_runLoopUp.store(true);
        btstack_run_loop_execute();          // 阻塞直到 trigger_exit
        g_runLoopUp.store(false);

        // 只有栈真正起来过才做关断（hci_close 在卡死的传输上会永久阻塞）
        if (g_btUp) {
            hci_power_control(HCI_POWER_OFF);
            hci_close();
        }
        g_btUp = false;

        // 释放 run loop，否则下一轮 btstack_run_loop_init 会触发断言
        btstack_run_loop_deinit();

        if (g_restartReq.exchange(false) && !g_exitReq.load()) {
            Post(L"BTstack: 栈已重启，重新连接…");
            Sleep(500);
            continue;                        // 全新状态重试
        }
        break;
    }
    Post(L"BTstack: 已停止");
    return 0;
}
