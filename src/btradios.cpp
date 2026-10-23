#ifndef UNICODE
#define UNICODE
#endif

#include <windows.h>
#include <bluetoothapis.h>
#include <bthsdpdef.h>

#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "Bthprops.lib")

// 常见服务 UUID（Bluetooth Base UUID 前缀 0000xxxx-0000-1000-8000-00805F9B34FB）
static const GUID GUID_AUDIOSINK  = { 0x0000110B, 0x0000, 0x1000, { 0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB } };
static const GUID GUID_HEADSET    = { 0x00001108, 0x0000, 0x1000, { 0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB } };
static const GUID GUID_HANDSFREE  = { 0x0000111E, 0x0000, 0x1000, { 0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB } };
static const GUID GUID_AVRCP_TG   = { 0x0000110C, 0x0000, 0x1000, { 0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB } };

static const wchar_t* SvcName(const GUID& g)
{
    if (IsEqualGUID(g, GUID_AUDIOSINK)) return L"AudioSink(A2DP音频)";
    if (IsEqualGUID(g, GUID_HEADSET))   return L"Headset";
    if (IsEqualGUID(g, GUID_HANDSFREE)) return L"Handsfree";
    if (IsEqualGUID(g, GUID_AVRCP_TG))  return L"AVRCP-TG";
    return nullptr;
}

int wmain()
{
    BLUETOOTH_FIND_RADIO_PARAMS p = { sizeof(p) };
    HANDLE h = INVALID_HANDLE_VALUE;
    HBLUETOOTH_RADIO_FIND f = BluetoothFindFirstRadio(&p, &h);

    if (!f) {
        wprintf(L"No Bluetooth radio enumerated (GetLastError=%lu)\n", GetLastError());
        return 1;
    }

    int n = 0;
    do {
        BLUETOOTH_RADIO_INFO ri = {};
        ri.dwSize = sizeof(ri);

        if (BluetoothGetRadioInfo(h, &ri) == ERROR_SUCCESS) {
            wprintf(L"[radio %d] %s\n", n, ri.szName);
            wprintf(L"    BD_ADDR      : %02X:%02X:%02X:%02X:%02X:%02X\n",
                    ri.address.rgBytes[5], ri.address.rgBytes[4], ri.address.rgBytes[3],
                    ri.address.rgBytes[2], ri.address.rgBytes[1], ri.address.rgBytes[0]);
            wprintf(L"    class        : 0x%06X   manufacturer: 0x%04X   lmpSubver: 0x%04X\n",
                    ri.ulClassofDevice, ri.manufacturer, ri.lmpSubversion);
            wprintf(L"    discoverable : %d  connectable : %d\n",
                    BluetoothIsDiscoverable(h), BluetoothIsConnectable(h));
        } else {
            wprintf(L"[radio %d] (BluetoothGetRadioInfo failed, err=%lu)\n", n, GetLastError());
        }

        // ---- 该 radio 下的设备 ----
        BLUETOOTH_DEVICE_SEARCH_PARAMS sp = {};
        sp.dwSize               = sizeof(sp);
        sp.hRadio               = h;
        sp.fReturnAuthenticated = TRUE;
        sp.fReturnRemembered    = TRUE;
        sp.fReturnUnknown       = TRUE;
        sp.fReturnConnected     = TRUE;
        sp.fIssueInquiry        = FALSE;

        BLUETOOTH_DEVICE_INFO di = {};
        di.dwSize = sizeof(di);
        HBLUETOOTH_DEVICE_FIND fd = BluetoothFindFirstDevice(&sp, &di);

        if (!fd) {
            wprintf(L"    (no device)\n");
        } else {
            wprintf(L"    devices:\n");
            int dn = 0;
            do {
                wprintf(L"      [%d] %s\n", dn, di.szName);
                wprintf(L"          addr        : %02X:%02X:%02X:%02X:%02X:%02X\n",
                        di.Address.rgBytes[5], di.Address.rgBytes[4], di.Address.rgBytes[3],
                        di.Address.rgBytes[2], di.Address.rgBytes[1], di.Address.rgBytes[0]);
                wprintf(L"          connected   : %d   remembered: %d   authenticated: %d\n",
                        (int)di.fConnected, (int)di.fRemembered, (int)di.fAuthenticated);
                wprintf(L"          classOfDev  : 0x%06X\n", di.ulClassofDevice);

                DWORD num = 0;
                DWORD err = BluetoothEnumerateInstalledServices(h, &di, &num, nullptr);
                if (err == ERROR_SUCCESS && num > 0) {
                    std::vector<GUID> guids(num);
                    DWORD n2 = num;
                    if (BluetoothEnumerateInstalledServices(h, &di, &n2, guids.data()) == ERROR_SUCCESS) {
                        wprintf(L"          services    :");
                        bool hasSink = false;
                        for (DWORD k = 0; k < n2; ++k) {
                            const wchar_t* nm = SvcName(guids[k]);
                            if (nm) {
                                wprintf(L" [%s]", nm);
                                if (IsEqualGUID(guids[k], GUID_AUDIOSINK)) hasSink = true;
                            } else {
                                wprintf(L" {%08X-...}", guids[k].Data1);
                            }
                        }
                        wprintf(L"\n");
                        wprintf(L"          A2DP 音频   : %s\n", hasSink ? L"已装载" : L"无");
                    }
                } else {
                    wprintf(L"          services    : (enum failed err=%lu num=%lu)\n", err, num);
                }
                ++dn;
            } while (BluetoothFindNextDevice(fd, &di));
            BluetoothFindDeviceClose(fd);
        }

        CloseHandle(h);
        h = INVALID_HANDLE_VALUE;
        ++n;
    } while (BluetoothFindNextRadio(f, &h));

    BluetoothFindRadioClose(f);
    wprintf(L"total radios = %d\n", n);
    return 0;
}
