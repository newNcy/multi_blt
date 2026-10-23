// BTstack 启动验证：用我们的 WinUSB dongle 把 HCI 拉起来
#include <Windows.h>
#include <stdint.h>
#include <stdio.h>

#include "btstack_config.h"
#include "btstack_event.h"
#include "btstack_memory.h"
#include "btstack_run_loop.h"
#include "btstack_run_loop_windows.h"
#include "hci.h"
#include "hci_dump.h"
#include "hci_dump_windows_stdout.h"
#include "hci_transport.h"
#include "hci_transport_usb.h"

static btstack_packet_callback_registration_t hci_event_callback_registration;

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    if (packet_type != HCI_EVENT_PACKET) return;
    uint8_t evt = hci_event_packet_get_type(packet);

    switch (evt) {
    case BTSTACK_EVENT_STATE:
        switch (btstack_event_state_get_state(packet)) {
        case HCI_STATE_WORKING: {
            bd_addr_t a;
            gap_local_bd_addr(a);
            printf("=== BTstack UP, BD_ADDR=%s ===\n", bd_addr_to_str(a));
            break;
        }
        case HCI_STATE_OFF:
            printf("=== HCI OFF ===\n");
            exit(0);
            break;
        default:
            break;
        }
        break;

    case HCI_EVENT_COMMAND_COMPLETE:
        printf("[cmd complete] %s\n", hci_event_command_complete_get_command_opcode(packet) == 0x1009
               ? "read bd addr" : "");
        break;

    case HCI_EVENT_INQUIRY_RESULT:
    case HCI_EVENT_INQUIRY_RESULT_WITH_RSSI:
        printf("[inquiry result]\n");
        break;

    default:
        break;
    }
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("BTstack booting (custom WinUSB dongle)...\n");

    btstack_memory_init();
    btstack_run_loop_init(btstack_run_loop_windows_get_instance());

    // 日志走 stdout，方便看进度
    hci_dump_init(hci_dump_windows_stdout_get_instance());

    // HCI 传输：BTstack 的 WinUSB 实现
    hci_init(hci_transport_usb_instance(), NULL);

    // 只认我们这个 dongle 的 VID/PID，避免它去打开其它 USB 设备
    hci_transport_usb_add_device(0x0a12, 0x0001);

    hci_event_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    printf("power on...\n");
    hci_power_control(HCI_POWER_ON);

    btstack_run_loop_execute();
    return 0;
}
