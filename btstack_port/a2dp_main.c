// 最小可跑的 A2DP Source：扫描 -> 找到音频设备 -> 连接配对 -> 推送音频
// 用法: BtA2dp.exe [BD_ADDR]     不带参数则自动连接扫描到的第一个音频设备

#include <Windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "btstack_config.h"
#include "btstack_event.h"
#include "btstack_memory.h"
#include "btstack_run_loop.h"
#include "btstack_run_loop_windows.h"
#include "classic/a2dp_source.h"
#include "classic/avdtp.h"
#include "classic/btstack_sbc_bluedroid.h"
#include "classic/device_id_server.h"
#include "classic/sdp_server.h"
#include "bluetooth_company_id.h"
#include "hci.h"
#include "hci_dump.h"
#include "hci_dump_windows_stdout.h"
#include "hci_transport.h"
#include "hci_transport_usb.h"

#define NUM_CHANNELS            2
#define AUDIO_TIMEOUT_MS        10
#define SBC_STORAGE_SIZE        1030
#define PREFERRED_SAMPLING_RATE 44100
#define INQUIRY_DURATION_1280MS 8

static const char * target_addr_string = "";    // 空 = 自动选第一个音频设备

typedef struct {
    uint16_t a2dp_cid;
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
} sending_ctx_t;

static sending_ctx_t media;
static int      current_sample_rate = PREFERRED_SAMPLING_RATE;
static int      app_state = 0;          // 0=扫描中 1=连接中 2=推流中
static bd_addr_t target_addr;
static int      have_target = 0;
static int      scan_rounds = 0;

static btstack_sbc_encoder_state_t   sbc_encoder_state;
static const btstack_sbc_encoder_t * sbc_encoder_instance;

static btstack_packet_callback_registration_t hci_event_callback_registration;
static avdtp_stream_endpoint_t * local_stream_endpoint;
static btstack_timer_source_t audio_timer;

static uint8_t sdp_a2dp_source_service_buffer[150];
static uint8_t device_id_sdp_service_buffer[100];

static uint8_t media_sbc_codec_capabilities[] = {
    (AVDTP_SBC_44100 << 4) | AVDTP_SBC_48000 | AVDTP_SBC_STEREO,
    0xFF,
    2, 53
};
static uint8_t media_sbc_codec_configuration[4];

typedef struct {
    int num_channels, sampling_frequency, block_length, subbands;
    int min_bitpool_value, max_bitpool_value;
    btstack_sbc_channel_mode_t      channel_mode;
    btstack_sbc_allocation_method_t allocation_method;
} media_codec_configuration_sbc_t;

static media_codec_configuration_sbc_t sbc_configuration;

// ------------------------------------------------------------ 链路密钥库
// Windows 注册表 BTHPORT\Parameters\Keys 里导出的密钥，免配对直接认证

static const char * k_airpods_addr = "28:2D:7F:D0:12:95";
static const link_key_t k_airpods_key = {
    0x8c,0xda,0x6f,0xe9,0x0e,0x0a,0x49,0x7c,0xb7,0x62,0xa0,0x4d,0x67,0x61,0x5a,0x1b
};

static int lk_get(bd_addr_t addr, link_key_t key, link_key_type_t * type)
{
    bd_addr_t a;
    sscanf_bd_addr(k_airpods_addr, a);
    if (memcmp(addr, a, 6) != 0) return 0;
    memcpy(key, k_airpods_key, 16);
    *type = AUTHENTICATED_COMBINATION_KEY_GENERATED_FROM_P192;
    printf("[key] using stored key for %s\n", bd_addr_to_str(addr));
    return 1;
}
static void lk_put(bd_addr_t addr, link_key_t key, link_key_type_t type)
{
    printf("[key] PAIRED with %s (key stored)\n", bd_addr_to_str(addr));
}
static void lk_del(bd_addr_t addr) {}
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

// ------------------------------------------------------------ 音频（正弦）

static void produce_audio(int16_t * pcm, int num_frames)
{
    static double phase = 0.0;
    double step = 2.0 * 3.14159265358979 * 441.0 / current_sample_rate;
    for (int i = 0; i < num_frames; ++i) {
        int16_t v = (int16_t)(sin(phase) * 8000.0);
        pcm[2 * i] = v;
        pcm[2 * i + 1] = v;
        phase += step;
        if (phase > 2.0 * 3.14159265358979) phase -= 2.0 * 3.14159265358979;
    }
}

// ------------------------------------------------------------ 发送

static void send_media_packet(void)
{
    int frame_len = sbc_encoder_instance->sbc_buffer_length(&sbc_encoder_state);
    uint8_t nframes = (uint8_t)(media.sbc_storage_count / frame_len);
    media.sbc_storage[0] = nframes;
    a2dp_source_stream_send_media_payload_rtp(media.a2dp_cid, media.local_seid, 0,
                                              media.rtp_timestamp,
                                              media.sbc_storage, media.sbc_storage_count + 1);
    media.rtp_timestamp += nframes * sbc_encoder_instance->num_audio_frames(&sbc_encoder_state);
    media.sbc_storage_count = 0;
    media.sbc_ready_to_send = 0;
}

static void fill_sbc_buffer(void)
{
    unsigned int spb = sbc_encoder_instance->num_audio_frames(&sbc_encoder_state);
    uint16_t flen = sbc_encoder_instance->sbc_buffer_length(&sbc_encoder_state);
    while (media.samples_ready >= spb
        && (media.max_media_payload_size - media.sbc_storage_count) >= flen) {
        int16_t pcm[256 * NUM_CHANNELS];
        produce_audio(pcm, spb);
        sbc_encoder_instance->encode_signed_16(&sbc_encoder_state, pcm,
                                               &media.sbc_storage[1 + media.sbc_storage_count]);
        media.sbc_storage_count += flen;
        media.samples_ready     -= spb;
    }
}

static void audio_timeout(btstack_timer_source_t * ts)
{
    btstack_run_loop_set_timer(ts, AUDIO_TIMEOUT_MS);
    btstack_run_loop_add_timer(ts);

    uint32_t now = btstack_run_loop_get_time_ms();
    uint32_t period = AUDIO_TIMEOUT_MS;
    if (media.time_audio_data_sent > 0) period = now - media.time_audio_data_sent;
    uint32_t n = (period * current_sample_rate) / 1000;
    media.acc_num_missed_samples += (period * current_sample_rate) % 1000;
    while (media.acc_num_missed_samples >= 1000) {
        n++;
        media.acc_num_missed_samples -= 1000;
    }
    media.time_audio_data_sent = now;
    media.samples_ready += n;

    if (media.sbc_ready_to_send) return;
    fill_sbc_buffer();
    if ((media.sbc_storage_count + sbc_encoder_instance->sbc_buffer_length(&sbc_encoder_state))
        > media.max_media_payload_size) {
        media.sbc_ready_to_send = 1;
        a2dp_source_stream_endpoint_request_can_send_now(media.a2dp_cid, media.local_seid);
    }
}

static void start_streaming(void)
{
    media.max_media_payload_size = btstack_min(a2dp_max_media_payload_size(media.a2dp_cid, media.local_seid),
                                               SBC_STORAGE_SIZE);
    media.sbc_storage_count = 0;
    media.sbc_ready_to_send = 0;
    media.streaming = 1;
    btstack_run_loop_remove_timer(&audio_timer);
    btstack_run_loop_set_timer_handler(&audio_timer, audio_timeout);
    btstack_run_loop_set_timer(&audio_timer, AUDIO_TIMEOUT_MS);
    btstack_run_loop_add_timer(&audio_timer);
}

// ------------------------------------------------------------ A2DP 事件

static void a2dp_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    if (packet_type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(packet) != HCI_EVENT_A2DP_META) return;

    uint8_t status, local_seid;
    uint16_t cid;

    switch (hci_event_a2dp_meta_get_subevent_code(packet)) {

    case A2DP_SUBEVENT_SIGNALING_CONNECTION_ESTABLISHED:
        status = a2dp_subevent_signaling_connection_established_get_status(packet);
        cid    = a2dp_subevent_signaling_connection_established_get_a2dp_cid(packet);
        if (status != ERROR_CODE_SUCCESS) {
            printf("[a2dp] signaling connect FAILED status=0x%02x\n", status);
            media.a2dp_cid = 0;
            app_state = 0;                       // 回到扫描，重新来
            gap_inquiry_start(INQUIRY_DURATION_1280MS);
            break;
        }
        media.a2dp_cid = cid;
        app_state = 1;
        printf("[a2dp] signaling CONNECTED cid=0x%02x local_seid=0x%02x\n", cid, media.local_seid);
        break;

    case A2DP_SUBEVENT_SIGNALING_MEDIA_CODEC_SBC_CONFIGURATION:
        cid = a2dp_subevent_signaling_media_codec_sbc_configuration_get_a2dp_cid(packet);
        if (cid != media.a2dp_cid) return;
        sbc_configuration.num_channels       = a2dp_subevent_signaling_media_codec_sbc_configuration_get_num_channels(packet);
        sbc_configuration.sampling_frequency = a2dp_subevent_signaling_media_codec_sbc_configuration_get_sampling_frequency(packet);
        sbc_configuration.block_length       = a2dp_subevent_signaling_media_codec_sbc_configuration_get_block_length(packet);
        sbc_configuration.subbands           = a2dp_subevent_signaling_media_codec_sbc_configuration_get_subbands(packet);
        sbc_configuration.min_bitpool_value  = a2dp_subevent_signaling_media_codec_sbc_configuration_get_min_bitpool_value(packet);
        sbc_configuration.max_bitpool_value  = a2dp_subevent_signaling_media_codec_sbc_configuration_get_max_bitpool_value(packet);
        sbc_configuration.allocation_method  = (btstack_sbc_allocation_method_t)
            (a2dp_subevent_signaling_media_codec_sbc_configuration_get_allocation_method(packet) - 1);
        switch (a2dp_subevent_signaling_media_codec_sbc_configuration_get_channel_mode(packet)) {
        case AVDTP_CHANNEL_MODE_JOINT_STEREO: sbc_configuration.channel_mode = SBC_CHANNEL_MODE_JOINT_STEREO; break;
        case AVDTP_CHANNEL_MODE_STEREO:       sbc_configuration.channel_mode = SBC_CHANNEL_MODE_STEREO; break;
        case AVDTP_CHANNEL_MODE_DUAL_CHANNEL: sbc_configuration.channel_mode = SBC_CHANNEL_MODE_DUAL_CHANNEL; break;
        default:                              sbc_configuration.channel_mode = SBC_CHANNEL_MODE_MONO; break;
        }
        printf("[a2dp] SBC negotiated fs=%u ch=%d block=%d sub=%d bitpool=%d..%d\n",
               sbc_configuration.sampling_frequency, sbc_configuration.num_channels,
               sbc_configuration.block_length, sbc_configuration.subbands,
               sbc_configuration.min_bitpool_value, sbc_configuration.max_bitpool_value);
        current_sample_rate = sbc_configuration.sampling_frequency;
        sbc_encoder_instance = btstack_sbc_encoder_bluedroid_init_instance(&sbc_encoder_state);
        sbc_encoder_instance->configure(&sbc_encoder_state, SBC_MODE_STANDARD,
                                        sbc_configuration.block_length, sbc_configuration.subbands,
                                        sbc_configuration.allocation_method,
                                        sbc_configuration.sampling_frequency,
                                        sbc_configuration.max_bitpool_value,
                                        sbc_configuration.channel_mode);
        break;

    case A2DP_SUBEVENT_STREAM_ESTABLISHED:
        status = a2dp_subevent_stream_established_get_status(packet);
        if (status != ERROR_CODE_SUCCESS) {
            printf("[a2dp] stream establish FAILED status=0x%02x\n", status);
            break;
        }
        printf("[a2dp] STREAM ESTABLISHED  <-- 语音链路打通\n");
        media.stream_opened = 1;
        app_state = 2;
        a2dp_source_start_stream(media.a2dp_cid, media.local_seid);
        break;

    case A2DP_SUBEVENT_STREAM_STARTED:
        printf("[a2dp] STREAMING ... 441Hz 正弦波推流中（耳机应该有声音）\n");
        start_streaming();
        break;

    case A2DP_SUBEVENT_STREAM_SUSPENDED:
        printf("[a2dp] suspended\n");
        btstack_run_loop_remove_timer(&audio_timer);
        break;

    case A2DP_SUBEVENT_STREAM_RELEASED:
        printf("[a2dp] stream released\n");
        media.stream_opened = 0;
        app_state = 0;
        btstack_run_loop_remove_timer(&audio_timer);
        break;

    case A2DP_SUBEVENT_STREAMING_CAN_SEND_MEDIA_PACKET_NOW:
        if (media.sbc_storage_count > 0) send_media_packet();
        if (media.streaming && media.stream_opened) {
            fill_sbc_buffer();
            if (media.sbc_storage_count > 0) {
                media.sbc_ready_to_send = 1;
                a2dp_source_stream_endpoint_request_can_send_now(media.a2dp_cid, media.local_seid);
            }
        }
        break;

    default:
        break;
    }
}

// ------------------------------------------------------------ HCI 事件（扫描/连接/配对）

static void hci_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    if (packet_type != HCI_EVENT_PACKET) return;
    uint8_t evt = hci_event_packet_get_type(packet);

    if (evt == HCI_EVENT_PIN_CODE_REQUEST) {
        bd_addr_t addr;
        hci_event_pin_code_request_get_bd_addr(packet, addr);
        printf("[pairing] PIN request from %s -> 0000\n", bd_addr_to_str(addr));
        gap_pin_code_response(addr, "0000");
        return;
    }

    if (evt == HCI_EVENT_CONNECTION_COMPLETE) {
        uint8_t st = hci_event_connection_complete_get_status(packet);
        bd_addr_t addr;
        hci_event_connection_complete_get_bd_addr(packet, addr);
        if (st == 0)
            printf("[acl] link to %s up (handle 0x%04x)\n",
                   bd_addr_to_str(addr), hci_event_connection_complete_get_connection_handle(packet));
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
        printf("[scan] %s  cod=0x%06x  %s\n", bd_addr_to_str(addr), cod,
               name[0] ? name : "(unnamed)");

        // 找到目标（指定地址）或第一个音频设备
        int match = 0;
        if (have_target && memcmp(addr, target_addr, 6) == 0) match = 1;
        else if (!have_target && ((cod >> 8) & 0x1F) == 0x04) match = 1;
        if (match && app_state == 0) {
            printf("[scan] >>> 目标命中，停止扫描并连接 <<<\n");
            have_target = 1;
            memcpy(target_addr, addr, 6);
            target_addr_string = bd_addr_to_str(target_addr);
            gap_inquiry_stop();
            a2dp_source_establish_stream(target_addr, &media.a2dp_cid);
        }
        return;
    }

    if (evt == GAP_EVENT_INQUIRY_COMPLETE) {
        if (app_state == 0) {
            ++scan_rounds;
            printf("[scan] 第 %d 轮结束，继续扫描（让耳机进入配对模式）...\n", scan_rounds);
            gap_inquiry_start(INQUIRY_DURATION_1280MS);
        }
        return;
    }

    if (evt != BTSTACK_EVENT_STATE) return;
    if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING) return;

    bd_addr_t a;
    gap_local_bd_addr(a);
    printf("\n=== BTstack UP  %s ===\n", bd_addr_to_str(a));
    printf("开始扫描 —— 请把耳机放进配对模式\n\n");
    gap_inquiry_start(INQUIRY_DURATION_1280MS);
}

// ------------------------------------------------------------ 初始化

static int app_init(void)
{
    hci_set_master_slave_policy(0);
    gap_ssp_set_auto_accept(1);                    // 数值确认自动接受
    hci_set_inquiry_mode(INQUIRY_MODE_RSSI_AND_EIR);

    l2cap_init();
    hci_set_link_key_db(&lk_db);
    a2dp_source_init();
    a2dp_source_register_packet_handler(&a2dp_packet_handler);

    local_stream_endpoint = a2dp_source_create_stream_endpoint(AVDTP_AUDIO, AVDTP_CODEC_SBC,
            media_sbc_codec_capabilities, sizeof(media_sbc_codec_capabilities),
            media_sbc_codec_configuration, sizeof(media_sbc_codec_configuration));
    if (!local_stream_endpoint) return 1;
    avdtp_set_preferred_sampling_frequency(local_stream_endpoint, PREFERRED_SAMPLING_RATE);
    media.local_seid = avdtp_local_seid(local_stream_endpoint);
    avdtp_source_register_delay_reporting_category(media.local_seid);

    sdp_init();
    memset(sdp_a2dp_source_service_buffer, 0, sizeof(sdp_a2dp_source_service_buffer));
    a2dp_source_create_sdp_record(sdp_a2dp_source_service_buffer, sdp_create_service_record_handle(),
                                  AVDTP_SOURCE_FEATURE_MASK_PLAYER, NULL, NULL);
    sdp_register_service(sdp_a2dp_source_service_buffer);

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

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 1) {
        target_addr_string = argv[1];
        sscanf_bd_addr(target_addr_string, target_addr);
        have_target = 1;
    }

    printf("=== DualBlt A2DP Source ===\n");
    printf("target: %s\n\n", have_target ? target_addr_string : "(自动：第一个扫描到的音频设备)");

    btstack_memory_init();
    btstack_run_loop_init(btstack_run_loop_windows_get_instance());
    hci_dump_init(hci_dump_windows_stdout_get_instance());

    hci_init(hci_transport_usb_instance(), NULL);
    hci_transport_usb_add_device(0x0a12, 0x0001);

    if (app_init()) { printf("init failed\n"); return 1; }

    hci_power_control(HCI_POWER_ON);
    btstack_run_loop_execute();
    return 0;
}
