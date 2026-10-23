#ifndef UNICODE
#define UNICODE
#endif

// 建立 ACL 连接（不依赖 Windows 蓝牙 API）
// 用法: BtConn.exe [BD_ADDR]      默认 28:2D:7F:D0:12:95 (AirPods)

#include "btcore.h"

#include <cstdio>
#include <string>

static BtHci* g_hci = nullptr;
static u8     g_target[6];
static bool   g_connected = false;
static u16    g_handle = 0;

static void Hex(const std::vector<u8>& v, int maxn = 24)
{
    for (size_t i = 0; i < v.size() && (int)i < maxn; ++i) wprintf(L"%02X ", v[i]);
    if ((int)v.size() > maxn) wprintf(L"...");
}

static void PrintAddr(const u8* a)
{
    wprintf(L"%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
}

// 处理单个 HCI 事件
static void HandleEvent(const std::vector<u8>& ev)
{
    switch (ev[0]) {

    case HCI_EV_CONNECTION_COMPLETE: {
        u8  st     = ev[2];
        u16 handle = (u16)(ev[3] | ((u16)(ev[4] & 0x0F) << 8));
        const u8* a = ev.data() + 5;
        wprintf(L"  [Connection Complete] status=0x%02X handle=0x%04X addr=", st, handle);
        PrintAddr(a);
        wprintf(L" linkType=%d encrypt=%d\n", ev[11], ev[12]);
        if (st == 0x00) { g_connected = true; g_handle = handle; }
        break;
    }

    case HCI_EV_CONNECTION_REQUEST: {
        const u8* a = ev.data() + 2;
        wprintf(L"  [Connection Request] addr=");
        PrintAddr(a);
        wprintf(L" cod=0x%02X%02X%02X linkType=%d -> accept\n", ev[10], ev[9], ev[8], ev[11]);
        // Accept_Connection_Request (0x0409): BD_ADDR(6) + role(1)  0=master 1=slave
        u8 p[7];
        memcpy(p, a, 6);
        p[6] = 0x00;
        g_hci->CmdOk(0x0409, p, 7);
        break;
    }

    case HCI_EV_DISCONNECTION_COMPLETE:
        wprintf(L"  [Disconnection Complete] status=0x%02X handle=0x%04X reason=0x%02X\n",
                ev[2], (u16)(ev[3] | ((u16)(ev[4] & 0x0F) << 8)), ev[5]);
        g_connected = false;
        break;

    case HCI_EV_LINK_KEY_REQUEST: {
        wprintf(L"  [Link Key Request] addr=");
        PrintAddr(ev.data() + 2);
        wprintf(L" -> negative reply (无已存密钥，触发配对)\n");
        // Link_Key_Request_Negative_Reply (0x040C)
        g_hci->CmdOk(0x040C, ev.data() + 2, 6);
        break;
    }

    case HCI_EV_PIN_CODE_REQUEST: {
        wprintf(L"  [PIN Code Request] addr=");
        PrintAddr(ev.data() + 2);
        wprintf(L" -> reply PIN=0000\n");
        // PIN_Code_Request_Reply (0x040D): BD_ADDR(6) + pin_len(1) + pin(16)
        u8 p[23];
        memcpy(p, ev.data() + 2, 6);
        p[6] = 4;
        memset(p + 7, 0, 16);
        p[7] = '0'; p[8] = '0'; p[9] = '0'; p[10] = '0';
        g_hci->CmdOk(0x040D, p, 23);
        break;
    }

    case HCI_EV_IO_CAPABILITY_REQUEST: {
        wprintf(L"  [IO Capability Request] addr=");
        PrintAddr(ev.data() + 2);
        wprintf(L" -> reply NoInputNoOutput\n");
        // IO_Capability_Request_Reply (0x042B): BD_ADDR(6)+io_cap(1)+oob(1)+auth_req(1)
        u8 p[9];
        memcpy(p, ev.data() + 2, 6);
        p[6] = 0x03;   // NoInputNoOutput
        p[7] = 0x00;   // OOB Data Present = none
        p[8] = 0x00;   // MITM not required, no bonding
        g_hci->CmdOk(0x042B, p, 9);
        break;
    }

    case HCI_EV_IO_CAPABILITY_RESPONSE:
        wprintf(L"  [IO Capability Response] ioCap=0x%02X authReq=0x%02X\n",
                ev[8], ev[10]);
        break;

    case HCI_EV_USER_CONFIRMATION_REQ: {
        unsigned int v = ((unsigned int)ev[8] << 24) | ((unsigned int)ev[9] << 16) |
                         ((unsigned int)ev[10] << 8) | ev[11];
        wprintf(L"  [User Confirmation Request] value=%06u -> auto accept\n", v);
        // User_Confirmation_Request_Reply (0x042C): BD_ADDR(6)
        g_hci->CmdOk(0x042C, ev.data() + 2, 6);
        break;
    }

    case HCI_EV_SIMPLE_PAIRING_COMPLETE:
        wprintf(L"  [Simple Pairing Complete] status=0x%02X addr=", ev[2]);
        PrintAddr(ev.data() + 3);
        wprintf(L"\n");
        break;

    case HCI_EV_LINK_KEY_NOTIFICATION: {
        wprintf(L"  [Link Key Notification] addr=");
        PrintAddr(ev.data() + 2);
        wprintf(L" key=");
        for (int i = 8; i < 24; ++i) wprintf(L"%02X", ev[i]);
        wprintf(L" type=%d\n", ev[24]);
        break;
    }

    case HCI_EV_NUM_COMPLETED_PACKETS:
        break;

    case HCI_EV_CMD_STATUS:
        if (ev[2] != 0x00)
            wprintf(L"  [Command Status] status=0x%02X opcode=0x%02X%02X\n", ev[2], ev[5], ev[4]);
        break;

    default:
        wprintf(L"  [ev 0x%02X] ", ev[0]);
        Hex(ev, 16); wprintf(L"\n");
        break;
    }
}

int wmain(int argc, wchar_t** argv)
{
    std::wstring target = (argc > 1) ? argv[1] : L"28:2D:7F:D0:12:95";
    if (!BtAddrFromStr(target, g_target)) {
        wprintf(L"invalid address: %s\n", target.c_str());
        return 1;
    }

    BtHci hci;
    g_hci = &hci;

    wprintf(L"opening dongle...\n");
    if (!hci.Open()) { wprintf(L"open failed\n"); return 1; }

    wprintf(L"HCI init...\n");
    if (!hci.Init()) { wprintf(L"init failed\n"); return 1; }
    wprintf(L"init OK (ACL mtu=%d)\n\n", hci.AclMtu());

    wprintf(L"target: %s\n\n", BtAddrToStr(g_target).c_str());

    // ---- HCI_Create_Connection (0x0405) ----
    // BD_ADDR(6) + Packet_Type(2) + PSRM(1) + Reserved(1) + ClockOffset(2) + AllowRoleSwitch(1)
    u8 p[13];
    memcpy(p, g_target, 6);
    p[6] = 0x18;   // Packet_Type = 0xCC18 (DM1 DH1 DM3 DH3 DM5 DH5)
    p[7] = 0xCC;
    p[8] = 0x01;   // Page_Scan_Repetition_Mode = R1
    p[9] = 0x00;   // Reserved
    p[10] = 0x00;  // Clock_Offset (invalid)
    p[11] = 0x00;
    p[12] = 0x01;  // Allow_Role_Switch = yes

    // R2 是规范要求所有设备必须支持的模式，比 R1 更容易 page 到
    p[8] = 0x02;

    for (int attempt = 1; attempt <= 3 && !g_connected; ++attempt) {
        wprintf(L"Create_Connection (attempt %d, PSRM=R2)...\n", attempt);
        if (!hci.Cmd(0x0405, p, 13)) { wprintf(L"send failed\n"); return 1; }

        int waited = 0;
        const int BUDGET = 15000;   // 每次 15 秒
        std::vector<u8> ev;
        while (waited < BUDGET) {
            ev.clear();
            if (hci.NextEvent(ev, 400)) {
                HandleEvent(ev);
                if (g_connected) break;
            } else {
                waited += 400;
            }
        }
        if (!g_connected) {
            // 取消未完成的连接尝试
            u8 cp[6];
            memcpy(cp, g_target, 6);
            hci.Cmd(0x0408, cp, 6);   // Create_Connection_Cancel
            Sleep(500);
        }
    }

    if (g_connected) {
        wprintf(L"\n*** ACL link established, handle=0x%04X ***\n", g_handle);

        // 观察 3 秒，看看对端有没有主动发 L2CAP 过来
        wprintf(L"\nlistening for ACL traffic (3s)...\n");
        std::vector<u8> pkt;
        int t = 0;
        while (t < 3000) {
            if (hci.AclRecv(pkt, 300)) {
                u16 h = (u16)(pkt[0] | ((u16)(pkt[1] & 0x0F) << 8));
                int len = pkt[2] | ((int)pkt[3] << 8);
                wprintf(L"  ACL handle=0x%04X len=%d data:", h, len);
                for (int i = 0; i < len && i < 24; ++i) wprintf(L" %02X", pkt[4 + i]);
                wprintf(L"\n");
            }
            t += 300;
        }

        wprintf(L"\ndisconnecting...\n");
        u8 dp[3];
        dp[0] = (u8)(g_handle & 0xFF);
        dp[1] = (u8)((g_handle >> 8) & 0x0F);
        dp[2] = 0x13;   // Reason = Remote User Ended Connection
        hci.Cmd(0x0406, dp, 3);
        for (int i = 0; i < 12; ++i) {
            std::vector<u8> e2;
            if (hci.NextEvent(e2, 400)) HandleEvent(e2);
        }
    } else {
        wprintf(L"\n*** connection failed (timeout) ***\n");
    }

    hci.Close();
    return 0;
}
