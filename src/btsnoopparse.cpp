// btsnoop 解析器（Linux Monitor 格式，datalink=2001）
// 记录数据 = 4 字节大端 opcode + HCI 包（无 H4 类型字节）
//   0x0002=命令TX  0x0003=事件RX  0x0004=ACL TX  0x0005=ACL RX
// 用法: BtSnoopParse.exe dualblt.snoop
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

struct CidStat { long long tx = 0, rx = 0; long long bytes = 0; };
static std::vector<std::pair<uint16_t, CidStat>> cidOrder;
static CidStat& Cid(uint16_t cid) {
    for (auto& kv : cidOrder) if (kv.first == cid) return kv.second;
    cidOrder.push_back({cid, CidStat()});
    return cidOrder.back().second;
}

int main(int argc, char** argv)
{
    if (argc < 2) { printf("usage: BtSnoopParse <file>\n"); return 1; }
    FILE* f = fopen(argv[1], "rb");
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
        size_t nextPos = dataOff + incl;   // 记录结束位置（立即推进，处理代码里的 continue 才安全）
        pos = nextPos;

        // ACL 数据（opcode 区分收发）
        if (opcode == 0x0004 || opcode == 0x0005) {
            if (d.size() < 8) continue;
            // ACL: handle(2 LE) datalen(2 LE) l2cap_len(2 LE) cid(2 LE) payload
            uint16_t cid = d[6] | (d[7] << 8);
            bool tx = (opcode == 0x0004);
            CidStat& s = Cid(cid);
            if (tx) { s.tx++; s.bytes += d.size() - 8; } else { s.rx++; s.bytes += d.size() - 8; }
            if (cid == 0x0001) {
                // L2CAP 信令
                if (d.size() >= 12) {
                    uint8_t code = d[8], id = d[9];
                    uint16_t plen = d[10] | (d[11] << 8);
                    printf("[%08.2f][%06ld] %s L2CAP code=0x%02X(%s) id=0x%02X len=%u data:",
                           tsec, rec, tx ? "TX" : "RX", code, L2capCodeName(code).c_str(), id, plen);
                    for (size_t i = 12; i < d.size() && i < 12 + 16; ++i) printf(" %02X", d[i]);
                    printf("\n");
                    if (code == 0x02 && d.size() >= 20) {
                        uint16_t dcid = d[12] | (d[13] << 8);
                        uint16_t scid = d[14] | (d[15] << 8);
                        uint16_t res  = d[16] | (d[17] << 8);
                        printf("        ConnRsp: destCID=0x%04X srcCID=0x%04X result=0x%04X\n", dcid, scid, res);
                    }
                }
            } else {
                if (tx) mediaTx++; else mediaRx++;
                // 对端发来的非信令包：采样前 8 条（AVRCP 暂停/音量、AVDTP 命令都在这）
                if (!tx && Cid(cid).rx <= 8) {
                    printf("[%08.2f][%06ld] RX cid=0x%04X len=%u data:", tsec, rec, cid, (unsigned)(d.size() - 8));
                    for (size_t i = 8; i < d.size() && i < 8 + 20; ++i) printf(" %02X", d[i]);
                    printf("\n");
                }
            }
            continue;
        }

        // HCI 事件（RX）：d[0]=事件码
        if (opcode == 0x0003 && d.size() >= 1) {
            uint8_t code = d[0];
            if (code == 0x03 || code == 0x05 || code == 0x06 || code == 0x08
                || code == 0x14 || code == 0x17 || code == 0x1B) {
                printf("[%08.2f][%06ld] RX HCI event 0x%02X:", tsec, rec, code);
                for (size_t i = 1; i < d.size() && i < 1 + 12; ++i) printf(" %02X", d[i]);
                printf("\n");
            }
            pos = nextPos;
            continue;
        }
        pos = nextPos;
    }
    printf("total records=%ld  resync=%ld\n", rec, resync);
    printf("media 概览: TX=%lld RX=%lld\n", mediaTx, mediaRx);
    printf("--- per-CID 统计 (tx=我方发送 rx=对端发来) ---\n");
    for (auto& kv : cidOrder)
        printf("cid=0x%04X  tx=%-8lld rx=%-8lld bytes=%lld\n", kv.first, kv.second.tx, kv.second.rx, kv.second.bytes);
    return 0;
}
