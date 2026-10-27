// BTstack A2DP Source 服务实现：线程化 + 真实音频输入 + 双路推流
// 双路：同一个 dongle 同时向两副耳机推流（同一份音频，各自独立编码/发送）
#include "btsvc.h"

#include <Windows.h>
#include <stdint.h>
#include <stdio.h>
#include <share.h>
#include <string.h>
#include <math.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#pragma comment(lib, "winmm.lib")   // timeBeginPeriod：1ms 定时器精度
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
static volatile bool          g_stopCleaned = false;   // 优雅停止已清理
static volatile int           g_connBackoff = 0;       // 0x08 后退避计数（200ms 单位）
static int                    g_setupKillStreak = 0;   // 连续建流无进展被掐次数（2 次 → 被动等待+提示）
static int                    g_0x69Streak = 0;        // 连续 0x69（对端无应答）次数（2 次 → 被动等待+提示）
static volatile DWORD         g_serviceStartTick = 0;  // 服务启动时刻（防撞车被动窗口）
static volatile bool          g_passiveAfterDrop = false;   // 断开后进入被动：不再主动寻呼，等耳机回连（入站自动恢复）
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
static char                   g_discAddr[32] = "";              // 按地址断开的目标
static std::atomic<bool>      g_cmdDisconnectAddr{ false };     // 双路：只断该地址
static char                   g_hpVolAddr[32] = "";             // 耳机音量设置目标
static int                    g_hpVolVal = 0;                   // 音量值（0..127）
static std::atomic<bool>      g_cmdHpVolume{ false };           // 设置耳机音量命令
static std::atomic<bool>      g_cmdJumpLive{ false };           // 跳实时（清待发缓存）
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
static volatile int           g_bpCap = 40;           // SBC 编码码率上限（用户可选；双路建议低码率）
static int                    g_bootFails = 0;        // 连续启动失败计数（3 次提示拔插）
static std::atomic<bool>      g_adapterDead{ false }; // 控制器挂死（UI 显示用）
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

// 常驻日志句柄：此前每行日志都 open/append/close，BTstack 音频线程被磁盘 I/O
// 周期性卡住（杀软扫描下一次几十毫秒）→ 推流一卡一卡。改为进程内只打开一次，
// 之后每行写入 + fflush（崩溃前最后一行也能落盘），开销降两个数量级
static std::mutex g_logMtx;
static FILE*      g_logFile = nullptr;                  // 日志线程私有句柄
static std::vector<std::wstring> g_logQ;               // 待写盘队列
static std::atomic<bool> g_logRun{ true };

// 专职日志落盘线程：音频/UI 线程只入队（短临界区），磁盘 IO 全在这里
static void LogDrainMain()
{
    while (g_logRun.load()) {
        Sleep(200);
        std::vector<std::wstring> batch;
        {
            std::lock_guard<std::mutex> lk(g_logMtx);
            batch.swap(g_logQ);
        }
        if (batch.empty()) continue;
        if (!g_logFile) {
            // 显式共享读：否则外部（PowerShell/type）读不了正被写的日志
            g_logFile = _wfsopen(AppFileW(L"btsvc.log").c_str(), L"a, ccs=UTF-8", _SH_DENYNO);
        }
        if (g_logFile) {
            for (auto& l : batch) fwprintf(g_logFile, L"%s", l.c_str());
            fflush(g_logFile);
        }
    }
}
static std::once_flag g_logOnce;
static void LogStart() { std::call_once(g_logOnce, []{ std::thread(LogDrainMain).detach(); }); }

static void Post(const wchar_t* fmt, ...)
{
    LogStart();   // 首次调用时拉起落盘线程
    wchar_t buf[256];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, 256, _TRUNCATE, fmt, ap);
    va_end(ap);

    // 异步落盘：磁盘 IO 不在音频/UI 线程上发生（fwprintf+fflush 在音频线程
    // 直接执行会造成编码停拍 = 周期性小卡）。入队后由专职日志线程写盘。
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t line[300];
        swprintf_s(line, L"[%02d:%02d:%02d] %s\n", st.wHour, st.wMinute, st.wSecond, buf);
        std::lock_guard<std::mutex> lk(g_logMtx);
        g_logQ.emplace_back(line);
        if (g_logQ.size() > 2000) g_logQ.erase(g_logQ.begin());   // 防泄漏兜底
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
    hci_con_handle_t acl_handle;         // 本路的 ACL 连接句柄
    uint32_t setupLastActiveMs;          // 建流最近一次收到对端活动的时刻（自适应看门狗）
    double   phase;                  // 本路的消费位置（源帧单位）
    int      negRate;                // 本路协商采样率
    uint32_t pktCount;
    uint32_t byteCount;
    uint32_t dropCount;              // 发送未完成（缓冲满等）次数，保留数据待重试
    volatile uint8_t testTone;       // 本路测试音开关（按设备隔离）
    volatile int hpVolume;           // 本路耳机音量（AVRCP 上报，0..127，-1 未知）
    volatile int hpSetPending;       // 在途音量设置笔数（应答按序回来，中间的丢弃）
    volatile DWORD hpSetLastMs;      // 最近一笔设置下发时刻（通知回声窗判定）
    volatile int avrcpDelayTicks;    // 推流启动后延迟连接 AVRCP 的倒计时（200ms 单位）
    volatile int volProbeTicks;      // 连上 AVRCP 后等待真实音量上报的探测窗（200ms 单位）
    uint32_t openedMs;               // 媒体流打开时刻（打开后迟迟无 START → 看门狗接管）
    uint8_t  aclDropPending;         // 信令断开后待拆 ACL（先 A2DP 后 ACL 的顺序拆除）
} sending_ctx_t;

static sending_ctx_t            media[NUM_STREAMS];

// 每设备记忆音量（定义见"耳机音量"段；此处前置供早期事件处理使用）
static void VolRemember(const char* addr, int vol);
static int  VolRemembered(const char* addr);
static btstack_sbc_encoder_bluedroid_t sbc_encoder_state[NUM_STREAMS];
static const btstack_sbc_encoder_t * sbc_encoder_instance[NUM_STREAMS];
static btstack_packet_callback_registration_t hci_event_callback_registration;
static avdtp_stream_endpoint_t * local_stream_endpoint[NUM_STREAMS];
// 双路：连接句柄按流（地址）记录，避免修 A 时误断 B 的 ACL

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
    media[i].hpVolume = -1;      // memset 会清成 0（会被误显示为 0%），未知应为 -1
    media[i].negRate = PREFERRED_SAMPLING_RATE;
    // memset 会把 acl_handle 清成 0，而 0 不是 INVALID（0xFFFF）——
    // 残留的 0 会让看门狗误以为 ACL 已建立，在寻呼阶段误杀建流
    media[i].acl_handle = HCI_CON_HANDLE_INVALID;
}
static bool AnyStreamActive()
{
    for (int i = 0; i < NUM_STREAMS; ++i)
        if (media[i].a2dp_cid != 0 || media[i].stream_opened) return true;
    return false;
}
// 按地址取/清 ACL 句柄（双路安全：修 A 不会误断 B）
static hci_con_handle_t HandleForAddr(const char* addr)
{
    if (!addr || !addr[0]) return HCI_CON_HANDLE_INVALID;
    int i = FindStreamByAddr(addr);
    if (i < 0) return HCI_CON_HANDLE_INVALID;
    return media[i].acl_handle;
}
static void ClearHandleForAddr(const char* addr)
{
    int i = FindStreamByAddr(addr);
    if (i >= 0) media[i].acl_handle = HCI_CON_HANDLE_INVALID;
}
// 是否有流还在建立中（信令已建但媒体流未开）—— btstack 的 SEP 发现一次只能跑一个，
// 两副耳机必须串行建立：第一路完全就绪（进入推流）后才发起第二路。
// "已打开但迟迟无 START"也算建立中（看门狗要管它，且此时不应串行建第二路）
static bool AnyStreamInSetup()
{
    for (int i = 0; i < NUM_STREAMS; ++i)
        if (media[i].a2dp_cid != 0 && (!media[i].stream_opened || !media[i].streaming))
            return true;
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
        // 尊重耳机自身音量（不强制 100%）。订阅音量变化通知：
        // 注册后耳机先发当前值（interim），之后每次用户调节实时上报
        avrcp_controller_enable_notification(acid, AVRCP_NOTIFICATION_EVENT_VOLUME_CHANGED);
        media[i].volProbeTicks = 8;   // 1.6s 探测窗：等耳机 interim 上报真实音量
        Post(L"AVRCP: 控制通道已建立 cid=0x%04x，等待耳机上报音量…", acid);
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
        // 未知子事件只记一次（不刷屏）。已知会到这里但无需处理的：
        //   0x10 SET_ABSOLUTE_VOLUME_RESPONSE / 0x11 NOTIFICATION_STATE
        //   —— controller 命名空间的事件串到 target 处理器（btstack 双角色广播）
        {
            static uint8_t once[0x40] = { 0 };
            if (packet[2] < 0x40 && !once[packet[2]]) {
                once[packet[2]] = 1;
                Post(L"AVRCP[target]: 未处理子事件 0x%02x（仅记录一次）", packet[2]);
            }
        }
        break;
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
        // 必须应答到"发问的那路"的 cid 上——双路时用错通道应答，
        // 发问的耳机收不到 → 按"暂停"自我静音 → 两副耳机交替静音。
        // 事件布局：[0]=meta [1]=len [2]=subevent [3..4]=avrcp_cid(小端)
        uint16_t q = little_endian_read_16(packet, 3);
        avrcp_target_play_status(q, play_info.song_length_ms,
                                 play_info.song_position_ms, play_info.status);
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
    case AVRCP_SUBEVENT_NOTIFICATION_VOLUME_CHANGED: {
        uint16_t acid = avrcp_subevent_notification_volume_changed_get_avrcp_cid(packet);
        int vol = avrcp_subevent_notification_volume_changed_get_absolute_volume(packet);
        int i = -1;
        for (int k = 0; k < NUM_STREAMS; ++k)
            if (media[k].avrcp_cid == acid) { i = k; break; }
        if (i >= 0) {
            // 回声过滤：耳机对我们设置的应答/通知可能滞后到达（事件通道异步），
            // 旧值通知会在在途清零后落进窗口把缓存拽回（40→35→40 抖动）。
            // 在途>0 或设置后 600ms 内的通知一律视为回声丢弃；真正的耳机
            // 本地按键调节发生在这两种窗口之外，正常实时同步。
            if (media[i].hpSetPending > 0 || GetTickCount() - media[i].hpSetLastMs < 600)
                break;
            // 假零过滤：有些耳机（AirPods）在第一次 SetAbsoluteVolume 之前，
            // 绝对音量未"实体化"，interim 上报 0 并非真实值——采信它会把
            // 显示钉在 0，用户一滚轮还会把耳机真写到 1%。保持"未知"。
            if (vol == 0 && media[i].hpSetLastMs == 0)
                break;
            media[i].hpVolume = vol;
            VolRemember(media[i].addr, vol);   // 记住真实值，下次假零时可落位
            Post(L"AVRCP: 耳机音量 %d%%", vol * 100 / 127);
        }
        break;
    }
    case AVRCP_SUBEVENT_SET_ABSOLUTE_VOLUME_RESPONSE: {
        // 设置命令的应答：耳机实际应用的音量。多笔在途时按序回来——
        // 中间应答（旧值）直接丢弃，只在最后一笔回来时写缓存（权威校正）
        uint16_t acid = avrcp_subevent_set_absolute_volume_response_get_avrcp_cid(packet);
        int vol = avrcp_subevent_set_absolute_volume_response_get_absolute_volume(packet);
        int i = -1;
        for (int k = 0; k < NUM_STREAMS; ++k)
            if (media[k].avrcp_cid == acid) { i = k; break; }
        if (i >= 0) {
            if (media[i].hpSetPending > 0) --media[i].hpSetPending;
            if (media[i].hpSetPending == 0 && media[i].hpVolume != vol) {
                media[i].hpVolume = vol;
                VolRemember(media[i].addr, vol);
                Post(L"AVRCP: 耳机确认音量 %d%%（校正）", vol * 100 / 127);
            }
        }
        break;
    }
    case AVRCP_SUBEVENT_NOTIFICATION_STATE:
        // 0x11：通知注册回执（enable_notification 的应答），无需处理
        break;
    default:
        // 未知子事件只记一次。已知会到这里但无需处理的：
        //   0x15 PLAY_STATUS（我们不查询播放状态）、0x16/0x17 OPERATION_START/COMPLETE
        //   （耳机透传按键事件，我们不转发系统）、0x20-0x23 NOW_PLAYING 信息
        //   0x1e EVENT_BATT/等等通知 —— 均与我们"纯推流"的场景无关
        {
            static uint8_t once[0x40] = { 0 };
            if (packet[2] < 0x40 && !once[packet[2]]) {
                once[packet[2]] = 1;
                Post(L"AVRCP[ctrl]: 未处理子事件 0x%02x（仅记录一次）", packet[2]);
            }
        }
        break;
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
    int hi = rate * 7 / 20;   // ~350ms（双路射频忙时的瞬时停滞不触发跳变，
                              //   跳变 = 相位瞬移 = 可闻咔哒；延迟上限随之增大）
    int lo = rate / 50;       // ~20ms
    if (RingAvailableIdx(i) > hi) {
        media[i].phase = (double)(g_wpos.load() - (uint64_t)lo);
    }

    if (media[i].testTone) {
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
    // 发送间隔诊断：>150ms 的间隔 = 一次可闻卡顿风险。
    // 用于定位卡顿落在哪个阶段（另一路连接时：寻呼/SDP/信令/AVRCP）
    {
        static DWORD lastSendMs[NUM_STREAMS] = { 0, 0 };
        DWORD now = GetTickCount();
        if (lastSendMs[i] != 0 && now - lastSendMs[i] > 150) {
            Post(L"BTstack[%d]: 发送间隔 %ums（卡顿风险，%S）",
                 i, now - lastSendMs[i],
                 AnyStreamInSetup() ? "另一路连接中" : "稳态");
        }
        lastSendMs[i] = now;
    }
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
    // 每 40 秒一条累计字节数（音频线程上少打日志：每条都是文件 I/O）
    if ((media[i].pktCount % 2000) == 1)
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

    // 每 30 秒记录一次（诊断；lastLog 按路独立——共享会让两路日志互相顶名）。
    // 之前 5 秒一条：音频线程上每条都是文件 I/O，是推流周期性卡顿的来源之一
    static uint32_t lastLog[NUM_STREAMS] = { 0, 0 };
    if (now - lastLog[i] >= 30000) {
        lastLog[i] = now;
        static uint32_t lastPkt[NUM_STREAMS] = { 0, 0 };
        uint32_t dpkt = media[i].pktCount - lastPkt[i];
        lastPkt[i] = media[i].pktCount;
        int rate = g_feedRate.load();
        SYSTEMTIME st; GetLocalTime(&st);
        Post(L"[%02d:%02d:%02d][%d] 水位 %d ms | 包速率 %u/s",
             st.wHour, st.wMinute, st.wSecond, i,
             (int)((int64_t)RingAvailableIdx(i) * 1000 / (rate > 0 ? rate : 48000)),
             dpkt / 30);
    }

    if (media[i].sbc_ready_to_send) {
        // 有保留的帧等待发送（上次因缓冲满未完成）：每 10ms 重新请求一次 CAN_SEND
        a2dp_source_stream_endpoint_request_can_send_now(media[i].a2dp_cid, media[i].local_seid);
        return;
    }
    // 关键修复：不要发“编码后的静音”。流刚建立就持续发全零 SBC 帧，会让部分音箱
    // （如 K3）判定无信号而静音输出，之后即使真实音频到来也保持静音。
    // 没有测试音且 ring 里没有数据时保持静默不发，等真实音频到达再开始推流。
    if (!media[i].testTone && RingAvailableIdx(i) <= 0) {
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
// 拆链时间戳：断开后 10 秒内关闭 page scan（拒收回连，见 page scan 协调）；
// 手动重连不设冷却（对撞已由 page scan 根治）
static char                   g_tearDownAddr[32] = "";
static uint32_t               g_tearDownMs = 0;
// 最近一次连接请求（点击/自动重连）：UI 在此期间显示"连接中"动画，
// 直到该地址开始推流（或 15 秒超时）
static char                   g_connectReqAddr[32] = "";
static volatile DWORD         g_connectReqTick = 0;
static void MarkConnectReq(const char* addr)
{
    if (!addr || !addr[0]) return;
    strcpy_s(g_connectReqAddr, sizeof(g_connectReqAddr), addr);
    g_connectReqTick = GetTickCount();
}

// 失败升级：切换到全新配对策略，断开残留链路，引擎会在下个周期自动执行
static void schedule_retry(const char* addr)
{
    // 非当前目标的旧尝试：只清理，不再重试（否则两个重试源互相打架，
    // 建立的流被看门狗互相掐死）
    if (addr && addr[0] && g_connectAddr[0] && _stricmp(addr, g_connectAddr) != 0) {
        Post(L"BTstack: %S 的旧尝试放弃（当前目标 %S）", addr, g_connectAddr);
        int i = FindStreamByAddr(addr);
        if (i >= 0) {
            media[i].a2dp_cid = 0;
            media[i].addr[0] = 0;
        }
        return;
    }
    g_strategy = 1;
    if (addr && addr[0]) {
        hci_con_handle_t h = HandleForAddr(addr);
        if (h != HCI_CON_HANDLE_INVALID) {
            gap_disconnect(h);
            ClearHandleForAddr(addr);
        }
    }
    Post(L"BTstack: 连接失败，已切换策略：全新配对（自动）");
}

static void JumpAllToLive()
{
    int64_t live = (int64_t)g_wpos.load();
    uint32_t now = btstack_run_loop_get_time_ms();
    for (int i = 0; i < NUM_STREAMS; ++i) {
        if (!media[i].streaming) continue;
        media[i].phase = (double)live;
        media[i].samples_ready = 0;
        media[i].time_audio_data_sent = now;      // 重置计时基准（关键）
        media[i].acc_num_missed_samples = 0;
        media[i].sbc_storage_count = 0;
        media[i].sbc_ready_to_send = 0;
    }
}

static void start_streaming(int i)
{
    media[i].max_media_payload_size = btstack_min(a2dp_max_media_payload_size(media[i].a2dp_cid, media[i].local_seid),
                                               SBC_STORAGE_SIZE);
    media[i].sbc_storage_count = 0;
    media[i].sbc_ready_to_send = 0;
    media[i].streaming = 1;
    JumpAllToLive();          // 集合+1：新流和被连接过程卡住的兄弟流都从实时开始
    g_setupKillStreak = 0;   // 流成功打开 = 无进展计数清零
    // A2DP 协商完成后再连 AVRCP（很多音箱不主动建）。再延迟 3 秒：
    // 新流刚起时另一路正在推流，AVRCP 的 SDP 查询+连接是突发大户，
    // 挪出最敏感的窗口，让先来的流先把缓冲垫起来
    media[i].avrcpDelayTicks = 15;   // 15 * 200ms = 3s（poll 里递减，到 0 再连）
    btstack_run_loop_remove_timer(&media[i].audio_timer);
    btstack_run_loop_set_timer_handler(&media[i].audio_timer, audio_timeout);
    btstack_run_loop_set_timer_context(&media[i].audio_timer, (void*)(intptr_t)i);
    // 相位错开：双路同时推流时，第二路偏移半个发送周期，
    // 避免两路同一毫秒一起灌包把 dongle 缓冲打爆（偶发单边断音）
    uint32_t first = AUDIO_TIMEOUT_MS;
    if (i != 0 && AnyStreamStreaming()) first = AUDIO_TIMEOUT_MS / 2;
    btstack_run_loop_set_timer(&media[i].audio_timer, first);
    btstack_run_loop_add_timer(&media[i].audio_timer);
}

// ------------------------------------------------------------ 事件

static void ClassicConnectCancel(const char* addr);   // 定义在命令轮询区（hci_packet_handler 提前用到）

static void a2dp_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_A2DP_META) return;

    uint8_t status;
    uint16_t cid;

    // 自适应看门狗：建立中的流收到任何 A2DP/AVDTP 事件 = 对端有响应，
    // 重置无进展倒计时（协商慢的耳机不再被误杀，真挂死才超时）
    {
        int ti = FindStreamByCid(channel);
        if (ti >= 0 && !media[ti].stream_opened)
            media[ti].setupLastActiveMs = btstack_run_loop_get_time_ms();
    }

    switch (hci_event_a2dp_meta_get_subevent_code(packet)) {

    case A2DP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED:
        status = a2dp_subevent_signaling_connection_established_get_status(packet);
        cid    = a2dp_subevent_signaling_connection_established_get_a2dp_cid(packet);
        if (status != ERROR_CODE_SUCCESS) {
            Post(L"BTstack: A2DP 信令连接失败 status=0x%02x", status);
            // 0x0B = ACL 链路已存在：dongle 重启后残留了旧链路，先断开再重连
            if (status == ERROR_CODE_ACL_CONNECTION_ALREADY_EXISTS) {
                bd_addr_t ea;
                a2dp_subevent_signaling_connection_established_get_bd_addr(packet, ea);
                const char* es = bd_addr_to_str(ea);
                hci_con_handle_t h = HandleForAddr(es);
                if (h != HCI_CON_HANDLE_INVALID) {
                    Post(L"BTstack: 检测到 %S 残留链路，断开后重连", es);
                    gap_disconnect(h);
                    ClearHandleForAddr(es);
                }
            }
            int i = FindStreamByCid(cid);
            if (i >= 0) media[i].a2dp_cid = 0;
            if (status == 0x0b) {
                // 0x0b = 连接对撞：耳机同时也在回连我们，其入站连接"正在建立中"，
                // btstack 拒绝我方的 CreateConnection（'already in open process'）。
                // 1-2 秒后入站 ACL 建成，重试会走"复用已存在连接"的成功路径。
                // 绝不能升级重配（会删掉好密钥）。
                g_connBackoff = 10;   // 2 秒
                Post(L"BTstack: 连接对撞（对端回连中）—— 2 秒后自动重试");
                break;
            }
            if (status == 0x69) {
                // 0x69 = 对端 10 秒未应答 L2CAP 连接请求，而 ACL/认证都是好的——
                // dongle LC 层卡死签名（上一场会话拆除后控制器状态未清）。
                // 实测进程内软复位救不了（WinUSB 重开失败、程序反而瘫痪），
                // 唯一可靠恢复 = 用户重启程序；被动等待期间耳机主动回连仍能接住。
                g_0x69Streak++;
                if (g_0x69Streak >= 2) {
                    Post(L"BTstack: 连续对端无应答 —— dongle 主动链路已卡死，进入被动等待；请重启程序恢复主动连接");
                    g_passiveAfterDrop = true;
                    g_0x69Streak = 0;
                    break;
                }
                Post(L"BTstack: 对端无应答（0x69），10 秒后重试");
                g_connBackoff = 50;
                break;
            }
            if (status == 0x09) {
                // 0x09 = 对端拒绝连接（资源受限）：耳机应答了但拒绝建链——
                // 几乎总是它正被其它设备占用（手机）或左右耳互连未完成。
                // 与密钥无关，重配无意义。惯例：别连续敲打忙耳机（30s 退避）
                g_connBackoff = 150;   // 30s
                Post(L"BTstack: 耳机拒绝连接（0x09 资源受限）——被占用（手机）或左右耳互连中；30 秒后自动重试");
                break;
            }
            if (status == 0x81 || status == 0x04 || status == 0x08 || status == 0x1f
                || status == 0x6a || status == 0x02 || status == 0x16) {
                // 0x81 = SDP_QUERY_INCOMPLETE（对端忙，如 AirPods 被分时访问）
                // 0x04/0x08 = 寻呼/连接超时（耳机暂时不在可连接状态）
                // 0x1f = 本地中止（我们的看门狗掐的，QCY 首建流不应答属正常，
                //        掐死后重试一次就通——绝不能升级到重配，否则删密钥死循环）
                // 0x69 = L2CAP RTX 超时（对端 10 秒没应答连接请求——耳机固件清理中）
                // 0x6a = baseband 断开（对端挂断了链路——多见于上次会话残留未清）
                // 0x02/0x16 = 寻呼限幅主动撤销产生的伪错误（本地取消/本地终止），
                //             绝不能升级重配——那会删好密钥
                // 这些都是暂态问题，直连重试即可。引擎退避后自动重连。
                g_connBackoff = (status == 0x08) ? 60 : 50;   // 12s / 10s（TWS 清理残留需要时间）
                Post(L"BTstack: 连接失败 status=0x%02x（暂态），退避后自动重试", status);
                break;
            }
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
                g_0x69Streak = 0;   // 信令连上 = 对端无应答状态解除
                // AVRCP 连接挪到媒体流打开后（start_streaming）：信令刚建立就发
                // AVRCP 的 SDP 查询会与 A2DP 协商竞争，实测 K3 会因此挂断整条链路
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
            // 编码码率封顶（用户可选）：双路时两路合计容易逼近单 dongle 射频
            // 上限，偶发缓冲溢出 = 单边断音/交替。切低码率可留足射频余量。
            int bp = c.max_bitpool_value;
            if (bp > g_bpCap) {
                Post(L"AVDTP[%d]: bitpool %d -> %d（码率上限）", i, bp, g_bpCap);
                bp = g_bpCap;
            }
            sbc_encoder_instance[i]->configure(&sbc_encoder_state[i], SBC_MODE_STANDARD,
                                            c.block_length, c.subbands,
                                            c.allocation_method,
                                            c.sampling_frequency,
                                            bp,
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
            media[i].openedMs = btstack_run_loop_get_time_ms();
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
            g_passiveAfterDrop = false;   // 已连接，清除被动状态
            if (g_connectReqAddr[0] && _stricmp(media[i].addr, g_connectReqAddr) == 0)
                g_connectReqAddr[0] = 0;   // 已开始推流："连接中"动画使命完成
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
            // 断开后进入被动模式：不主动寻呼（耳机在盒子里寻呼只会空转），
            // 耳机回连时入站连接自动恢复推流；用户再点连接则恢复主动
            if (g_connectAddr[0] && _stricmp(media[i].addr, g_connectAddr) == 0) {
                g_passiveAfterDrop = true;
                Post(L"BTstack: 已进入被动等待（耳机回连自动恢复，不再主动寻呼）");
            }
            // 集合-1：断链控制过程挤占过射频，剩余的流重置缓冲从实时继续
            if (AnyStreamStreaming()) JumpAllToLive();
            media[i].a2dp_cid   = 0;
            media[i].stream_opened = 0;
            media[i].streaming  = 0;
            btstack_run_loop_remove_timer(&media[i].audio_timer);
            // 顺序拆除的第二步：信令已释放，现在再拆 ACL（单路断开时挂起的请求）
            if (media[i].aclDropPending) {
                media[i].aclDropPending = 0;
                if (media[i].acl_handle != HCI_CON_HANDLE_INVALID) {
                    gap_disconnect(media[i].acl_handle);
                    media[i].acl_handle = HCI_CON_HANDLE_INVALID;
                }
            }
            if (!AnyStreamStreaming()) {
                g_running.store(false);
                g_phaseIdx.store(g_scanActive ? 1 : 0);   // 全局相位随实际流收敛（扫描中除外）
            }
        }
        break;
    }

    default:
        // 未知子事件只记一次。已知会到这里但无需处理的：
        //   0x13-0x18 各编解码能力上报（SBC/AAC/ATRAC/其它——SEP 发现时的信息，
        //            btstack 内部已用它们完成协商，应用层不用再处理）
        //   0x19 延迟上报能力（我们注册了 delay reporting category，信息性）
        //   0x1a-0x1c 为 AVDTP 层事件（与 A2DP 共用 meta 命名空间的串扰）
        {
            static uint8_t once[0x40] = { 0 };
            uint8_t c = hci_event_a2dp_meta_get_subevent_code(packet);
            if (c < 0x40 && !once[c]) {
                once[c] = 1;
                Post(L"A2DP[?]: 未处理子事件 0x%02x（仅记录一次）", c);
            }
        }
        break;
        break;
    }
}

// ------------------------------------------------------------ 链路密钥库（持久化到文件）
// 配对成功一次就把密钥存盘；之后连接直接用密钥，不再配对 —— 和 Windows 一样秒连。

struct LkEntry { link_key_t key; link_key_type_t type; std::string nameUtf8; };   // 名字与密钥同存一行
static std::map<std::string, LkEntry> g_lkMap;     // key: "XX:XX:XX:XX:XX:XX"
static std::recursive_mutex g_lkLock;              // 密钥库串行化（UI 线程与栈线程并发访问）
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

// 解析密钥文件到 g_lkMap（返回条数）
static unsigned LkParseFile(const char* path)
{
    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") != 0 || !f) return 0;
    char line[256];
    unsigned cnt = 0;
    while (fgets(line, sizeof(line), f)) {
        // 手动分词（MSVC 的 sscanf_s 禁用 %n，之前用它导致永远 loaded 0）
        char* p = line;
        auto NextTok = [&p]() -> char* {
            while (*p == ' ' || *p == '\t') ++p;
            char* s = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') ++p;
            if (*p) { *p = 0; ++p; }
            return s;
        };
        char* a = NextTok();
        if (!a[0]) continue;
        unsigned k[16];
        bool ok = true;
        for (int i = 0; i < 16; ++i) {
            char* tk = NextTok();
            if (!tk[0]) { ok = false; break; }
            k[i] = strtoul(tk, nullptr, 16);
        }
        if (!ok) continue;
        char* tt = NextTok();
        if (!tt[0]) continue;
        unsigned t = strtoul(tt, nullptr, 10);
        LkEntry e;
        for (int i = 0; i < 16; ++i) e.key[i] = (uint8_t)k[i];
        e.type = (link_key_type_t)t;
        // 行尾设备名（\t 或空格后，到行尾）
        while (*p == ' ' || *p == '\t') ++p;
        for (; *p && *p != '\r' && *p != '\n'; ++p) e.nameUtf8.push_back(*p);
        g_lkMap[a] = e;
        ++cnt;
    }
    fclose(f);
    return cnt;
}


static void LkSave();

static void LkLoad()
{
    std::lock_guard<std::recursive_mutex> lk(g_lkLock);
    g_lkMap.clear();
    unsigned n = LkParseFile(k_lkFilePath.c_str());
    Post(L"BTstack: 密钥库 loaded %u link keys", n);
}

static void LkWriteFile(const char* path)
{
    // 直写（不用临时文件）。写盘全部发生在事件时刻（配对成功/忘记设备），
    // 都是几十字节的毫秒级操作且 fclose 同步落盘后才算完成——
    // 关闭程序时没有任何挂起的写，不存在"写一半被杀"。
    FILE* f = nullptr;
    if (fopen_s(&f, path, "w") != 0 || !f) return;
    for (auto& kv : g_lkMap) {
        fprintf(f, "%s", kv.first.c_str());
        for (int i = 0; i < 16; ++i) fprintf(f, " %02x", kv.second.key[i]);
        fprintf(f, " %u", (unsigned)kv.second.type);
        if (!kv.second.nameUtf8.empty())
            fprintf(f, "\t%s", kv.second.nameUtf8.c_str());
        fprintf(f, "\n");
    }
    fclose(f);
}

static void LkSave()
{
    std::lock_guard<std::recursive_mutex> lk(g_lkLock);
    LkWriteFile(k_lkFilePath.c_str());
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
    std::lock_guard<std::recursive_mutex> lk(g_lkLock);
    auto it = g_lkMap.find(LkKeyStr(addr));
    if (it == g_lkMap.end()) return 0;
    memcpy(key, it->second.key, 16);
    *type = it->second.type;
    Post(L"BTstack: 使用已存密钥 %S（免配对）", LkKeyStr(addr).c_str());
    return 1;
}
static void lk_put(bd_addr_t addr, link_key_t key, link_key_type_t type)
{
    std::lock_guard<std::recursive_mutex> lk(g_lkLock);
    // 就地更新密钥字段，保留原有名字（配对是重新协商密钥，不该清名字）
    LkEntry& e = g_lkMap[LkKeyStr(addr)];
    memcpy(e.key, key, 16);
    e.type = type;
    LkSave();
    Post(L"BTstack: 与 %S 配对成功，密钥已保存", LkKeyStr(addr).c_str());
}
static void lk_del(bd_addr_t addr) { std::lock_guard<std::recursive_mutex> lk(g_lkLock); g_lkMap.erase(LkKeyStr(addr)); }   // 只删内存：文件保留最后成功状态，配对失败 LkLoad 即恢复
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

// 启动后延迟自动重连：等耳机的入站回连先落地（避免双向寻呼对撞）
static btstack_timer_source_t g_bootAutoTimer;
static bool                   g_bootAutoPending = false;
static void boot_auto_connect(btstack_timer_source_t * ts)
{
    (void)ts;
    g_bootAutoPending = false;
    if (!g_addr[0] || !g_btUp || AnyStreamActive()) return;   // 已回连/已连接就不重复
    int k = FindFreeStream();
    if (k < 0) return;
    strcpy_s(media[k].addr, sizeof(media[k].addr), g_addr);
    bd_addr_t target;
    sscanf_bd_addr(g_addr, target);
    a2dp_source_establish_stream(target, &media[k].a2dp_cid);
    media[k].setupLastActiveMs = btstack_run_loop_get_time_ms();
}

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
        g_bootFails = 0;       // 就绪事件到达：清零连续失败计数
        g_adapterDead.store(false);   // 恢复：清除挂死标志
        bd_addr_t a;
        gap_local_bd_addr(a);
        Post(L"BTstack: 已启动 %S", bd_addr_to_str(a));

        if (g_addr[0]) {
            // 启动后延迟 5 秒再自动重连：程序重启会掉掉耳机上一次的 ACL，
            // TWS/K3 类耳机会立刻主动回连——立刻出向寻呼必然对撞（0x0b/悬空 page）。
            // 等 5 秒：耳机回连了就复用其入站链路（引擎自动接住），没回连再自己寻呼
            g_bootAutoPending = true;
            MarkConnectReq(g_addr);   // 自动重连排队期间 UI 也显示"连接中"
            btstack_run_loop_remove_timer(&g_bootAutoTimer);
            btstack_run_loop_set_timer_handler(&g_bootAutoTimer, boot_auto_connect);
            btstack_run_loop_set_timer(&g_bootAutoTimer, 5000);
            btstack_run_loop_add_timer(&g_bootAutoTimer);
            Post(L"BTstack: 5 秒后自动重连 %S（等待耳机回连让路）", g_addr);
        } else if (g_scanActive) {
            gap_inquiry_start(8);
            Post(L"BTstack: 扫描已开始");
        } else {
            Post(L"BTstack: 就绪，点「扫描」查找耳机");
        }
        return;
    }

    // 记录每个地址的 ACL 连接句柄（0x0B=链路已存在时用它断开残留链路）
    if (evt == HCI_EVENT_CONNECTION_COMPLETE) {
        bd_addr_t addr;
        hci_event_connection_complete_get_bd_addr(packet, addr);
        uint8_t st = hci_event_connection_complete_get_status(packet);
        if (st == ERROR_CODE_SUCCESS) {
            int i = FindStreamByAddr(bd_addr_to_str(addr));
            if (i >= 0) {
                media[i].acl_handle = hci_event_connection_complete_get_connection_handle(packet);
                // K3 类耳机寻呼应答可达 15 秒以上：ACL 建成才算第一个"进展"，
                // 无进展倒计时从这里重新起算（否则会被看门狗掐在建成瞬间）
                media[i].setupLastActiveMs = btstack_run_loop_get_time_ms();
                // 角色切换：让耳机当主。CSR 方案的音箱做从设备时会接受 ACL/认证
                // 却拒绝响应一切 L2CAP 请求（SDP 查询/信令建流全部石沉大海）——
                // Windows 栈秒连正是等耳机主动回连、由耳机当主驱动全部流程。
                // 我们寻呼它时 = 我们当主 = 踩中这个坑，必须切回从位。
                if (g_connectAddr[0] && _stricmp(bd_addr_to_str(addr), g_connectAddr) == 0) {
                    Post(L"HCI: 请求角色切换 —— 让耳机 %s 当主", bd_addr_to_str(addr));
                    hci_send_cmd(&hci_switch_role_command, addr, HCI_ROLE_SLAVE);
                }
            }
        } else if (st == ERROR_CODE_ACL_CONNECTION_ALREADY_EXISTS) {
            // 对端反向连上来抢先建了链路：同样是进展（复用该 ACL 继续建流），
            // 绝不能当作超时掐掉
            int i = FindStreamByAddr(bd_addr_to_str(addr));
            if (i >= 0) media[i].setupLastActiveMs = btstack_run_loop_get_time_ms();
        }
        // 诊断：ACL 建立结果（成败都打——0x04 在这里出现=寻呼阶段就没通过）
        Post(L"HCI: ACL 连接 %s status=0x%02x%s",
             bd_addr_to_str(addr), st,
             st == ERROR_CODE_SUCCESS ? L"（已建立）" : L"");
        return;
    }
    if (evt == HCI_EVENT_DISCONNECTION_COMPLETE) {
        uint16_t h = hci_event_disconnection_complete_get_connection_handle(packet);
        uint8_t  r = hci_event_disconnection_complete_get_reason(packet);
        // 诊断：断开原因码——0x13=对端挂断 0x16=本机挂断 0x08=连接超时 0x22=对端资源不足
        for (int i = 0; i < NUM_STREAMS; ++i)
            if (media[i].acl_handle == h) {
                Post(L"HCI: ACL 断开 [%d] %s 原因=0x%02x", i,
                     media[i].addr[0] ? media[i].addr : "?", r);
                media[i].acl_handle = HCI_CON_HANDLE_INVALID;
                break;
            }
        return;
    }
    if (evt == HCI_EVENT_CONNECTION_REQUEST) {
        // 诊断 + 让位：耳机主动回连（入站）
        bd_addr_t addr;
        hci_event_connection_request_get_bd_addr(packet, addr);
        const char* addrS = bd_addr_to_str(addr);
        Post(L"HCI: 收到入站连接请求 %s（class=0x%06x）", addrS,
             (unsigned)hci_event_connection_request_get_class_of_device(packet));
        // 关键：目标耳机主动回连时，撤销我方对它的出向建流/寻呼，让它的入站
        // 流程走完（回连 ACL → SDP 查询我们 → 入站 PSM25 信令 → 接受者角色协商）。
        // 双方同时发起会互相卡死 L2CAP 配置（实测 0x69 应答超时/0x1f 无进展）。
        if (g_connectAddr[0] && _stricmp(addrS, g_connectAddr) == 0) {
            Post(L"HCI: 目标 %S 主动回连 —— 撤销我方出向建流，改为被动接收", g_connectAddr);
            for (int i = 0; i < NUM_STREAMS; ++i) {
                if (media[i].a2dp_cid != 0 && !media[i].stream_opened
                    && media[i].addr[0] && _stricmp(media[i].addr, g_connectAddr) == 0) {
                    ClassicConnectCancel(media[i].addr);
                    a2dp_source_disconnect(media[i].a2dp_cid);
                    media[i].a2dp_cid = 0;
                    // addr 保留：入站信令建立成功后按地址复用该流
                }
            }
            g_connBackoff = 60;   // 12 秒内引擎不再主动寻呼，让入站建流走完
        }
        return;
    }

    if (evt == HCI_EVENT_PIN_CODE_REQUEST) {
        bd_addr_t addr;
        hci_event_pin_code_request_get_bd_addr(packet, addr);
        // 原则：永不参与传统 PIN 配对——回答 "0000" 既必败，
        // 万一成还会造出 legacy 弱密钥绑定。直接中止传统配对，
        // 让链路按"配对失败"收场，引擎升级全新 SSP 重试（Just Works 永远免密）。
        g_strategy = 1;
        g_pairing = true;
        Post(L"BTstack: 拒绝传统 PIN 配对 -> 引擎将全新 SSP 重试");
        gap_pin_code_negative(addr);
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
            if (g_connectAddr[0]) {
                Post(L"BTstack: \u5220\u9664\u5931\u6548\u5bc6\u94a5 %S", g_connectAddr);
                BtSvcForgetDevice(g_connectAddr);
            }
            g_connectAddr[0] = 0;
            g_phaseIdx.store(AnyStreamStreaming() ? 3 : 4);
        } else {
            Post(L"BTstack: 配对失败 status=0x%02x，重载密钥库（文件未动过）", st);
            // lk_del 只删内存不删文件：文件里仍是最后成功配对的好密钥，
            // 重载即恢复。耳机不在可连接状态时引擎稍后自动再试。
            LkLoad();
            g_phaseIdx.store(AnyStreamStreaming() ? 3 : 0);
        }
        return;
    }

    if (evt == GAP_EVENT_INQUIRY_RESULT) {
        bd_addr_t addr;
        gap_event_inquiry_result_get_bd_addr(packet, addr);
        // 过滤全零地址：已连接设备（如推流中的 QCY）会在扫描时产生地址为
        // 00:00:00:00:00:00 的查询回声，进列表会变成"同名两行"
        static const bd_addr_t kZero = { 0, 0, 0, 0, 0, 0 };
        if (memcmp(addr, kZero, 6) == 0) return;
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
    // 优雅停止：退出请求后先断流 + 断 ACL（400ms），再真正退出。
    // 直接杀栈会让耳机异常掉线并疯狂自动重连，与新栈的寻呼撞车（0x08 超时）
    if (g_exitReq.load()) {
        static DWORD stopDeadline = 0;
        if (!g_stopCleaned) {
            g_stopCleaned = true;
            int n = 0;
            for (int i = 0; i < NUM_STREAMS; ++i) {
                if (media[i].a2dp_cid != 0) {
                    Post(L"BTstack[%d]: 优雅停止：断开 A2DP %S", i, media[i].addr);
                    a2dp_source_disconnect(media[i].a2dp_cid);
                    ++n;
                }
                if (media[i].acl_handle != HCI_CON_HANDLE_INVALID)
                    gap_disconnect(media[i].acl_handle);
            }
            Post(L"BTstack: 优雅停止：已发断开 %d 路，等待释放完毕…", n);
            stopDeadline = GetTickCount() + 2000;
            btstack_run_loop_set_timer(ts, 100);
            btstack_run_loop_add_timer(ts);
            return;
        }
        // 等待全部流真正释放（对端确认 + 链路拆除），2 秒兜底
        bool allReleased = true;
        for (int i = 0; i < NUM_STREAMS; ++i) {
            if (media[i].a2dp_cid != 0 || media[i].stream_opened) { allReleased = false; break; }
        }
        if (!allReleased && GetTickCount() < stopDeadline) {
            btstack_run_loop_set_timer(ts, 100);
            btstack_run_loop_add_timer(ts);
            return;
        }
        if (!allReleased) Post(L"BTstack: 优雅停止：等待释放超时（2s），强制退出");
        else             Post(L"BTstack: 优雅停止：全部释放完毕，退出");
        btstack_run_loop_trigger_exit();
        return;
    }

    if (g_connBackoff > 0) g_connBackoff--;

    // page scan 协调（业界标准做法，参照 ESP-IDF a2dp_source 的 NON_CONNECTABLE）：
    // 主动连接进行中关闭 page scan —— 耳机同时对我们的寻呼无人应答，自然放弃，
    // 消除双向建链对撞（dongle LC 缠死的主要诱因）；拆链冷却期同样关闭（断开后
    // 10 秒内不接受回连）；其余状态恢复打开 —— 掉线后耳机可回连自动恢复（Windows 模式）
    {
        static bool s_scanDesired = true;
        bool want = true;
        if (g_tearDownAddr[0] && GetTickCount() - g_tearDownMs < 10000) want = false;
        if (g_connectAddr[0] && AnyStreamInSetup())                    want = false;
        if (want != s_scanDesired) {
            s_scanDesired = want;
            Post(L"BTstack: page scan %s", want ? L"开（接受耳机回连）" : L"关（拒绝回连/防对撞）");
            gap_connectable_control(want ? 1 : 0);
        }
    }

    // 看门狗：上电后 15 秒仍未就绪（USB 打开失败后 btstack 不会自动重试），
    // 重启整个栈（线程循环会重建传输层再试）。就绪后本计数停止累计。
    // 注意：无论是否触发，都必须把定时器挂回去，否则轮询链断掉、看门狗失效。
    if (!g_btUp && !g_exitReq.load()) {
        static int bootTicks = 0;
        if (++bootTicks >= 75) {          // 75 * 200ms = 15s
            bootTicks = 0;
            if (++g_bootFails >= 3) {
                g_bootFails = 0;
                // 控制器固件挂死：USB 层活着但控制器不再应答 HCI，
                // 栈重启救不了——必须物理拔插 dongle 断电复位
                g_adapterDead.store(true);
                Post(L"BTstack: 适配器控制器无响应（连续 3 次启动失败）。请拔插 dongle 后自动恢复。");
            } else {
                Post(L"BTstack: 15 秒未就绪（USB 打开失败），自动重试…");
            }
            btstack_run_loop_set_timer(ts, 200);
            btstack_run_loop_add_timer(ts);
            g_restartReq.store(true);
            btstack_run_loop_trigger_exit();
            return;
        }
    }
    // 异步就绪门控：BTSTACK_EVENT_STATE（HCI 上电完成事件）未到前，
    // 不向控制器发任何命令——所有命令保持排队，就绪后下一拍自动执行。
    // （之前栈未上电就发 establish → 0x0c 命令不允许风暴）
    if (!g_btUp) {
        btstack_run_loop_set_timer(ts, 200);
        btstack_run_loop_add_timer(ts);
        return;
    }

    // 配对成功后延迟建流（等 dedicated bonding 的链路彻底断开）
    if (g_pairDelayConnect) {
        if (++g_pairDelayTicks >= 25) {   // 25 * 200ms = 5s
            // 5s 而不是更短：TWS 配对完成后有几秒"整理期"（左右耳同步、
            // 新密钥写闪存），期间不响应寻呼——太早建流必吃 0x08
            g_pairDelayConnect = false;
            g_cmdConnect.store(true);
        }
    }

    // 自愈引擎：目标已设且完全空闲时，按策略每 5 秒自动推进（永不停，无需人工干预）
    {
        if (++g_engineTick >= 25) {       // 25 * 200ms = 5s
            g_engineTick = 0;
            if (g_connBackoff > 0) { /* 0x08 退避中：暂停寻呼，让对方入站重连落到栈上 */ }
            else if (g_passiveAfterDrop) { /* 断开后被动等待：耳机回连自动恢复，不主动寻呼 */ }
            else if (g_connectAddr[0] && !AnyStreamActive() && !g_pairing && !g_pairForce
                && !g_pairDelayConnect && !g_inAutoRepair) {
                g_phaseIdx.store(2);
                if (g_strategy == 1) {
                    // 策略1：全新配对（配对失败=耳机不在，冷却 5s 后自动再试；
                    // 耳机一旦进入配对模式即自动接续）
                    static int bondCd = 0;
                    if (bondCd > 0) { bondCd--; }
                    else {
                        bondCd = 25;      // 5s 冷却
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
    // 寻呼限幅：一路正在推流时，另一路的建流寻呼不得整段跑完——
    // 对端不在场时寻呼会空跑满 5 秒超时，期间推流包间隔被挤到 150~420ms
    // （诊断已证实，耳机缓冲打穿=可闻卡顿）。1.2 秒未通即主动撤销让位推流；
    // 撤销触发的失败按暂态退避，引擎稍后再试（寻呼占空比大幅下降）
    static int g_pageTicks = 0;
    if (AnyStreamStreaming() && AnyStreamInSetup() && !g_pairing && !g_pairForce) {
        if (++g_pageTicks >= 10) {         // 10 * 200ms = 2s（QCY 寻呼慢，1.2s 太紧）
            g_pageTicks = 0;
            for (int i = 0; i < NUM_STREAMS; ++i) {
                if (media[i].a2dp_cid != 0 && !media[i].stream_opened && media[i].addr[0]) {
                    ClassicConnectCancel(media[i].addr);
                    g_connBackoff = 15;   // 取消后歇 3s：防止引擎瞬时重试空转 churn
                    Post(L"BTstack[%d]: 寻呼限幅：2s 未通即撤销（保推流），3s 后自动重试", i);
                }
            }
        }
    } else {
        g_pageTicks = 0;
    }

    // 音量探测落位：耳机 1.6s 内没上报可信音量（假零被过滤/无通知）→
    // 用记忆值（上次真实音量）主动落位；没记住过则 50%。
    // 这让显示和真实一致，而不是悬在"未知"
    for (int i = 0; i < NUM_STREAMS; ++i) {
        if (media[i].volProbeTicks > 0 && media[i].avrcp_cid && media[i].addr[0]) {
            if (--media[i].volProbeTicks == 0) {
                if (media[i].hpSetLastMs == 0 && media[i].hpVolume <= 0) {
                    int v = VolRemembered(media[i].addr);
                    if (v < 0) v = 64;   // 没记住过：50% 起步
                    avrcp_controller_set_absolute_volume(media[i].avrcp_cid, (uint8_t)v);
                    media[i].hpSetPending++;
                    media[i].hpSetLastMs = GetTickCount();
                    media[i].hpVolume = v;   // 乐观更新（应答会校正）
                    Post(L"AVRCP: 耳机未上报音量，应用记忆值 %d%%", v * 100 / 127);
                }
            }
        }
    }

    // AVRCP 延迟连接：推流启动 3 秒后（避开另一路刚起流的最敏感窗口）
    for (int i = 0; i < NUM_STREAMS; ++i) {
        if (media[i].avrcpDelayTicks > 0 && media[i].streaming) {
            if (--media[i].avrcpDelayTicks == 0 && media[i].avrcp_cid == 0) {
                bd_addr_t raddr;
                sscanf_bd_addr(media[i].addr, raddr);
                int r = avrcp_connect(raddr, &media[i].avrcp_cid);
                Post(L"BTstack[%d]: 主动发起 AVRCP 连接 %S ret=%d（延迟 3s）", i, media[i].addr, r);
            }
        }
    }

    // 跳实时（复制开始等推流情况变化）：清空各路待发缓存，从实时边开始
    if (g_cmdJumpLive.exchange(false)) {
        JumpAllToLive();
        Post(L"BTstack: 已清空待发缓存，从实时边开始推流");
    }

    // 设置耳机音量（来自 UI，转到本线程执行 AVRCP 命令）。
    // 缓存已在 BtSvcSetHpVolume 调用点乐观更新（消除轮询延迟）。
    // 每笔命令记一笔"在途"：应答按序到达，只有最后一笔的应答才写缓存，
    // 否则中间应答（已被新值取代的旧音量）会把缓存来回拉扯（40→35→40 抖动）
    if (g_cmdHpVolume.exchange(false)) {
        int i = FindStreamByAddr(g_hpVolAddr);
        if (i >= 0 && media[i].avrcp_cid) {
            avrcp_controller_set_absolute_volume(media[i].avrcp_cid, (uint8_t)g_hpVolVal);
            media[i].hpSetPending++;
            media[i].hpSetLastMs = GetTickCount();
            Post(L"AVRCP: 设置 %S 音量 -> %d%%", g_hpVolAddr, g_hpVolVal * 100 / 127);
        }
        g_hpVolAddr[0] = 0;
    }

    // 按地址断开（双路）：只断选中的那路，另一路不受影响
    if (g_cmdDisconnectAddr.exchange(false)) {
        int i = FindStreamByAddr(g_discAddr);
        if (i >= 0) {
            Post(L"BTstack[%d]: 断开 %S（单路）", i, g_discAddr);
            // 顺序拆除：先关 A2DP 信令，SIGNALING_CONNECTION_RELEASED 到达后再拆 ACL
            // （同时拆会让 dongle 的 LC 状态机两件事一起做，是第二次连接缠死的诱因之一）
            if (media[i].a2dp_cid != 0)
                a2dp_source_disconnect(media[i].a2dp_cid);
            if (media[i].acl_handle != HCI_CON_HANDLE_INVALID)
                media[i].aclDropPending = 1;   // 信令释放事件里补拆 ACL
            btstack_run_loop_remove_timer(&media[i].audio_timer);
            // a2dp_cid / stream_opened / streaming 交给释放事件清（此处清零会让
            // 释放事件按 cid 找不到槽位 [-1]，aclDropPending 失效、状态残留）。
            // addr 先清：UI 立即脱离本路，也断开各处按地址的匹配
            media[i].avrcp_cid = 0;   // 在途的 AVRCP 连接不再归属本路（否则失败事件 0x81 空报）
            media[i].addr[0] = 0;
            if (g_connectReqAddr[0] && _stricmp(g_discAddr, g_connectReqAddr) == 0)
                g_connectReqAddr[0] = 0;   // 该地址的"连接中"动画随断开结束
            // 该地址进入短拒绝窗，防它秒回连
            strcpy_s(g_rejectAddr, sizeof(g_rejectAddr), g_discAddr);
            g_rejectUntil.store(GetTickCount() + 8000);
            // 拆链冷却 10 秒：引擎自动重试与用户重连都让路（见 g_tearDownMs）
            strcpy_s(g_tearDownAddr, sizeof(g_tearDownAddr), g_discAddr);
            g_tearDownMs = GetTickCount();
            g_connBackoff = 50;    // 引擎自动直连退避 10s
            // 若正是当前目标：清目标并进入被动
            if (g_connectAddr[0] && _stricmp(g_connectAddr, g_discAddr) == 0) {
                g_connectAddr[0] = 0;
                g_passiveAfterDrop = true;
            }
            g_phaseIdx.store(AnyStreamStreaming() ? 3 : 0);
        }
        // 用户主动断开的设备：清掉自动重连目标，防软复位自愈时"复活"它
        if (g_addr[0] && g_discAddr[0] && _stricmp(g_addr, g_discAddr) == 0)
            g_addr[0] = 0;
        g_discAddr[0] = 0;
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
        // 双路安全：只断当前配对目标的句柄，不碰另一副耳机的链路
        hci_con_handle_t th = HandleForAddr(g_connectAddr);
        if (th != HCI_CON_HANDLE_INVALID) {
            Post(L"BTstack: \u65ad\u5f00\u6b8b\u7559 ACL\uff0c\u591a\u7b49 2s");
            gap_disconnect(th);
            ClearHandleForAddr(g_connectAddr);
            g_pairTicks = -8;           // 断开后多等约 2s，让链路彻底干净（0x0b 防护）
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

    // 建流卡死看门狗（自适应）：只管 ACL 建成之后的阶段 —— 建立中的流只要收到
    // 对端任何 A2DP/AVDTP 事件就续命，连续 15 秒零进展才判定挂死、断开重试。
    // 寻呼阶段（ACL 未建立）不归它管：K3 类耳机寻呼应答可达 15 秒以上，
    // 由控制器的 page timeout（24.6s）兜底，K3 真不在线控制器自己会报 0x04。
    // 只杀当前目标的流；旧目标的残留流直接清理。
    if (AnyStreamInSetup() && !g_pairing) {
        uint32_t nowMs = btstack_run_loop_get_time_ms();
        for (int i = 0; i < NUM_STREAMS; ++i) {
            bool inSetup   = media[i].a2dp_cid != 0 && !media[i].stream_opened;
            bool openedIce = media[i].stream_opened && !media[i].streaming;   // 打开了但迟迟不 START
            if (media[i].a2dp_cid == 0 || (!inSetup && !openedIce)) continue;
            if (inSetup && media[i].acl_handle == HCI_CON_HANDLE_INVALID) continue;   // 寻呼进行中
            uint32_t lim  = inSetup ? 15000 : 10000;
            uint32_t idle = nowMs - (inSetup ? media[i].setupLastActiveMs : media[i].openedMs);
            if (idle <= lim) continue;
            bool isTarget = g_connectAddr[0] &&
                            _stricmp(media[i].addr, g_connectAddr) == 0;
            if (inSetup)
                Post(L"BTstack[%d]: 建流 %.1f 秒无任何进展，断开重试…", i, idle / 1000.0f);
            else
                Post(L"BTstack[%d]: 流已打开但 %.1f 秒未收到 START（对端卡死），断开重试…",
                     i, idle / 1000.0f);
            // 先撤销底层 create_connection：超时常因为 ACL 一直没完成
            // （对端不应答 page），不撤销的话悬空 page 会占住基带，
            // 之后的重试连不上、扫描也扫不出任何设备
            ClassicConnectCancel(media[i].addr);
            a2dp_source_disconnect(media[i].a2dp_cid);
            // 状态必须一次清干净：如果只清 a2dp_cid，随后的信令断开事件按 cid
            // 找不到槽位（-1），stream_opened 残留 → 槽位变僵尸"已在推流"，
            // 该地址从此连不上（02:04:15 的 K3 就是这么死的）
            media[i].a2dp_cid = 0;
            media[i].stream_opened = 0;
            media[i].streaming = 0;
            media[i].avrcp_cid = 0;
            // 掐流同样进入拆链冷却：立刻重试只会再次缠死 dongle
            strcpy_s(g_tearDownAddr, sizeof(g_tearDownAddr), media[i].addr);
            g_tearDownMs = GetTickCount();
            g_connBackoff = 50;
            // 连续两轮掐流 = 对端链路状态/dongle LC 层已经缠死。实测进程内软复位
            // 救不了（USB 重开失败），改为被动等待 + 提示用户重启程序；
            // 被动期间耳机主动回连的入站链路仍然可用
            if (++g_setupKillStreak >= 2 && isTarget && g_connectAddr[0]) {
                Post(L"BTstack: 连续两轮建流无进展 —— 进入被动等待；建议重启程序恢复主动连接");
                g_passiveAfterDrop = true;
                g_setupKillStreak = 0;
            }
            if (!isTarget) {
                // 旧目标的残留：清理掉，不再重试（避免与当前目标打架）
                Post(L"BTstack[%d]: %S 的旧尝试放弃（当前目标 %S）",
                     i, media[i].addr, g_connectAddr);
                media[i].addr[0] = 0;
            }
            // 当前目标：不清 addr，引擎按现有策略 5 秒后自动重试
        }
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
            // 立即重连不再设冷却：对撞已由 page scan 协调根治（主动连接期间
            // 耳机的回连寻呼无人应答），残留旧 ACL 由 0x0b 复用路径兜底
            g_scanActive = false;
            g_passiveMode = false;
            gap_inquiry_stop();
            app_state = 1;
            // 已有路在推流时不降级全局状态（推流中+连接第二路并行）
            if (!AnyStreamStreaming()) g_phaseIdx.store(2);
            g_pairing = false;
            g_rejectAddr[0] = 0;            // 用户主动连接：撤销拒绝名单
            g_rejectUntil.store(0);

            // 用户点了新设备 = 用户意图最高优先级：
            // 清空排队队列（里面的旧目标不再自动接续）。
            // 注意：绝不取消其它地址正在建立中的流 —— 双路下那会把第一台设备
            // 的建立拆掉（02:06:46 实测：点第二台 = 第一台信令被断）。各槽位
            // 独立建立，新目标走 FindFreeStream 拿自己的槽位即可
            if (g_pendingCount > 0) {
                Post(L"BTstack: 清空 %d 个排队连接（用户改连 %S）", g_pendingCount, g_connectAddr);
                g_pendingCount = 0;
            }

            // 多路：已有此地址的流 → 复用；否则找空闲流
            // （stream_opened 必须配着活的 a2dp_cid 才算真在推流——防僵尸槽位）
            int i = FindStreamByAddr(g_connectAddr);
            if (i >= 0 && media[i].stream_opened && media[i].a2dp_cid != 0) {
                // 该地址已在推流：不重复建流，仅恢复状态显示
                Post(L"BTstack[%d]: %S 已在推流，忽略重复连接", i, g_connectAddr);
                g_phaseIdx.store(3);
            } else if (i >= 0 && media[i].a2dp_cid != 0 && !media[i].stream_opened) {
                // 该地址已在建立中：忽略重复点击（重启建立会饿死 5s 看门狗）
                Post(L"BTstack[%d]: %S 已在连接中，忽略重复点击", i, g_connectAddr);
                g_phaseIdx.store(2);
            } else {
            if (i < 0) i = FindFreeStream();
            if (i < 0) {
                Post(L"BTstack: 所有推流通道已占用（上限 %d 路，断开一些再连）", NUM_STREAMS);
            } else if (g_pairing || g_pairForce) {
                // 配对进行中：establish 会与 bonding 抢链路（互相污染），排队等配对结束
                if (g_pendingCount < 8) {
                    strcpy_s(g_pendingAddr[g_pendingCount], 32, g_connectAddr);
                    g_pendingCount++;
                    Post(L"BTstack: 配对进行中，%S 已排队，配对结束后自动连接", g_connectAddr);
                }
            } else if (AnyStreamInSetup() && FindStreamByAddr(g_connectAddr) != i) {
                // 串行化：有流正在建立中，后续路排队等待（避免 btstack SEP 发现并发断言）
                if (g_pendingCount < 8) {
                    strcpy_s(g_pendingAddr[g_pendingCount], 32, g_connectAddr);
                    g_pendingCount++;
                    Post(L"BTstack: 有流正在建立中，%S 已排队，就绪后自动连接", g_connectAddr);
                } else {
                    Post(L"BTstack: 排队已满，%S 的连接请求被忽略", g_connectAddr);
                }
            } else if (!BtSvcIsPaired(g_connectAddr)) {
                // 没有密钥：直连必踩 PIN 陷阱（耳机有旧绑定）→ 直接全新配对，
                // 省掉"直连失败 0x66 → 升级"的整轮浪费
                Post(L"BTstack: %S 无密钥，直接全新配对", g_connectAddr);
                g_strategy = 1;
                g_pairForce = true;
                g_pairTicks = 0;
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
                    MarkConnectReq(g_connectAddr);   // UI 从此刻起显示"连接中"动画
                    Post(L"BTstack[%d]: 开始连接 %S", i, g_connectAddr);
                    bd_addr_t target;
                    sscanf_bd_addr(g_connectAddr, target);
                    uint8_t st = a2dp_source_establish_stream(target, &media[i].a2dp_cid);
                    Post(L"BTstack[%d]: establish_stream status=0x%02x cid=0x%04x", i, st, media[i].a2dp_cid);
                    media[i].setupLastActiveMs = btstack_run_loop_get_time_ms();
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
    // 记住目标：栈软复位（dongle 卡死自愈）后 BTSTACK_EVENT_STATE 会用
    // g_addr 自动重连本设备——用户无需再点一次
    strcpy_s(g_addr, sizeof(g_addr), addr);
    g_passiveAfterDrop = false;   // 用户点击 = 主动意图，解除被动等待
    // 防撞车：重启后 15 秒内点已配对设备，先给 3 秒被动窗口——
    // TWS 耳机断开后会立刻回连我们（入站），等它落进来比双向寻呼对撞快且稳；
    // 3 秒没等到（耳机不在回连状态），引擎照常主动寻呼
    DWORD up = GetTickCount() - g_serviceStartTick;
    if (up < 15000 && BtSvcIsPaired(addr) && g_connBackoff < 15)
        g_connBackoff = 15;   // 3 秒被动窗口
    g_cmdConnect.store(true);
}

void BtSvcDisconnect()
{
    if (!g_thread.joinable()) return;
    g_phaseIdx.store(0);   // 立即反馈：UI 马上退出"推流中"动画，不等线程 tick（线程内会再清一次状态）
    g_disconnectTick.store(GetTickCount());
    g_cmdDisconnect.store(true);
}

// 按地址断开（双路：点谁断谁，另一路不动）——声明见命令标志区
void BtSvcDisconnectAddr(const char* addr)
{
    if (!g_thread.joinable() || !addr || !addr[0]) return;
    strcpy_s(g_discAddr, sizeof(g_discAddr), addr);
    g_disconnectTick.store(GetTickCount());
    g_cmdDisconnectAddr.store(true);
}

void BtSvcPair(const char* addr)
{
    if (!g_thread.joinable() || !addr) return;
    strcpy_s(g_connectAddr, sizeof(g_connectAddr), addr);
    g_cmdPair.store(true);
}

// 设置已配对设备的名字（与密钥同文件存储）
bool BtSvcSetName(const char* addr, const char* nameUtf8)
{
    if (!addr || !addr[0] || !nameUtf8) return false;
    std::lock_guard<std::recursive_mutex> lk(g_lkLock);
    // 线程未跑时内存库可能没加载：先从文件刷一遍
    if (g_lkMap.empty()) {
        g_lkMap.clear();
        LkParseFile(k_lkFilePath.c_str());
    }
    char up[32] = "";
    for (int i = 0; addr[i] && i < 31; ++i)
        up[i] = (char)toupper((unsigned char)addr[i]);
    auto it = g_lkMap.find(up);
    if (it == g_lkMap.end()) return false;
    // 幂等：名字没变就不写盘（面板每 500ms 同步一次，靠这个挡住无谓 IO）
    if (it->second.nameUtf8 == nameUtf8) return true;
    it->second.nameUtf8 = nameUtf8;
    LkWriteFile(k_lkFilePath.c_str());
    return true;
}

// 枚举已配对设备（addr/name 均为 UTF-8）
void BtSvcEnumPaired(void (*cb)(const char* addr, const char* nameUtf8, void* user), void* user)
{
    std::lock_guard<std::recursive_mutex> lk(g_lkLock);
    if (g_lkMap.empty()) {
        g_lkMap.clear();
        LkParseFile(k_lkFilePath.c_str());
    }
    for (auto& kv : g_lkMap)
        cb(kv.first.c_str(), kv.second.nameUtf8.c_str(), user);
}

bool BtSvcIsPaired(const char* addr)
{
    if (!addr || !addr[0]) return false;
    std::lock_guard<std::recursive_mutex> lk(g_lkLock);
    // 查内存密钥库（与栈实际用于鉴权的密钥一致，避免文件/内存不同步导致误判）
    char up[32] = "";
    for (int i = 0; addr[i] && i < 31; ++i)
        up[i] = (char)toupper((unsigned char)addr[i]);
    return g_lkMap.find(up) != g_lkMap.end();
}

void BtSvcForgetDevice(const char* addr)
{
    if (!addr || !addr[0]) return;
    std::lock_guard<std::recursive_mutex> lk(g_lkLock);
    // 内存删除 + 整体重写文件（单一数据源，锁内串行）
    g_lkMap.erase(addr);
    LkSave();
    // 清掉自动重连目标，否则会继续无密钥重连并干扰扫描
    if (g_connectAddr[0] && _stricmp(g_connectAddr, addr) == 0) {
        g_connectAddr[0] = 0;
        g_phaseIdx.store(AnyStreamStreaming() ? 3 : 0);
    }
    Post(L"BTstack: 已忘记设备 %S（下次连接将重新配对）", addr);
}

void BtSvcSetTestTone(bool on)
{
    // 全局开关（兼容旧入口）：作用于所有路
    for (int i = 0; i < NUM_STREAMS; ++i) media[i].testTone = on ? 1 : 0;
    Post(L"BTstack: 测试音（全部）= %s", on ? L"开" : L"关");
}

// 按设备测试音：只开关指定地址的那一路
bool BtSvcTestToneAddr(const char* addr)
{
    int i = FindStreamByAddr(addr);
    return i >= 0 && media[i].testTone != 0;
}

void BtSvcSetTestToneAddr(const char* addr, bool on)
{
    int i = FindStreamByAddr(addr);
    if (i >= 0) media[i].testTone = on ? 1 : 0;
}

// ------------------------------------------------------------ SBC 码率上限（用户可选）

void BtSvcSetBitpoolCap(int cap)
{
    if (cap < 2) cap = 2;
    if (cap > 53) cap = 53;
    if ((int)g_bpCap != cap) {
        g_bpCap = cap;
        Post(L"BTstack: 码率上限 = bp%d（重连后生效）", cap);
    }
}

int BtSvcGetBitpoolCap() { return g_bpCap; }

// ------------------------------------------------------------ 耳机音量（AVRCP 绝对音量）

// 每设备记忆音量（上次真实值），持久化 volumes.txt。
// 用途：耳机注册通知的 interim 上报假 0（AirPods 怪癖）时，
// 用记忆值主动落位——显示与真实一致，而不是悬在"未知"
static std::recursive_mutex          g_volLock;
static std::map<std::string, int>    g_volMap;
static bool                         g_volLoaded = false;

static void VolLoad()
{
    FILE* f = nullptr;
    if (fopen_s(&f, AppFile("volumes.txt").c_str(), "r") != 0 || !f) return;
    char line[64];
    while (fgets(line, sizeof(line), f)) {
        char a[32] = ""; int v = 0;
        if (sscanf_s(line, "%31s %d", a, (unsigned)sizeof(a), &v) == 2 && a[0])
            g_volMap[a] = v;
    }
    fclose(f);
}

// 记住某设备的真实音量（通知/应答里的权威值到达时调用）
static void VolRemember(const char* addr, int vol)
{
    if (!addr || !addr[0] || vol < 0 || vol > 127) return;
    std::lock_guard<std::recursive_mutex> lk(g_volLock);
    if (!g_volLoaded) { g_volLoaded = true; VolLoad(); }
    char up[32] = "";
    for (int i = 0; addr[i] && i < 31; ++i)
        up[i] = (char)toupper((unsigned char)addr[i]);
    auto it = g_volMap.find(up);
    if (it != g_volMap.end() && it->second == vol) return;   // 没变不写盘
    g_volMap[up] = vol;
    FILE* f = nullptr;
    if (fopen_s(&f, AppFile("volumes.txt").c_str(), "w") != 0 || !f) return;
    for (auto& kv : g_volMap) fprintf(f, "%s %d\n", kv.first.c_str(), kv.second);
    fclose(f);
}

// 取记忆音量（-1 = 没记住过）
static int VolRemembered(const char* addr)
{
    if (!addr || !addr[0]) return -1;
    std::lock_guard<std::recursive_mutex> lk(g_volLock);
    if (!g_volLoaded) { g_volLoaded = true; VolLoad(); }
    char up[32] = "";
    for (int i = 0; addr[i] && i < 31; ++i)
        up[i] = (char)toupper((unsigned char)addr[i]);
    auto it = g_volMap.find(up);
    return it == g_volMap.end() ? -1 : it->second;
}

// 读某路的耳机当前音量（0..127；-1 = 未知/未连接 AVRCP）
int BtSvcGetHpVolume(const char* addr)
{
    int i = FindStreamByAddr(addr);
    return (i >= 0) ? media[i].hpVolume : -1;
}

// 设置某路的耳机音量（0..127）。经命令队列转到 BTstack 线程执行
// （命令变量声明在顶部命令标志区，poll 中使用）

void BtSvcSetHpVolume(const char* addr, int vol127)
{
    if (!addr || !addr[0]) return;
    if (vol127 < 0) vol127 = 0;
    if (vol127 > 127) vol127 = 127;
    strcpy_s(g_hpVolAddr, sizeof(g_hpVolAddr), addr);
    g_hpVolVal = vol127;
    g_cmdHpVolume.store(true);
    // 立即乐观更新缓存：UI 下一帧就能读到，消除命令队列 200ms 轮询延迟
    // （耳机应答 0x10 到达时用真实值覆盖；耳机本地按键通知同样覆盖）
    int i = FindStreamByAddr(addr);
    if (i >= 0) media[i].hpVolume = vol127;
}

void BtSvcSetGain(float gain)
{
    if (gain < 0) gain = 0;
    if (gain > 40) gain = 40;   // ~+32dB 上限（编码前有 int16 削波保护）
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
    // 不直接 trigger_exit！优雅停止由 poll 定时器完成：
    //   第一拍：断 A2DP 流 + 断 ACL（耳机看到正常断开，不触发疯狂重连）
    //   第二拍（400ms 后）：btstack_run_loop_trigger_exit
    // 若 run loop 已不在（USB 卡死等），3 秒超时分离兜底。
    HANDLE h = g_thread.native_handle();
    if (WaitForSingleObject(h, 5000) == WAIT_OBJECT_0) {
        g_thread.join();
    } else {
        g_thread.detach();             // 放任后台自行退出（罕见：线程卡死）
    }
    g_thread = {};
    g_running.store(false);
    g_phaseIdx.store(0);
}

bool BtSvcRunning() { return g_running.load(); }
bool BtSvcScanActive() { return g_scanActive; }
int  BtSvcPhase() { return g_phaseIdx.load(); }
bool BtSvcAdapterDead() { return g_adapterDead.load(); }
bool BtSvcIsStreamingAddr(const char* addr)
{
    int i = FindStreamByAddr(addr);
    return i >= 0 && media[i].stream_opened && media[i].streaming;
}
bool BtSvcIsConnectingAddr(const char* addr)
{
    int i = FindStreamByAddr(addr);
    // 流层面：建立中（cid 在、未开流）或已开流但未收到 START（打开≠开始）
    if (i >= 0 && media[i].a2dp_cid != 0 && !media[i].streaming) return true;
    // 点击层：用户点了连接 / 自动重连排队中 —— 到推流为止始终显示"连接中"。
    // （热 ACL 复用时整个建流 <1 秒，只看流状态会跳过连接中动画）
    if (g_connectReqAddr[0] && _stricmp(addr, g_connectReqAddr) == 0
        && GetTickCount() - g_connectReqTick < 15000
        && !BtSvcIsStreamingAddr(addr))
        return true;
    return false;
}
bool BtSvcThreadActive() { return g_thread.joinable(); }
bool BtSvcTestTone() { return g_testTone.load(); }

// ------------------------------------------------------------ 推流跳实时（清待发缓存，从此刻开始）

// 推流情况变化（复制开始/恢复、测试音切换）时调用：清空待发缓存并跳到
// 实时边——连接期间攒下的陈旧音频不再发，从"此刻"的采集数据开始
void BtSvcJumpToLive()
{
    g_cmdJumpLive.store(true);
}

// ------------------------------------------------------------ 采集信号指示器（UI 防抖指示）
static std::atomic<float>     g_capLevel{ 0.0f };      // 平滑电平（服务侧快起慢落，可多读者）
static volatile DWORD        g_capSoundMs = 0;         // 最近一次检测到非静音的时刻

// 当前采集电平（0..1，纯读，多读者共享：主指示器 + 各设备行）
float BtSvcCaptureLevel()
{
    return g_capLevel.load();
}

// 采集是否活着：近 1.5s 内出现过非静音信号
bool BtSvcCaptureAlive()
{
    DWORD ms = g_capSoundMs;
    return ms != 0 && GetTickCount() - ms < 1500;
}

void BtSvcFeedAudio(const short* pcm, int frames, int feedRate)
{
    // 采集信号测量（供 UI 指示器）：块峰值 → 服务侧平滑（快起慢落）
    {
        int pk = 0;
        for (int i = 0; i < frames * NUM_CHANNELS; ++i) {
            int a = pcm[i] < 0 ? -pcm[i] : pcm[i];
            if (a > pk) pk = a;
        }
        float p = (float)pk / 32768.0f;
        float sm = g_capLevel.load();
        g_capLevel.store(p > sm ? p : sm * 0.90f);      // 快起，慢落（按块）
        if (pk > 200) g_capSoundMs = GetTickCount();   // 静音阈值 ~0.6%
    }

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
    // Windows 默认定时器精度 15.6ms——音频发送要求 10ms 节拍，不提高精度
    // 会按 15.6ms 量化，节拍抖动直接表现为周期性卡顿
    timeBeginPeriod(1);
    g_serviceStartTick = GetTickCount();   // 服务启动时刻（防撞车被动窗口用）
    while (!g_exitReq.load()) {
        g_stopCleaned = false;
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
    timeEndPeriod(1);   // 恢复系统默认定时器精度
    return 0;
}
