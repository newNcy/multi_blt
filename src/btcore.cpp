#include "btcore.h"

#include <initguid.h>
#include <setupapi.h>
#include <usbspec.h>
#include <winusb.h>

#include <cstdio>
#include <cstring>

#pragma comment(lib, "winusb.lib")
#pragma comment(lib, "setupapi.lib")

// 与 DualBltWinUSB.inf 中的 DeviceInterfaceGUIDs 一致
DEFINE_GUID(GUID_DEVIF_DUALBT_WINUSB,
    0x2F5A1E1B, 0x9C4E, 0x4E27, 0x8A, 0x0D, 0x6B, 0x5C, 0x1D, 0x3E, 0x7A, 0x01);

// ---------------------------------------------------------------- 工具

bool BtAddrFromStr(const std::wstring& s, u8 out[6])
{
    int v[6];
    if (swscanf_s(s.c_str(), L"%x:%x:%x:%x:%x:%x",
                  &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
        // 也接受 12 位连续十六进制
        if (s.size() != 12) return false;
        for (int i = 0; i < 6; ++i) {
            std::wstring two = s.substr(i * 2, 2);
            if (swscanf_s(two.c_str(), L"%x", &v[i]) != 1) return false;
        }
    }
    // 字符串是大端顺序，HCI 里是小端
    for (int i = 0; i < 6; ++i) out[i] = (u8)v[5 - i];
    return true;
}

std::wstring BtAddrToStr(const u8 a[6])
{
    wchar_t buf[32];
    swprintf_s(buf, L"%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
    return std::wstring(buf);
}

// ---------------------------------------------------------------- 打开

BtHci::BtHci() {}
BtHci::~BtHci() { Close(); }

static bool FindWinUsbPath(std::wstring& outPath)
{
    HDEVINFO di = SetupDiGetClassDevsW(&GUID_DEVIF_DUALBT_WINUSB, nullptr, nullptr,
                                       DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (di == INVALID_HANDLE_VALUE) return false;
    SP_DEVICE_INTERFACE_DATA ifd = {};
    ifd.cbSize = sizeof(ifd);
    if (!SetupDiEnumDeviceInterfaces(di, nullptr, &GUID_DEVIF_DUALBT_WINUSB, 0, &ifd)) {
        SetupDiDestroyDeviceInfoList(di);
        return false;
    }
    DWORD need = 0;
    SetupDiGetDeviceInterfaceDetailW(di, &ifd, nullptr, 0, &need, nullptr);
    std::vector<u8> buf(need ? need : 512);
    PSP_DEVICE_INTERFACE_DETAIL_DATA_W d = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)buf.data();
    d->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    if (!SetupDiGetDeviceInterfaceDetailW(di, &ifd, d, (DWORD)buf.size(), nullptr, nullptr)) {
        SetupDiDestroyDeviceInfoList(di);
        return false;
    }
    outPath = d->DevicePath;
    SetupDiDestroyDeviceInfoList(di);
    return true;
}

bool BtHci::Open()
{
    std::wstring path;
    if (!FindWinUsbPath(path)) return false;

    dev_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (dev_ == INVALID_HANDLE_VALUE) return false;

    if (!WinUsb_Initialize(dev_, &h_)) {
        CloseHandle(dev_);
        dev_ = INVALID_HANDLE_VALUE;
        return false;
    }

    USB_INTERFACE_DESCRIPTOR ifd = {};
    WinUsb_QueryInterfaceSettings(h_, 0, &ifd);
    for (u8 i = 0; i < ifd.bNumEndpoints; ++i) {
        WINUSB_PIPE_INFORMATION pi = {};
        if (!WinUsb_QueryPipe(h_, 0, i, &pi)) continue;
        if (pi.PipeType == UsbdPipeTypeInterrupt && (pi.PipeId & 0x80)) evtPipe_ = pi.PipeId;
        if (pi.PipeType == UsbdPipeTypeBulk && (pi.PipeId & 0x80))     aclIn_   = pi.PipeId;
        if (pi.PipeType == UsbdPipeTypeBulk && !(pi.PipeId & 0x80))    aclOut_  = pi.PipeId;
    }
    if (evtPipe_ == 0) { Close(); return false; }

    BOOL acs = TRUE;
    WinUsb_SetPipePolicy(h_, evtPipe_, AUTO_CLEAR_STALL, sizeof(acs), &acs);
    if (aclIn_) WinUsb_SetPipePolicy(h_, aclIn_, AUTO_CLEAR_STALL, sizeof(acs), &acs);
    if (aclOut_) WinUsb_SetPipePolicy(h_, aclOut_, AUTO_CLEAR_STALL, sizeof(acs), &acs);

    // 管道超时只在这里设置一次。
    // 不能在每次读之前改策略：ACL 线程和主线程并发调用 SetPipePolicy 会互相干扰，
    // 导致事件丢失（表现为 inquiry 扫不到任何设备）。
    ULONG evtTmo = 300;
    WinUsb_SetPipePolicy(h_, evtPipe_, PIPE_TRANSFER_TIMEOUT, sizeof(evtTmo), &evtTmo);
    if (aclIn_) {
        ULONG aclTmo = 500;
        WinUsb_SetPipePolicy(h_, aclIn_, PIPE_TRANSFER_TIMEOUT, sizeof(aclTmo), &aclTmo);
    }

    // ACL 接收线程
    aclStop_ = false;
    if (aclIn_) aclTh_ = std::thread(&BtHci::AclThread, this);

    return true;
}

void BtHci::Close()
{
    if (aclTh_.joinable()) {
        aclStop_ = true;
        aclTh_.join();
    }
    if (h_) { WinUsb_Free(h_); h_ = nullptr; }
    if (dev_ != INVALID_HANDLE_VALUE) { CloseHandle(dev_); dev_ = INVALID_HANDLE_VALUE; }
    rx_.clear();
    aclRx_.clear();
    aclQ_.clear();
}

// ---------------------------------------------------------------- 原始读写

bool BtHci::ReadPipe(u8 pipe, u8* buf, int cap, int timeoutMs, int& got)
{
    // 注意：这里不再调用 SetPipePolicy（超时已在 Open 时设好）。
    // timeoutMs 只用于决定外层等待时长。
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return false;

    ULONG n = 0;
    BOOL ok = WinUsb_ReadPipe(h_, pipe, buf, (ULONG)cap, &n, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        ok = (WaitForSingleObject(ov.hEvent, (DWORD)(timeoutMs + 1000)) == WAIT_OBJECT_0)
             && WinUsb_GetOverlappedResult(h_, &ov, &n, FALSE);
    }
    DWORD err = GetLastError();
    CloseHandle(ov.hEvent);
    if (!ok) {
        if (err == ERROR_SEM_TIMEOUT || err == WAIT_TIMEOUT) { got = 0; return true; }
        return false;
    }
    got = (int)n;
    (void)timeoutMs;
    return true;
}

// ---------------------------------------------------------------- HCI 命令

bool BtHci::Cmd(u16 op, const void* params, int plen)
{
    if (!h_) return false;
    std::vector<u8> cmd;
    cmd.push_back((u8)(op & 0xFF));
    cmd.push_back((u8)((op >> 8) & 0xFF));
    cmd.push_back((u8)plen);
    if (plen > 0) {
        const u8* p = (const u8*)params;
        cmd.insert(cmd.end(), p, p + plen);
    }

    WINUSB_SETUP_PACKET sp = {};
    sp.RequestType = 0x20;      // Host->Device | Class | Device
    sp.Request     = 0x00;
    sp.Length      = (WORD)cmd.size();

    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return false;
    ULONG tx = 0;
    BOOL ok = WinUsb_ControlTransfer(h_, sp, cmd.data(), (ULONG)cmd.size(), &tx, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        ok = (WaitForSingleObject(ov.hEvent, 3000) == WAIT_OBJECT_0)
             && WinUsb_GetOverlappedResult(h_, &ov, &tx, FALSE);
    }
    CloseHandle(ov.hEvent);
    return ok;
}

// 取下一个完整事件：HCI 事件 = code(1) + param_len(1) + params
bool BtHci::NextEvent(std::vector<u8>& ev, int timeoutMs)
{
    for (;;) {
        if (rx_.size() >= 2) {
            size_t need = 2 + (size_t)rx_[1];
            if (rx_.size() >= need) {
                ev.assign(rx_.begin(), rx_.begin() + need);
                rx_.erase(rx_.begin(), rx_.begin() + need);
                if (verbose_) {
                    wprintf(L"    [ev] code=0x%02X len=%d :", ev[0], (int)ev.size());
                    for (size_t i = 0; i < ev.size() && i < 20; ++i) wprintf(L" %02X", ev[i]);
                    wprintf(L"\n");
                }
                return true;
            }
        }
        u8 chunk[64];
        int got = 0;
        if (!ReadPipe(evtPipe_, chunk, sizeof(chunk), timeoutMs, got)) return false;
        if (got <= 0) {
            // 保留残帧，下次继续拼（长事件 257 字节会跨多次读，
            // 丢掉残帧会导致后续整个事件流错位）
            if (verbose_) wprintf(L"    [rx] timeout, buf=%d\n", (int)rx_.size());
            return false;
        }
        rx_.insert(rx_.end(), chunk, chunk + got);
        if (verbose_) wprintf(L"    [rx] +%-3d buf=%-4d need=%d\n",
                              got, (int)rx_.size(),
                              rx_.size() >= 2 ? 2 + (int)rx_[1] : 0);
    }
}

bool BtHci::CmdComplete(u16 op, const void* params, int plen,
                        std::vector<u8>& ret, int timeoutMs)
{
    if (!Cmd(op, params, plen)) return false;

    int waited = 0;
    while (waited < timeoutMs) {
        std::vector<u8> ev;
        if (!NextEvent(ev, 300)) { waited += 300; continue; }

        if (ev[0] == HCI_EV_CMD_COMPLETE) {
            u16 echo = (u16)(((u16)ev[4] << 8) | ev[3]);
            if (echo == op) {
                if (ev.size() > 6) ret.assign(ev.begin() + 6, ev.end());
                else ret.clear();
                return ev[5] == 0x00;
            }
            continue;   // 别的命令的 Complete，跳过
        }
        if (ev[0] == HCI_EV_CMD_STATUS) {
            u16 echo = (u16)(((u16)ev[5] << 8) | ev[4]);
            if (echo == op && ev[2] != 0x00) return false;
            continue;
        }
        if (ev[0] == HCI_EV_NUM_COMPLETED_PACKETS) continue;
        // 其余事件丢给调用方后续读取会错过；这里简单忽略
    }
    return false;
}

bool BtHci::CmdOk(u16 op, const void* params, int plen)
{
    std::vector<u8> ret;
    return CmdComplete(op, params, plen, ret, 2500);
}

// ---------------------------------------------------------------- ACL

void BtHci::AclThread()
{
    while (!aclStop_) {
        u8 chunk[1024];
        int got = 0;
        if (!ReadPipe(aclIn_, chunk, sizeof(chunk), 500, got)) {
            Sleep(10);
            continue;
        }
        if (got <= 0) continue;

        std::lock_guard<std::mutex> lk(aclMx_);
        aclRx_.insert(aclRx_.end(), chunk, chunk + got);

        // ACL 包 = handle/flags(2) + len(2) + data
        for (;;) {
            if (aclRx_.size() < 4) break;
            size_t total = 4 + (size_t)(aclRx_[2] | ((u16)aclRx_[3] << 8));
            if (aclRx_.size() < total) break;
            std::vector<u8> pkt(aclRx_.begin(), aclRx_.begin() + total);
            aclRx_.erase(aclRx_.begin(), aclRx_.begin() + total);
            aclQ_.push_back(std::move(pkt));
        }
    }
}

bool BtHci::AclSend(u16 handle, const void* data, int len)
{
    if (!h_ || !aclOut_) return false;

    // handle(12bit) | PB(2bit) | BC(2bit)     首片 PB=10, 续片 PB=01
    u16 hdr = (u16)((handle & 0x0FFF) | (0x2 << 12));

    std::vector<u8> pkt;
    pkt.push_back((u8)(hdr & 0xFF));
    pkt.push_back((u8)((hdr >> 8) & 0xFF));
    pkt.push_back((u8)(len & 0xFF));
    pkt.push_back((u8)((len >> 8) & 0xFF));
    const u8* p = (const u8*)data;
    pkt.insert(pkt.end(), p, p + len);

    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return false;
    ULONG tx = 0;
    BOOL ok = WinUsb_WritePipe(h_, aclOut_, pkt.data(), (ULONG)pkt.size(), &tx, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        ok = (WaitForSingleObject(ov.hEvent, 3000) == WAIT_OBJECT_0)
             && WinUsb_GetOverlappedResult(h_, &ov, &tx, FALSE);
    }
    CloseHandle(ov.hEvent);
    return ok;
}

bool BtHci::AclRecv(std::vector<u8>& pkt, int timeoutMs)
{
    int waited = 0;
    while (waited < timeoutMs) {
        {
            std::lock_guard<std::mutex> lk(aclMx_);
            if (!aclQ_.empty()) {
                pkt = std::move(aclQ_.front());
                aclQ_.pop_front();
                return true;
            }
        }
        Sleep(20);
        waited += 20;
    }
    return false;
}

// ---------------------------------------------------------------- ACL 连接

// 处理连接过程中可能出现的配对事件（自动接受，失败也不影响链路建立）
void BtHci::HandlePairingEvents(const std::vector<u8>& ev)
{
    switch (ev[0]) {
    case HCI_EV_LINK_KEY_REQUEST:
        CmdOk(0x040C, ev.data() + 2, 6);          // 暂无密钥，触发配对
        break;
    case HCI_EV_PIN_CODE_REQUEST: {
        u8 pr[23];
        memcpy(pr, ev.data() + 2, 6);
        pr[6] = 4;
        memset(pr + 7, 0, 16);
        pr[7] = '0'; pr[8] = '0'; pr[9] = '0'; pr[10] = '0';
        CmdOk(0x040D, pr, 23);
        break;
    }
    case HCI_EV_IO_CAPABILITY_REQUEST: {
        u8 io[9];
        memcpy(io, ev.data() + 2, 6);
        io[6] = 0x03;   // NoInputNoOutput
        io[7] = 0x00;   // OOB 无
        io[8] = 0x00;   // 不要求 MITM
        CmdOk(0x042B, io, 9);
        break;
    }
    case HCI_EV_USER_CONFIRMATION_REQ:
        CmdOk(0x042C, ev.data() + 2, 6);          // 数值比对自动接受
        break;
    default:
        break;
    }
}

bool BtHci::ConnectAcl(const u8 addr[6], u16& handle, int timeoutMs)
{
    handle = 0xFFFF;

    // HCI_Create_Connection (0x0405)
    u8 p[13];
    memcpy(p, addr, 6);
    p[6] = 0x18; p[7] = 0xCC;   // Packet_Type 0xCC18 (DM1/DH1/DM3/DH3/DM5/DH5)
    p[8] = 0x02;                // PSRM = R2（本 dongle 用 R1 寻呼会超时）
    p[9] = 0x00;                // Reserved
    p[10] = 0x00; p[11] = 0x00; // Clock_Offset 无效
    p[12] = 0x01;               // Allow_Role_Switch
    if (!Cmd(0x0405, p, 13)) return false;

    int waited = 0;
    while (waited < timeoutMs) {
        std::vector<u8> ev;
        if (!NextEvent(ev, 300)) { waited += 300; continue; }

        if (ev[0] == HCI_EV_CONNECTION_COMPLETE) {
            if (ev[2] == 0x00) {
                handle = (u16)(ev[3] | ((u16)(ev[4] & 0x0F) << 8));
                return true;
            }
            return false;   // 连接失败（含 page timeout）
        }
        HandlePairingEvents(ev);
    }
    // 超时，取消寻呼
    u8 cp[6];
    memcpy(cp, addr, 6);
    Cmd(0x0408, cp, 6);         // Create_Connection_Cancel
    return false;
}

void BtHci::DisconnectAcl(u16 handle, u8 reason)
{
    u8 p[3];
    p[0] = (u8)(handle & 0xFF);
    p[1] = (u8)((handle >> 8) & 0x0F);
    p[2] = reason;
    Cmd(0x0406, p, 3);
    int waited = 0;
    while (waited < 3000) {
        std::vector<u8> ev;
        if (!NextEvent(ev, 300)) { waited += 300; continue; }
        if (ev[0] == HCI_EV_DISCONNECTION_COMPLETE) return;
    }
}

// ---------------------------------------------------------------- 初始化

static bool Step(BtHci* h, const wchar_t* name, u16 op, const void* p, int n)
{
    bool ok = h->CmdOk(op, p, n);
    wprintf(L"  %-24s : %s\n", name, ok ? L"OK" : L"FAIL");
    return ok;
}

bool BtHci::Init()
{
    bool ok = true;

    ok &= Step(this, L"Reset", 0x0C03, nullptr, 0);

    std::vector<u8> ret;
    if (CmdComplete(0x1005, nullptr, 0, ret) && ret.size() >= 5) {   // Read_Buffer_Size
        aclMtu_ = (int)(ret[0] | ((u16)ret[1] << 8));
        aclNum_ = (int)(ret[2] | ((u16)ret[3] << 8));
        wprintf(L"  %-24s : OK (ACL mtu=%d num=%d)\n", L"Read_Buffer_Size", aclMtu_, aclNum_);
    } else {
        wprintf(L"  %-24s : FAIL\n", L"Read_Buffer_Size");
    }
    if (aclMtu_ <= 0) aclMtu_ = 672;

    {   // Set_Event_Mask
        u8 m[8];
        for (int i = 0; i < 8; ++i) m[i] = 0xFF;
        m[7] = 0x9F;
        ok &= Step(this, L"Set_Event_Mask", 0x0C01, m, 8);
    }
    { u8 v[3] = { 0x04, 0x04, 0x24 }; ok &= Step(this, L"Write_Class_Of_Device", 0x0C24, v, 3); }
    {
        u8 name[248];
        memset(name, 0, sizeof(name));
        const char* s = "DualBlt";
        memcpy(name, s, strlen(s));
        ok &= Step(this, L"Write_Local_Name", 0x0C13, name, 248);
    }
    { u8 v = 0x01; ok &= Step(this, L"Simple_Pairing_Mode", 0x0C56, &v, 1); }
    { u8 v = 0x01; ok &= Step(this, L"Authentication_Enable", 0x0C20, &v, 1); }
    { u8 v = 0x03; ok &= Step(this, L"Write_Scan_Enable", 0x0C1A, &v, 1); }
    { u8 v = 0x00; ok &= Step(this, L"Inquiry_Mode(std)", 0x0C45, &v, 1); }
    return ok;
}

// ---------------------------------------------------------------- 只读操作

bool BtHci::ReadBdAddr(u8 out[6])
{
    if (!CmdOk(0x0C03, nullptr, 0)) return false;              // Reset
    std::vector<u8> ret;
    if (!CmdComplete(0x1009, nullptr, 0, ret, 2000)) return false;
    if (ret.size() < 6) return false;
    memcpy(out, ret.data(), 6);
    return true;
}

bool BtHci::ScanBegin()
{
    if (!Init()) return false;
    // 标准查询模式。
    // 说明：
    //  - Remote_Name_Request：本克隆芯片直接拒绝（Command Disallowed 0x0C），无法用
    //  - EIR 模式(0x02)：固件有缺陷，inquiry 后第一个 0x2F 事件会丢掉头部 16 字节，
    //    导致事件流错位。所以名字不走空中查询，由上层从 Windows 已配对记录里补。
    u8 v = 0x00;
    CmdOk(0x0C45, &v, 1);
    return true;
}

void BtHci::ScanEnd() {}

void BtHci::DebugRawInquiry(int seconds)
{
    u8 p[5] = { 0x33, 0x8B, 0x9E, (u8)seconds, 0x00 };
    if (!Cmd(0x0401, p, 5)) { wprintf(L"  inquiry send failed\n"); return; }

    int waited = 0;
    while (waited < (seconds + 6) * 1000) {
        u8 buf[128];
        int got = 0;
        if (!ReadPipe(evtPipe_, buf, sizeof(buf), 300, got)) break;
        if (got <= 0) { waited += 300; continue; }
        wprintf(L"  RAW +%3d: ", got);
        for (int i = 0; i < got && i < 100; ++i) wprintf(L" %02X", buf[i]);
        wprintf(L"\n");
        if (got < (int)sizeof(buf)) waited += 0;   // 短包不代表结束，继续读
    }
}

bool BtHci::RemoteName(const u8 addr[6], u8 psrm, u16 clk, std::string& out)
{
    // HCI_Remote_Name_Request (0x0419)
    // BD_ADDR(6) + Page_Scan_Repetition_Mode(1) + Reserved(1) + Clock_Offset(2)
    u8 rp[10];
    memcpy(rp, addr, 6);
    rp[6] = psrm;                       // R2 是所有设备强制支持的模式，R1 在本 dongle 上会超时
    rp[7] = 0x00;
    rp[8] = (u8)(clk & 0xFF);
    rp[9] = (u8)((clk >> 8) | 0x80);    // bit15 = 时钟偏移有效
    if (!Cmd(0x0419, rp, 10)) return false;
    for (int t = 0; t < 20; ++t) {
        std::vector<u8> e2;
        if (!NextEvent(e2, 500)) break;
        if (e2[0] == 0x07 && e2.size() > 9 && e2[2] == 0x00) {
            for (size_t i = 9; i < e2.size() && e2[i] != 0; ++i) out.push_back((char)e2[i]);
            return true;
        }
        if (e2[0] == 0x07) return false;   // status != 0，别再等
    }
    return false;
}

// 单轮 inquiry；resolveNames=false 时跳过名字解析（持续扫描时对老设备跳过）
bool BtHci::InquiryRound(std::vector<ScanDev>& out, int seconds, bool resolveNames)
{
    out.clear();

    u8 p[5];
    p[0] = 0x33; p[1] = 0x8B; p[2] = 0x9E;   // GIAC
    p[3] = (u8)((seconds * 10 + 12) / 13);   // 每 1.28s 一个单位
    p[4] = 0x00;
    if (!Cmd(0x0401, p, 5)) return false;

    int waited = 0;
    std::vector<std::vector<u8>> addrs;
    while (waited < (seconds + 6) * 1000) {
        std::vector<u8> ev;
        if (!NextEvent(ev, 300)) { waited += 300; continue; }

        if (ev[0] == HCI_EV_INQUIRY_COMPLETE) break;
        if (ev[0] == HCI_EV_CMD_STATUS) continue;

        int base = 0, stride = 0;
        if (ev[0] == HCI_EV_INQUIRY_RESULT)      { base = 3; stride = 14; }
        else if (ev[0] == 0x22)                  { base = 3; stride = 15; }
        else if (ev[0] == 0x2F)                  { base = 3; stride = 14; }
        else continue;

        int cnt = ev[1];
        const u8* q = ev.data() + base;
        for (int i = 0; i < cnt; ++i) {
            if (q + 6 > ev.data() + (int)ev.size()) break;
            bool dup = false;
            for (auto& a : addrs)
                if (memcmp(a.data(), q, 6) == 0) { dup = true; break; }
            if (!dup) {
                ScanDev d;
                memcpy(d.addr, q, 6);
                d.psrm = q[6];
                if (stride == 14 && ev[0] == HCI_EV_INQUIRY_RESULT) {
                    d.cod[0] = q[9]; d.cod[1] = q[10]; d.cod[2] = q[11]; d.rssi = 0;
                    d.clk = (u16)(q[12] | ((u16)q[13] << 8));
                } else if (ev[0] == 0x22) {
                    d.cod[0] = q[9]; d.cod[1] = q[10]; d.cod[2] = q[11];
                    d.rssi = (signed char)q[14];
                    d.clk = (u16)(q[12] | ((u16)q[13] << 8));
                } else {   // 0x2F
                    d.cod[0] = q[8]; d.cod[1] = q[9]; d.cod[2] = q[10];
                    d.rssi = (signed char)q[13];
                    d.clk = (u16)(q[11] | ((u16)q[12] << 8));
                    // EIR 数据区从 q+14 开始，长度 = 事件总长 - 17
                    int eirOff = 14;
                    int eirLen = (int)ev.size() - (3 + eirOff);
                    const u8* eir = q + eirOff;
                    int pos = 0;
                    while (pos + 1 < eirLen) {
                        int len = eir[pos];
                        if (len <= 0 || pos + 1 + len > eirLen) break;
                        int typ = eir[pos + 1];
                        if ((typ == 0x09 || typ == 0x08) && len > 1) {
                            d.name.assign((const char*)(eir + pos + 2), len - 1);
                            break;
                        }
                        pos += len + 1;
                    }
                }
                addrs.push_back(std::vector<u8>(q, q + 6));
                out.push_back(d);
            }
            q += stride;
        }
    }

    if (!resolveNames) return true;

    // 逐个取远端名字（用 inquiry 得到的 PSRM 与时钟偏移，寻呼成功率更高）
    for (auto& d : out) {
        RemoteName(d.addr, d.psrm, d.clk, d.name);
    }
    return true;
}

bool BtHci::Scan(std::vector<ScanDev>& out, int seconds, std::wstring& err)
{
    out.clear();
    if (!ScanBegin()) { err = L"HCI 初始化失败"; return false; }
    if (!InquiryRound(out, seconds, true)) { err = L"inquiry 发送失败"; return false; }
    return true;
}
