#ifndef UNICODE
#define UNICODE
#endif

// 用户态 HCI 初始化 + 主动扫描（inquiry）
// 全程只发标准 HCI 命令，不写 NVM。

#include <windows.h>
#include <winusb.h>
#include <setupapi.h>
#include <initguid.h>
#include <usbspec.h>

#include <cstdio>
#include <string>
#include <vector>
#include <set>

#pragma comment(lib, "winusb.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "user32.lib")

DEFINE_GUID(GUID_DEVIF_DUALBT_WINUSB,
    0x2F5A1E1B, 0x9C4E, 0x4E27, 0x8A, 0x0D, 0x6B, 0x5C, 0x1D, 0x3E, 0x7A, 0x01);

struct Ctx {
    WINUSB_INTERFACE_HANDLE h = nullptr;
    UCHAR evtPipe = 0xFF;
    UCHAR aclOut  = 0xFF;
    UCHAR aclIn   = 0xFF;
    int   aclMtu  = 0;
    int   aclNum  = 0;
};

static std::vector<BYTE> g_rx;    // HCI 事件流重组缓冲

static bool FindWinUsbPath(std::wstring& outPath)
{
    HDEVINFO di = SetupDiGetClassDevsW(&GUID_DEVIF_DUALBT_WINUSB, nullptr, nullptr,
                                       DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (di == INVALID_HANDLE_VALUE) return false;
    SP_DEVICE_INTERFACE_DATA ifd = {};
    ifd.cbSize = sizeof(ifd);
    if (!SetupDiEnumDeviceInterfaces(di, nullptr, &GUID_DEVIF_DUALBT_WINUSB, 0, &ifd)) {
        SetupDiDestroyDeviceInfoList(di); return false;
    }
    DWORD need = 0;
    SetupDiGetDeviceInterfaceDetailW(di, &ifd, nullptr, 0, &need, nullptr);
    std::vector<BYTE> buf(need ? need : 512);
    PSP_DEVICE_INTERFACE_DETAIL_DATA_W d = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)buf.data();
    d->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    if (!SetupDiGetDeviceInterfaceDetailW(di, &ifd, d, (DWORD)buf.size(), nullptr, nullptr)) {
        SetupDiDestroyDeviceInfoList(di); return false;
    }
    outPath = d->DevicePath;
    SetupDiDestroyDeviceInfoList(di);
    return true;
}

static bool WaitOv(WINUSB_INTERFACE_HANDLE h, OVERLAPPED& ov, ULONG& tx)
{
    if (WaitForSingleObject(ov.hEvent, 3000) != WAIT_OBJECT_0) return false;
    return WinUsb_GetOverlappedResult(h, &ov, &tx, FALSE) ? true : false;
}

static bool HciCommand(Ctx& c, unsigned op, const BYTE* params, int plen)
{
    std::vector<BYTE> cmd;
    cmd.push_back((BYTE)(op & 0xFF));
    cmd.push_back((BYTE)((op >> 8) & 0xFF));
    cmd.push_back((BYTE)plen);
    if (plen > 0) cmd.insert(cmd.end(), params, params + plen);

    WINUSB_SETUP_PACKET sp = {};
    sp.RequestType = 0x20;
    sp.Request     = 0x00;
    sp.Length      = (WORD)cmd.size();

    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return false;
    ULONG tx = 0;
    BOOL ok = WinUsb_ControlTransfer(c.h, sp, cmd.data(), (ULONG)cmd.size(), &tx, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) ok = WaitOv(c.h, ov, tx);
    CloseHandle(ov.hEvent);
    return ok;
}

static int ReadRaw(Ctx& c, BYTE* buf, int cap, int timeoutMs)
{
    ULONG tmo = (ULONG)timeoutMs;
    WinUsb_SetPipePolicy(c.h, c.evtPipe, PIPE_TRANSFER_TIMEOUT, sizeof(tmo), &tmo);

    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return -1;
    ULONG got = 0;
    BOOL ok = WinUsb_ReadPipe(c.h, c.evtPipe, buf, (ULONG)cap, &got, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) ok = WaitOv(c.h, ov, got);
    DWORD err = GetLastError();
    CloseHandle(ov.hEvent);
    if (!ok) {
        if (err == ERROR_SEM_TIMEOUT || err == WAIT_TIMEOUT) return 0;
        return -1;
    }
    return (int)got;
}

// 从缓冲里切出一个完整事件；不足则继续读
static bool NextEvent(Ctx& c, std::vector<BYTE>& out, int timeoutMs)
{
    for (;;) {
        if (g_rx.size() >= 2) {
            size_t need = 2 + (size_t)g_rx[1];
            if (g_rx.size() >= need) {
                out.assign(g_rx.begin(), g_rx.begin() + need);
                g_rx.erase(g_rx.begin(), g_rx.begin() + need);
                return true;
            }
        }
        BYTE chunk[64];
        int n = ReadRaw(c, chunk, sizeof(chunk), timeoutMs);
        if (n <= 0) return false;
        g_rx.insert(g_rx.end(), chunk, chunk + n);
    }
}

static void Dump(const BYTE* b, int n, int maxn = 20)
{
    for (int i = 0; i < n && i < maxn; ++i) wprintf(L"%02X ", b[i]);
    if (n > maxn) wprintf(L"...");
}

static bool CmdComplete(Ctx& c, unsigned op, const BYTE* params, int plen,
                        const wchar_t* label, BYTE* out = nullptr, int outCap = 0, int* outLen = nullptr)
{
    if (!HciCommand(c, op, params, plen)) {
        wprintf(L"  %-28s : SEND FAILED\n", label);
        return false;
    }
    std::vector<BYTE> ev;
    if (!NextEvent(c, ev, 2500) || ev[0] != 0x0E) {
        wprintf(L"  %-28s : NO RESPONSE\n", label);
        return false;
    }
    unsigned echo = ((unsigned)ev[4] << 8) | ev[3];
    BYTE status = ev[5];
    bool ok = (echo == op && status == 0x00);

    wprintf(L"  %-28s : %s", label, ok ? L"OK" : L"FAIL");
    if (!ok) wprintf(L" (echo=0x%04X status=0x%02X)", echo, status);
    wprintf(L"\n");

    if (ok && out && outCap > 0) {
        int payload = (int)ev.size() - 6;
        if (payload > 0) {
            if (payload > outCap) payload = outCap;
            memcpy(out, ev.data() + 6, payload);
            if (outLen) *outLen = payload;
        }
    }
    return ok;
}

// 从 EIR 数据里取设备名
static std::string EirName(const BYTE* eir, int left)
{
    int pos = 0;
    while (pos + 1 < left) {
        int len = eir[pos];
        if (len <= 0) break;
        if (pos + 1 + len > left) break;
        int typ = eir[pos + 1];
        if ((typ == 0x09 || typ == 0x08) && len > 1)
            return std::string((const char*)(eir + pos + 2), len - 1);
        pos += len + 1;
    }
    return std::string();
}

static void PrintAddr(const BYTE* a)
{
    wprintf(L"%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
}

int wmain()
{
    std::wstring path;
    if (!FindWinUsbPath(path)) { wprintf(L"No WinUSB interface found.\n"); return 1; }

    HANDLE dev = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (dev == INVALID_HANDLE_VALUE) { wprintf(L"CreateFile failed %lu\n", GetLastError()); return 1; }

    Ctx c;
    if (!WinUsb_Initialize(dev, &c.h)) { wprintf(L"WinUsb_Initialize failed %lu\n", GetLastError()); CloseHandle(dev); return 1; }

    USB_INTERFACE_DESCRIPTOR ifd = {};
    WinUsb_QueryInterfaceSettings(c.h, 0, &ifd);
    for (UCHAR i = 0; i < ifd.bNumEndpoints; ++i) {
        WINUSB_PIPE_INFORMATION pi = {};
        if (!WinUsb_QueryPipe(c.h, 0, i, &pi)) continue;
        if (pi.PipeType == UsbdPipeTypeInterrupt && (pi.PipeId & 0x80))  c.evtPipe = pi.PipeId;
        if (pi.PipeType == UsbdPipeTypeBulk     && (pi.PipeId & 0x80))  c.aclIn   = pi.PipeId;
        if (pi.PipeType == UsbdPipeTypeBulk     && !(pi.PipeId & 0x80)) c.aclOut  = pi.PipeId;
    }
    if (c.evtPipe == 0xFF) { wprintf(L"No interrupt IN pipe\n"); WinUsb_Free(c.h); CloseHandle(dev); return 1; }
    BOOL acs = TRUE;
    WinUsb_SetPipePolicy(c.h, c.evtPipe, AUTO_CLEAR_STALL, sizeof(acs), &acs);

    wprintf(L"pipes: event=0x%02X aclIN=0x%02X aclOUT=0x%02X\n\n", c.evtPipe, c.aclIn, c.aclOut);

    // ---------------- HCI 初始化 ----------------
    wprintf(L"=== HCI init ===\n");
    CmdComplete(c, 0x0C03, nullptr, 0, L"Reset");

    BYTE tmp[256] = {}; int tl = 0;
    if (CmdComplete(c, 0x1005, nullptr, 0, L"Read_Buffer_Size", tmp, sizeof(tmp), &tl) && tl >= 5) {
        c.aclMtu = ((int)tmp[1] << 8) | tmp[0];
        c.aclNum = ((int)tmp[3] << 8) | tmp[2];
        wprintf(L"      ACL mtu=%d  num=%d\n", c.aclMtu, c.aclNum);
    }
    if (CmdComplete(c, 0x1003, nullptr, 0, L"Read_Local_Features", tmp, sizeof(tmp), &tl) && tl >= 8) {
        wprintf(L"      features:");
        for (int i = 0; i < 8 && i < tl; ++i) wprintf(L" %02X", tmp[i]);
        wprintf(L"\n");
    }
    if (CmdComplete(c, 0x1009, nullptr, 0, L"Read_BD_ADDR", tmp, sizeof(tmp), &tl) && tl >= 6) {
        wprintf(L"      BD_ADDR="); PrintAddr(tmp); wprintf(L"\n");
    }
    {
        BYTE m[8];
        for (int i = 0; i < 8; ++i) m[i] = 0xFF;
        m[7] = 0x9F;
        CmdComplete(c, 0x0C01, m, 8, L"Set_Event_Mask");
    }
    { BYTE v[3] = { 0x04, 0x04, 0x24 }; CmdComplete(c, 0x0C24, v, 3, L"Write_Class_Of_Device"); }
    {
        BYTE name[248]; memset(name, 0, sizeof(name));
        const char* s = "DualBlt"; memcpy(name, s, strlen(s));
        CmdComplete(c, 0x0C13, name, 248, L"Write_Local_Name");
    }
    { BYTE v[1] = { 0x01 }; CmdComplete(c, 0x0C56, v, 1, L"Write_Simple_Pairing_Mode"); }
    { BYTE v[1] = { 0x01 }; CmdComplete(c, 0x0C20, v, 1, L"Write_Authentication_Enable"); }
    { BYTE v[1] = { 0x03 }; CmdComplete(c, 0x0C1A, v, 1, L"Write_Scan_Enable"); }
    // 标准查询模式：只回 0x02 短事件，不带 240 字节 EIR，避免超长事件定帧问题
    { BYTE v[1] = { 0x00 }; CmdComplete(c, 0x0C45, v, 1, L"Write_Inquiry_Mode(std)"); }

    // ---------------- Inquiry ----------------
    wprintf(L"\n=== Inquiry (10.24s) ===\n");
    BYTE p[5];
    p[0] = 0x33; p[1] = 0x8B; p[2] = 0x9E;   // GIAC
    p[3] = 0x08;                              // 8 * 1.28s
    p[4] = 0x00;
    if (!HciCommand(c, 0x0401, p, 5)) {
        wprintf(L"  inquiry send failed\n");
    } else {
        std::set<std::string> seen;
        std::vector<std::vector<BYTE>> foundAddrs;
        int waited = 0;
        int dumped = 0;
        std::vector<BYTE> ev;

        while (waited < 16000) {
            ev.clear();
            if (!NextEvent(c, ev, 500)) { waited += 500; continue; }

            BYTE code = ev[0];
            int  len  = (int)ev.size();

            if (code == 0x01) {   // Inquiry Complete
                wprintf(L"  Inquiry Complete (status=0x%02X)\n", ev.size() > 2 ? ev[2] : 0xFF);
                break;
            }
            if (code == 0x0F) {   // Command Status
                wprintf(L"  Command Status: status=0x%02X opcode=0x%02X%02X\n",
                        ev[2], ev[5], ev[4]);
                continue;
            }
            if (code == 0x00 && ev.size() == 2) continue;   // 空填充，忽略
            if (code == 0x02) {   // Inquiry Result：每条 14 字节
                int cnt = ev[1];
                const BYTE* q = ev.data() + 3;   // 跳过 code / param_len / Num_Responses

                // 前两个事件打印原始字节，用来核对字段偏移
                if (dumped < 2) {
                    ++dumped;
                    wprintf(L"  [raw 0x02 len=%d] ", len);
                    Dump(ev.data(), len, 40); wprintf(L"\n");
                }

                for (int i = 0; i < cnt; ++i) {
                    if (q + 14 > ev.data() + len) break;
                    char key[32];
                    snprintf(key, sizeof(key), "%02X%02X%02X%02X%02X%02X",
                             q[5], q[4], q[3], q[2], q[1], q[0]);
                    if (seen.insert(key).second) {
                        std::vector<BYTE> a(q, q + 6);
                        foundAddrs.push_back(a);
                        wprintf(L"  addr="); PrintAddr(q);
                        wprintf(L"  cod=0x%02X%02X%02X\n", q[11], q[10], q[9]);
                    }
                    q += 14;
                }
                continue;
            }
            if (code == 0x22) {   // Inquiry Result with RSSI：每条 15 字节
                int cnt = ev[1];
                const BYTE* q = ev.data() + 3;
                for (int i = 0; i < cnt; ++i) {
                    if (q + 15 > ev.data() + len) break;
                    char key[32];
                    snprintf(key, sizeof(key), "%02X%02X%02X%02X%02X%02X",
                             q[5], q[4], q[3], q[2], q[1], q[0]);
                    if (seen.insert(key).second) {
                        wprintf(L"  addr="); PrintAddr(q);
                        wprintf(L"  cod=0x%02X%02X%02X  rssi=%d\n",
                                q[11], q[10], q[9], (int)(signed char)q[14]);
                    }
                    q += 15;
                }
                continue;
            }
            if (code == 0x2F) {   // Extended Inquiry Result
                const BYTE* q = ev.data() + 3;
                if (q + 14 > ev.data() + len) continue;
                char key[32];
                snprintf(key, sizeof(key), "%02X%02X%02X%02X%02X%02X",
                         q[5], q[4], q[3], q[2], q[1], q[0]);
                if (seen.insert(key).second) {
                    wprintf(L"  addr="); PrintAddr(q);
                    wprintf(L"  cod=0x%02X%02X%02X  rssi=%d",
                            q[10], q[9], q[8], (int)(signed char)q[13]);
                    std::string nm = EirName(q + 14, (int)(ev.data() + len - (q + 14)));
                    if (!nm.empty()) wprintf(L"  name=%S", std::wstring(nm.begin(), nm.end()).c_str());
                    wprintf(L"\n");
                }
                continue;
            }
            if (code == 0x3E) continue;   // LE Meta，忽略

            wprintf(L"      [ev 0x%02X] ", code);
            Dump(ev.data(), len, 12); wprintf(L"\n");
        }
        wprintf(L"  distinct devices = %d\n", (int)seen.size());

        // ---- 逐个查远端设备名（Remote_Name_Request, 0x0419）----
        wprintf(L"\n=== Remote Name Request ===\n");
        for (size_t k = 0; k < foundAddrs.size(); ++k) {
            const BYTE* a = foundAddrs[k].data();
            BYTE rp[10];
            memcpy(rp, a, 6);
            rp[6] = 0x01;   // Page_Scan_Repetition_Mode = R1
            rp[7] = 0x00;   // Reserved
            rp[8] = 0x00;   // Clock_Offset
            rp[9] = 0x00;

            wprintf(L"  "); PrintAddr(a); wprintf(L" : ");

            if (!HciCommand(c, 0x0419, rp, 10)) { wprintf(L"send failed\n"); continue; }

            std::vector<BYTE> e2;
            bool got = false;
            for (int t = 0; t < 20 && !got; ++t) {
                e2.clear();
                if (!NextEvent(c, e2, 600)) continue;
                if (e2[0] == 0x07) {                       // Remote Name Request Complete
                    BYTE st = e2.size() > 2 ? e2[2] : 0xFF;
                    if (st == 0x00) {
                        std::string nm;
                        for (size_t i = 9; i < e2.size() && e2[i] != 0; ++i) nm.push_back((char)e2[i]);
                        wprintf(L"%S\n", std::wstring(nm.begin(), nm.end()).c_str());
                    } else {
                        wprintf(L"(status=0x%02X)\n", st);
                    }
                    got = true;
                }
            }
            if (!got) wprintf(L"(no name response)\n");
        }
    }

    WinUsb_Free(c.h);
    CloseHandle(dev);
    return 0;
}
