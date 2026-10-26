// btsnoop 解析器（Linux Monitor 格式，datalink=2001）— 全量输出版
// 记录数据 = 4 字节大端 opcode + HCI 包（无 H4 类型字节）
//   0x0002=命令TX  0x0003=事件RX  0x0004=ACL TX  0x0005=ACL RX
// 用法: BtSnoopParse [-full] dualblt.snoop
#ifndef UNICODE
#define UNICODE
#endif
#include <cstdio>
#include <cstdint>
#include <vector>
#include <string>

static uint32_t Rd32(const uint8_t* p) { return (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3]; }
static unsigned long long Rd64(const uint8_t* p) { unsigned long long v=0; for(int i=0;i<8;i++) v=(v<<8)|p[i]; return v; }

static bool IsOpcode(uint32_t op) {
    return op == 0x02 || op == 0x03 || op == 0x04 || op == 0x05 || op == 0x0C;
}

static const char* HciEventName(uint8_t e) {
    switch (e) {
    case 0x01: return "InquiryCmp";
    case 0x02: return "InquiryResult";
    case 0x03: return "ConnComplete";
    case 0x04: return "ConnRequest";
    case 0x05: return "DisconnComplete";
    case 0x06: return "AuthComplete";
    case 0x07: return "RemoteNameReqCmp";
    case 0x08: return "EncryptChange";
    case 0x0b: return "RoleChange";
    case 0x0d: return "MaxSlotsChange";
    case 0x0e: return "CmdComplete";
    case 0x0f: return "CmdStatus";
    case 0x12: return "RoleChange2";
    case 0x13: return "NumCompletedPkts";
    case 0x14: return "ModeChange";
    case 0x17: return "QoSViolation";
    case 0x1b: return "MaxSlotsChange2";
    case 0xff: return "VendorMeta";
    default:   return "";
    }
}

static const char* HciCmdName(uint16_t ogf, uint16_t ocf) {
    if (ogf == 1) { // Link control
        switch (ocf) {
        case 0x0001: return "Inquiry";
        case 0x0002: return "InquiryCancel";
        case 0x0005: return "CreateConnection";
        case 0x0008: return "CreateConnCancel";
        case 0x0009: return "AcceptConnReq";
        case 0x000a: return "RejectConnReq";
        case 0x000b: return "LinkKeyReqReply";
        case 0x000c: return "LinkKeyReqNegReply";
        case 0x0011: return "PinCodeReqReply";
        case 0x0012: return "PinCodeReqNegReply";
        case 0x0013: return "AuthRequested";
        case 0x0019: return "RemoteNameReq";
        case 0x001b: return "SetConnEncryption";
        case 0x003b: return "SwitchRole";
        case 0x0045: return "IOCapReqReply";
        case 0x004c: return "RemoteIOCapReqReply";
        default:     return "";
        }
    }
    if (ogf == 2) { // Link policy
        switch (ocf) {
        case 0x0001: return "HoldMode";
        case 0x0003: return "SniffMode";
        case 0x0004: return "ExitSniff";
        case 0x0005: return "ParkState";
        case 0x0006: return "ExitPark";
        case 0x0008: return "WriteLinkPolicy";
        case 0x000d: return "SniffSubrating";
        default:     return "";
        }
    }
    if (ogf == 3) { // Host controller
        switch (ocf) {
        case 0x0003: return "Reset";
        case 0x0016: return "WriteLocalName";
        case 0x0018: return "WritePageTimeout";
        case 0x001a: return "WriteScanEnable";
        case 0x0024: return "WriteClassOfDevice";
        case 0x0033: return "SetHostFlowControl";
        case 0x0056: return "WriteSimplePairingMode";
        case 0x006b: return "WriteInquiryTxPower";
        case 0x006d: return "WriteDefaultErrDataRpt";
        default:     return "";
        }
    }
    if (ogf == 4) return "InfoParam";
    return "";
}

static const char* L2capCodeName(uint8_t c)
{
    switch (c) {
    case 0x01: return "ConnReq";
    case 0x02: return "ConnRsp";
    case 0x03: return "CfgReq";
    case 0x04: return "CfgRsp";
    case 0x05: return "DisconnReq";
    case 0x06: return "DisconnRsp";
    case 0x07: return "EchoReq";
    case 0x08: return "EchoRsp";
    case 0x0A: return "InfoReq";
    case 0x0B: return "InfoRsp";
    default:   return "other";
    }
}

static const char* PsmName(uint16_t psm) {
    switch (psm) {
    case 0x0001: return "SDP";
    case 0x0003: return "RFCOMM";
    case 0x0019: return "AVDTP";
    case 0x001b: return "AVCTP";
    default:     return "";
    }
}

static void Hex(const uint8_t* d, size_t n, size_t maxn)
{
    for (size_t i = 0; i < n && i < maxn; ++i) printf(" %02X", d[i]);
    if (n > maxn) printf(" ...(%u)", (unsigned)n);
}

int main(int argc, char** argv)
{
    const char* path = nullptr;
    bool full = false;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-full")) full = true;
        else path = argv[i];
    }
    if (!path) { printf("usage: BtSnoopParse [-full] <file>\n"); return 1; }
    FILE* f = fopen(path, "rb");
    if (!f) { printf("cannot open\n"); return 1; }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 16, SEEK_SET);          // 跳过 btsnoop 全局头
    std::vector<uint8_t> buf(fsize > 16 ? fsize - 16 : 0);
    if (!buf.empty()) {
        size_t got = fread(buf.data(), 1, buf.size(), f);
        buf.resize(got);
    }
    fclose(f);

    long rec = 0, resync = 0;
    long long mediaTx = 0, mediaRx = 0;
    unsigned long long ts0 = 0;
    size_t pos = 0;
    while (pos + 20 <= buf.size()) {
        // 自适应记录解析：hdr=24(orig,incl,flags,drops,ts8) 或 hdr=16(orig,incl,flags,ts8)
        bool ok = false;
        uint32_t incl = 0, opcode = 0;
        unsigned long long ts = 0;
        size_t dataOff = 0;
        for (int variant = 0; variant < 2 && !ok; ++variant) {
            size_t hdrLen = (variant == 0) ? 24 : 16;
            if (pos + hdrLen > buf.size()) break;
            uint32_t i0 = Rd32(&buf[pos + 4]);
            uint32_t op = Rd32(&buf[pos + 8]);
            if (!IsOpcode(op)) continue;
            if (i0 == 0 || i0 > 2000 || pos + hdrLen + i0 > buf.size()) continue;
            size_t next = pos + hdrLen + i0;
            if (next + 12 <= buf.size()) {
                uint32_t op2 = Rd32(&buf[next + 8]);
                if (!IsOpcode(op2)) continue;
            }
            ok = true;
            incl = i0; opcode = op;
            dataOff = pos + hdrLen;
            ts = Rd64(&buf[pos + hdrLen - 8]);
        }
        if (!ok) { ++resync; ++pos; continue; }
        std::vector<uint8_t> d(buf.begin() + dataOff, buf.begin() + dataOff + incl);
        if (ts0 == 0) ts0 = ts;
        ++rec;
        double tsec = (double)(ts - ts0) / 10000.0;   // btsnoop 时间单位 0.1ms
        if (tsec < 0) tsec = 0;
        size_t nextPos = dataOff + incl;
        pos = nextPos;

        // ---- btstack 内部日志（opcode 0x0C）----
        if (opcode == 0x000C && !d.empty()) {
            const uint8_t* p = d.data();
            size_t n = d.size();
            // 可能带 1 字节类型前缀，打印可打印文本部分
            printf("[%08.2f][%06ld] LOG ", tsec, rec);
            size_t start = 0;
            if (n > 1 && (p[0] == 1 || p[0] == 2 || p[0] == 3)) { start = 1; }
            for (size_t i = start; i < n && i < start + 240; ++i) {
                uint8_t c = p[i];
                putchar((c >= 0x20 && c < 0x7F) ? c : (c == '\n' ? '\n' : '.'));
            }
            printf("\n");
            continue;
        }
        // ---- HCI 命令（TX）----
        if (opcode == 0x0002) {
            if (d.size() < 3) continue;
            uint16_t ogf = (d[1] >> 4) & 0xF, ocf = d[0] | ((d[1] & 0xF) << 8);
            printf("[%08.2f][%06ld] TX CMD %s (ogf=0x%X ocf=0x%04X)%s",
                   tsec, rec, HciCmdName(ogf, ocf), ogf, ocf,
                   HciCmdName(ogf, ocf)[0] ? "" : " ");
            Hex(d.data() + 3, d.size() - 3, full ? 64 : 16);
            printf("\n");
            continue;
        }
        // ---- HCI 事件（RX）----
        if (opcode == 0x0003 && d.size() >= 1) {
            uint8_t code = d[0];
            // 过滤高频噪音：命令完成/状态与数据包计数保留，其余全打
            printf("[%08.2f][%06ld] RX EVT %s (0x%02X)%s",
                   tsec, rec, HciEventName(code), code,
                   HciEventName(code)[0] ? "" : " ");
            Hex(d.data() + 1, d.size() - 1, full ? 64 : 16);
            printf("\n");
            continue;
        }
        // ---- ACL 数据 ----
        if (opcode == 0x0004 || opcode == 0x0005) {
            if (d.size() < 8) continue;
            uint16_t cid = d[6] | (d[7] << 8);
            bool tx = (opcode == 0x0004);
            if (tx) mediaTx++; else mediaRx++;
            if (cid == 0x0001 && d.size() >= 12) {
                // L2CAP 信令
                uint8_t code = d[8], id = d[9];
                uint16_t plen = d[10] | (d[11] << 8);
                printf("[%08.2f][%06ld] %s L2SIG %s id=0x%02X len=%u data:",
                       tsec, rec, tx ? "TX" : "RX", L2capCodeName(code), id, plen);
                Hex(d.data() + 12, d.size() - 12, full ? 64 : 16);
                printf("\n");
            } else {
                printf("[%08.2f][%06ld] %s L2DATA cid=0x%04X len=%u data:",
                       tsec, rec, tx ? "TX" : "RX", cid, (unsigned)(d.size() - 8));
                Hex(d.data() + 8, d.size() - 8, full ? 48 : 20);
                printf("\n");
            }
            continue;
        }
    }
    printf("total records=%ld  resync=%ld\n", rec, resync);
    printf("media 概览: TX=%lld RX=%lld\n", mediaTx, mediaRx);
    return 0;
}
