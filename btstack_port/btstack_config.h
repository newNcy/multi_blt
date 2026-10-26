//
// BTstack 最小配置：只开 Classic（A2DP source 用），不开 BLE/Mesh
//
#ifndef BTSTACK_CONFIG_H
#define BTSTACK_CONFIG_H

#define HAVE_ASSERT
#define HAVE_MALLOC
#define HAVE_POSIX_FILE_IO
#define HAVE_POSIX_TIME

#define ENABLE_CLASSIC
#define ENABLE_LOG_ERROR
#define ENABLE_LOG_INFO
#define ENABLE_PRINTF_HEXDUMP

// 【DualBlt】跳过 AVDTP 建流前的 SDP 查询（CSR 克隆 dongle 会截断 K3 的 SDP 应答，
// 查询永远无法完成）。AVDTP PSM 由规范固定 0x0019，直连即可。详见 avdtp.c 补丁注释。
#define AVDTP_SKIP_SDP_QUERY

// ACL 包大小：SBC 立体声 44.1kHz 一帧 RTP 载荷足够
#define HCI_ACL_PAYLOAD_SIZE 1024
#define HCI_INCOMING_PRE_BUFFER_SIZE 14

#define NVM_NUM_DEVICE_DB_ENTRIES 16
#define NVM_NUM_LINK_KEYS 16

#endif
