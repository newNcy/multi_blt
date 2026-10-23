#pragma once

// 用户态蓝牙协议栈核心：WinUSB 传输层 + HCI + ACL
// 不依赖任何 Windows 蓝牙 API（不使用 bluetoothapis / BthPort）。

#include <windows.h>
#include <winusb.h>

#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

typedef unsigned char  u8;
typedef unsigned short u16;

bool BtAddrFromStr(const std::wstring& s, u8 out[6]);
std::wstring BtAddrToStr(const u8 a[6]);

// HCI 事件码
enum : u8 {
    HCI_EV_INQUIRY_COMPLETE        = 0x01,
    HCI_EV_INQUIRY_RESULT          = 0x02,
    HCI_EV_CONNECTION_COMPLETE     = 0x03,
    HCI_EV_CONNECTION_REQUEST      = 0x04,
    HCI_EV_DISCONNECTION_COMPLETE  = 0x05,
    HCI_EV_REMOTE_NAME_REQ_COMPLETE= 0x07,
    HCI_EV_CMD_COMPLETE            = 0x0E,
    HCI_EV_CMD_STATUS              = 0x0F,
    HCI_EV_NUM_COMPLETED_PACKETS   = 0x13,
    HCI_EV_LINK_KEY_REQUEST        = 0x17,
    HCI_EV_PIN_CODE_REQUEST        = 0x16,
    HCI_EV_IO_CAPABILITY_REQUEST   = 0x31,
    HCI_EV_IO_CAPABILITY_RESPONSE  = 0x32,
    HCI_EV_USER_CONFIRMATION_REQ   = 0x33,
    HCI_EV_SIMPLE_PAIRING_COMPLETE = 0x36,
    HCI_EV_LINK_KEY_NOTIFICATION   = 0x18,
};

class BtHci {
public:
    BtHci();
    ~BtHci();

    // 打开第一个 WinUSB 蓝牙 dongle
    bool Open();
    void Close();
    bool IsOpen() const { return h_ != nullptr; }

    // ---- HCI 命令 ----
    bool Cmd(u16 op, const void* params, int plen);

    // 发命令并等待对应的 Command Complete；ret 返回参数部分
    bool CmdComplete(u16 op, const void* params, int plen,
                     std::vector<u8>& ret, int timeoutMs = 2500);
    // 只等成功与否
    bool CmdOk(u16 op, const void* params, int plen);

    // 取下一个完整 HCI 事件（含 2 字节头）
    bool NextEvent(std::vector<u8>& ev, int timeoutMs);

    // ---- ACL ----
    bool AclSend(u16 handle, const void* data, int len);
    bool AclRecv(std::vector<u8>& pkt, int timeoutMs);   // pkt 含 4 字节 ACL 头

    int  AclMtu() const { return aclMtu_; }

    // 标准初始化序列
    bool Init();

    // 只读：Reset 后读 BD_ADDR（不做完整初始化）
    bool ReadBdAddr(u8 out[6]);

    // 只读：主动扫描（inquiry），返回发现的设备
    struct ScanDev {
        u8  addr[6];
        u8  cod[3];
        int rssi;
        u8  psrm;        // Page_Scan_Repetition_Mode（来自 inquiry 结果）
        u16 clk;         // Clock_Offset（来自 inquiry 结果，不含有效位）
        std::string name;   // UTF-8
    };
    bool Scan(std::vector<ScanDev>& out, int seconds, std::wstring& err);

    // 持续扫描：Begin 一次，然后循环 InquiryRound，最后 End + Close
    bool ScanBegin();
    bool InquiryRound(std::vector<ScanDev>& out, int seconds, bool resolveNames = true);
    void ScanEnd();

    // 调试用：把收到的每个 HCI 事件打印出来
    void SetVerbose(bool v) { verbose_ = v; }

    // 调试用：发 inquiry 并把管道里的原始字节流原样倒出来（不做任何组帧）
    void DebugRawInquiry(int seconds);

    // 单个设备的远端名字（用于持续扫描时只查新设备）
    // psrm 用 R2（本 dongle 用 R1 寻呼会超时），clk 传 inquiry 得到的时钟偏移
    bool RemoteName(const u8 addr[6], u8 psrm, u16 clk, std::string& out);

    // 建立/断开 ACL 链路（不配对；只用于在已有链路上查询信息）
    bool ConnectAcl(const u8 addr[6], u16& handle, int timeoutMs = 15000);
    void DisconnectAcl(u16 handle, u8 reason = 0x13);

private:
    void AclThread();
    bool ReadPipe(u8 pipe, u8* buf, int cap, int timeoutMs, int& got);
    void HandlePairingEvents(const std::vector<u8>& ev);

    HANDLE                   dev_ = INVALID_HANDLE_VALUE;
    WINUSB_INTERFACE_HANDLE  h_   = nullptr;
    u8   evtPipe_ = 0, aclIn_ = 0, aclOut_ = 0;
    int  aclMtu_ = 0;
    int  aclNum_ = 0;

    std::vector<u8> rx_;        // 事件重组缓冲（仅主线程使用）
    bool            verbose_ = false;

    std::thread              aclTh_;
    bool                     aclStop_ = false;
    std::mutex               aclMx_;
    std::deque<std::vector<u8>> aclQ_;
    std::vector<u8>          aclRx_;
};
