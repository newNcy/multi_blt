#ifndef UNICODE
#define UNICODE
#endif

// AVDTP 信令探测：确定本耳机使用的 Signal Identifier 字节序约定
// 用法: BtAvdtp.exe [BD_ADDR]

#include "btcore.h"

#include <cstdio>
#include <string>

static const u16 PSM_AVDTP  = 0x0019;
static const u16 CID_SIGNAL = 0x0001;
static const u16 OUR_CID    = 0x0040;

static void Put16(std::vector<u8>& v, u16 x)
{
    v.push_back((u8)(x & 0xFF));
    v.push_back((u8)(x >> 8));
}

static std::vector<u8> L2capPdu(u16 cid, const std::vector<u8>& payload)
{
    std::vector<u8> p;
    Put16(p, (u16)payload.size());
    Put16(p, cid);
    p.insert(p.end(), payload.begin(), payload.end());
    return p;
}

static std::vector<u8> L2capSignal(u8 code, u8 id, const std::vector<u8>& data)
{
    std::vector<u8> p;
    p.push_back(code);
    p.push_back(id);
    Put16(p, (u16)data.size());
    p.insert(p.end(), data.begin(), data.end());
    return p;
}

static void Hex(const u8* d, int n)
{
    for (int i = 0; i < n; ++i) wprintf(L"%02X ", d[i]);
}

// ---------------------------------------------------------------- 接收

// 泵 HCI 事件（处理链路事件），ms 毫秒
static void PumpHci(BtHci& hci, int ms)
{
    int waited = 0;
    std::vector<u8> ev;
    while (waited < ms) {
        if (!hci.NextEvent(ev, 30)) { waited += 30; continue; }
        if (ev[0] == HCI_EV_DISCONNECTION_COMPLETE) {
            wprintf(L"  !! 链路已断开 status=0x%02X\n", ev[2]);
            return;
        }
        // 其它事件忽略（配对事件由 ConnectAcl 内部处理过）
    }
}

// 收指定 CID 上的一个 L2CAP 包；顺带应答信令请求
static bool RecvOnCid(BtHci& hci, u16 handle, u16 wantCid,
                      std::vector<u8>& payload, int timeoutMs)
{
    int waited = 0;
    std::vector<u8> pkt;
    while (waited < timeoutMs) {
        if (!hci.AclRecv(pkt, 200)) { waited += 200; PumpHci(hci, 10); continue; }
        if (pkt.size() < 8) continue;

        u16 cid = (u16)(pkt[6] | ((u16)pkt[7] << 8));
        const u8* pl = pkt.data() + 8;
        int plen = (int)pkt.size() - 8;

        if (cid == CID_SIGNAL && plen >= 4) {
            u8 code = pl[0], id = pl[1];
            if (code == 0x04) {                       // 对端 Configuration Request
                std::vector<u8> data;
                Put16(data, OUR_CID);
                Put16(data, 0x0000);
                Put16(data, 0x0000);
                std::vector<u8> sig = L2capSignal(0x05, id, data);
                std::vector<u8> pdu = L2capPdu(CID_SIGNAL, sig);
                hci.AclSend(handle, pdu.data(), (int)pdu.size());
                continue;
            }
            if (code == 0x06) {                       // 对端 Disconnection Request
                std::vector<u8> data;
                Put16(data, (u16)(pl[4] | ((u16)pl[5] << 8)));
                Put16(data, (u16)(pl[6] | ((u16)pl[7] << 8)));
                std::vector<u8> sig = L2capSignal(0x07, id, data);
                std::vector<u8> pdu = L2capPdu(CID_SIGNAL, sig);
                hci.AclSend(handle, pdu.data(), (int)pdu.size());
                continue;
            }
            payload.assign(pl, pl + plen);
            return true;                              // 信令响应
        }
        if (cid == wantCid) {
            payload.assign(pl, pl + plen);
            return true;
        }
        // 其它 CID 忽略
    }
    return false;
}

// ---------------------------------------------------------------- 主流程

int wmain(int argc, wchar_t** argv)
{
    std::wstring target = (argc > 1) ? argv[1] : L"C4:33:96:19:A0:9F";
    u8 addr[6];
    if (!BtAddrFromStr(target, addr)) { wprintf(L"invalid address\n"); return 1; }

    BtHci hci;
    if (!hci.Open() || !hci.ScanBegin()) { wprintf(L"open/init failed\n"); return 1; }

    wprintf(L"target %s\n", BtAddrToStr(addr).c_str());

    // 预扫描确认在场
    std::vector<BtHci::ScanDev> seen;
    hci.InquiryRound(seen, 5, false);
    bool present = false;
    for (auto& s : seen) if (memcmp(s.addr, addr, 6) == 0) { present = true; break; }
    if (!present) { wprintf(L"  target NOT discoverable - put it into pairing mode\n"); return 2; }

    // 建链
    u16 handle = 0xFFFF;
    wprintf(L"ACL connect...\n");
    if (!hci.ConnectAcl(addr, handle)) { wprintf(L"  connect FAILED\n"); return 1; }
    wprintf(L"  handle=0x%04X\n", handle);

    // L2CAP 连接 AVDTP
    u16 dcid = 0;
    {
        std::vector<u8> data;
        Put16(data, PSM_AVDTP);
        Put16(data, OUR_CID);
        std::vector<u8> pdu = L2capPdu(CID_SIGNAL, L2capSignal(0x02, 0x01, data));
        if (!hci.AclSend(handle, pdu.data(), (int)pdu.size())) { wprintf(L"  send failed\n"); return 1; }

        std::vector<u8> resp;
        if (!RecvOnCid(hci, handle, CID_SIGNAL, resp, 4000) || resp.size() < 8 || resp[0] != 0x03) {
            wprintf(L"  no L2CAP conn response\n");
            return 1;
        }
        dcid = (u16)(resp[4] | ((u16)resp[5] << 8));
        u16 result = (u16)(resp[8] | ((u16)resp[9] << 8));
        wprintf(L"  L2CAP connected DCID=0x%04X result=0x%04X\n", dcid, result);
        if (result != 0) return 1;
    }

    // L2CAP 配置
    {
        std::vector<u8> opt;
        opt.push_back(0x01); opt.push_back(0x02);
        opt.push_back(0xA0); opt.push_back(0x02);      // MTU 672
        std::vector<u8> data;
        Put16(data, dcid);
        Put16(data, 0x0000);
        data.insert(data.end(), opt.begin(), opt.end());
        std::vector<u8> pdu = L2capPdu(CID_SIGNAL, L2capSignal(0x04, 0x02, data));
        hci.AclSend(handle, pdu.data(), (int)pdu.size());

        // 等自己的配置响应（期间顺带应答对端的配置请求）
        for (int i = 0; i < 20; ++i) {
            std::vector<u8> resp;
            if (!RecvOnCid(hci, handle, CID_SIGNAL, resp, 300)) continue;
            if (resp[0] == 0x05) {
                u16 result = (u16)(resp[8] | ((u16)resp[9] << 8));
                wprintf(L"  L2CAP configured result=0x%04X\n", result);
                break;
            }
        }
    }

    // ---- AVDTP Discover 编码探测 ----
    // 标准约定: byte1 = sigId << 2  →  DISCOVER(0x01) = 0x04
    // 备选约定: byte1 = sigId       →  DISCOVER(0x01) = 0x01
    struct Var { const wchar_t* name; u8 b0, b1; };
    Var vars[] = {
        { L"00 04  (sigId 在高6位, label=0)", 0x00, 0x04 },
        { L"00 01  (sigId 在低6位, label=0)", 0x00, 0x01 },
        { L"10 04  (sigId 在高6位, label=1)", 0x10, 0x04 },
        { L"10 01  (sigId 在低6位, label=1)", 0x10, 0x01 },
    };

    wprintf(L"\n--- AVDTP Discover 编码探测 ---\n");
    bool gotSep = false;
    for (auto& v : vars) {
        if (gotSep) break;
        u8 cmd[2] = { v.b0, v.b1 };
        std::vector<u8> pdu = L2capPdu(dcid, std::vector<u8>(cmd, cmd + 2));
        wprintf(L"发送 %s : ", v.name);
        Hex(cmd, 2);
        wprintf(L" -> %s\n", hci.AclSend(handle, pdu.data(), (int)pdu.size()) ? L"ok" : L"fail");

        std::vector<u8> resp;
        if (!RecvOnCid(hci, handle, OUR_CID, resp, 1500)) {
            wprintf(L"  no response\n");
            continue;
        }
        wprintf(L"  响应 len=%d : ", (int)resp.size());
        Hex(resp.data(), (int)resp.size() < 24 ? (int)resp.size() : 24);
        wprintf(L"\n");

        if (resp.size() < 2) continue;
        u8 msgType = resp[0] & 0x03;
        u8 sigHi = (u8)((resp[1] >> 2) & 0x3F);
        u8 sigLo = (u8)(resp[1] & 0x3F);
        wprintf(L"  msgType=%d  sigId(高6位解释)=%d  sigId(低6位解释)=%d\n",
                msgType, sigHi, sigLo);

        if (msgType == 0x02) {   // Response Accept
            wprintf(L"  *** ACCEPTED! convention: %s ***\n", v.name);
            gotSep = true;
            wprintf(L"  SEP data: ");
            Hex(resp.data() + 2, (int)resp.size() - 2);
            wprintf(L"\n");
        } else {
            wprintf(L"  rejected, error=0x%02X\n", resp.size() > 2 ? resp[2] : 0);
        }
    }

    if (!gotSep) wprintf(L"\n  all variants rejected - link may need encryption first\n");

    hci.DisconnectAcl(handle);
    hci.Close();
    return 0;
}
