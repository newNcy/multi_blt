#ifndef UNICODE
#define UNICODE
#endif

// 诊断：扫描 + Remote_Name_Request（verbose 转储所有事件）

#include "btcore.h"
#include <cstdio>

int wmain(int argc, wchar_t** argv)
{
    BtHci hci;
    if (!hci.Open()) { wprintf(L"WinUSB 适配器打开失败\n"); return 1; }

    // 用法: BtTest.exe [BD_ADDR]  -> 只对指定地址做名字请求
    if (argc > 1) {
        u8 addr[6];
        if (!BtAddrFromStr(argv[1], addr)) { wprintf(L"bad addr\n"); return 1; }
        if (!hci.ScanBegin()) { wprintf(L"init failed\n"); return 1; }
        wprintf(L"目标 %s\n", BtAddrToStr(addr).c_str());

        // 0) 先扫 5 秒确认目标在场
        std::vector<BtHci::ScanDev> seen;
        hci.InquiryRound(seen, 5, false);
        bool present = false;
        u8 psrm = 0x02;
        u16 clk = 0;
        for (auto& s : seen) {
            if (memcmp(s.addr, addr, 6) == 0) { present = true; psrm = s.psrm; clk = s.clk; break; }
        }
        if (!present) {
            wprintf(L"  目标当前不可发现（未进入配对模式或已休眠），先扫描确认再试。\n");
            hci.Close();
            return 2;
        }
        wprintf(L"  目标在场 psrm=%d clk=0x%04X\n", psrm, clk);

        // 1) 先建 ACL 链路
        u16 handle = 0xFFFF;
        wprintf(L"  ConnectAcl...\n");
        if (!hci.ConnectAcl(addr, handle)) {
            wprintf(L"  连接失败\n");
            hci.Close();
            return 1;
        }
        wprintf(L"  已连接 handle=0x%04X\n", handle);

        // 2) 在已有链路上取名字
        hci.SetVerbose(true);
        std::string nm;
        bool ok = hci.RemoteName(addr, psrm, clk, nm);
        hci.SetVerbose(false);
        wprintf(L"  RemoteName -> %d, name=%S\n", (int)ok,
                std::wstring(nm.begin(), nm.end()).c_str());

        // 3) 断开
        hci.DisconnectAcl(handle);
        wprintf(L"  已断开\n");
        hci.Close();
        return 0;
    }

    u8 addr[6] = {};
    if (!hci.ReadBdAddr(addr)) { wprintf(L"ReadBdAddr FAIL\n"); return 1; }
    wprintf(L"ReadBdAddr OK : %s\n", BtAddrToStr(addr).c_str());
    hci.Close();

    BtHci hci2;
    if (!hci2.Open()) { wprintf(L"reopen failed\n"); return 1; }
    std::vector<BtHci::ScanDev> found;
    std::wstring err;
    if (!hci2.Scan(found, 8, err)) { wprintf(L"Scan FAIL: %s\n", err.c_str()); return 1; }
    wprintf(L"Scan OK : %u 个设备\n", (unsigned)found.size());
    for (auto& d : found) {
        wprintf(L"  %s  psrm=%d clk=0x%04X  name=%S\n",
                BtAddrToStr(d.addr).c_str(), d.psrm, d.clk,
                d.name.empty() ? L"(空)" : std::wstring(d.name.begin(), d.name.end()).c_str());
    }

    // 对第一个没有名字的设备做名字请求，转储全部事件
    for (auto& d : found) {
        if (!d.name.empty()) continue;
        wprintf(L"\n--- Remote_Name_Request 调试 (%s, psrm=%d, clk=0x%04X) ---\n",
                BtAddrToStr(d.addr).c_str(), d.psrm, d.clk);
        hci2.SetVerbose(true);
        std::string nm;
        bool ok = hci2.RemoteName(d.addr, d.psrm, d.clk, nm);
        hci2.SetVerbose(false);
        wprintf(L"  -> result=%d name=%S\n", (int)ok,
                std::wstring(nm.begin(), nm.end()).c_str());
        break;
    }

    hci2.Close();
    return 0;
}
