// btsnoop 解析器：提取 L2CAP 信令交互（连接请求/响应、配置、断开）
// 用法: BtSnoopParse.exe dualblt.snoop
#ifndef UNICODE
#define UNICODE
#endif
#include <cstdio>
#include <cstdint>
#include <vector>
#include <string>

static uint32_t Rd32(const uint8_t* p) { return (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3]; }
static uint64_t Rd64(const uint8_t* p) { uint64_t v=0; for(int i=0;i<8;i++) v=(v<<8)|p[i]; return v; }

static std::string L2capCodeName(uint8_t c)
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

int main(int argc, char** argv)
{
    if (argc < 2) { printf("usage: BtSnoopParse <file>\n"); return 1; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { printf("cannot open\n"); return 1; }

    uint8_t h[16];
    if (fread(h, 1, 16, f) != 16) return 1;
    if (memcmp(h, "btsnoop\0", 8) != 0) { printf("not btsnoop\n"); return 1; }
    uint32_t datalink = Rd32(h + 12);
    printf("datalink=%u\n", datalink);

    long rec = 0;
    for (;;) {
        uint8_t rh[24];
        size_t n = fread(rh, 1, 24, f);
        if (n < 24) break;
        uint32_t orig = Rd32(rh), incl = Rd32(rh + 4), flags = Rd32(rh + 8);
        std::vector<uint8_t> d(incl);
        if (fread(d.data(), 1, incl, f) != incl) break;
        ++rec;

        if (d.empty()) continue;
        uint8_t type = d[0];

        // 只要 ACL 包里的 L2CAP 信令（CID 0x0001）与 HFP 相关的
        if (type == 0x02 && d.size() >= 9) {
            // H4 ACL: handle(2 LE) + data_len(2 LE) + L2CAP len(2 LE) + cid(2 LE)
            uint16_t cid = d[6] | (d[7] << 8);
            if (cid == 0x0001 && d.size() >= 12) {
                uint16_t ll = d[4] | (d[5] << 8);
                uint8_t code = d[8];
                uint8_t id = d[9];
                uint16_t plen = d[10] | (d[11] << 8);
                printf("[%06ld] %s L2CAP code=0x%02X(%s) id=0x%02X len=%u  data:",
                       rec, (flags & 1) ? "TX" : "RX", code, L2capCodeName(code).c_str(), id, plen);
                for (size_t i = 12; i < d.size() && i < 12 + 16; ++i) printf(" %02X", d[i]);
                printf("\n");
                // ConnRsp/DisconnRsp 的关键字段
                if (code == 0x02 && d.size() >= 20) {
                    uint16_t dcid = d[12] | (d[13] << 8);
                    uint16_t scid = d[14] | (d[15] << 8);
                    uint16_t res  = d[16] | (d[17] << 8);
                    uint16_t st   = d[18] | (d[19] << 8);
                    printf("        ConnRsp: destCID=0x%04X srcCID=0x%04X result=0x%04X status=0x%04X\n",
                           dcid, scid, res, st);
                }
            }
        }
        if (type == 0x04 && d.size() >= 3) {
            // 关键 HCI 事件：连接完成 / 断开 / 加密变更
            uint8_t code = d[1];
            if (code == 0x03 || code == 0x05 || code == 0x06 || code == 0x08 || code == 0x17) {
                printf("[%06ld] %s HCI event 0x%02X:", rec, (flags & 1) ? "TX" : "RX", code);
                for (size_t i = 2; i < d.size() && i < 14; ++i) printf(" %02X", d[i]);
                printf("\n");
            }
        }
    }
    printf("total records=%ld\n", rec);
    fclose(f);
    return 0;
}
