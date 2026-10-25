@echo off
setlocal
set VCVARS=G:\VS2022BuildTools\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" goto :novc
call "%VCVARS%"
if errorlevel 1 goto :vcfail
if not exist build mkdir build
if not exist build\bt mkdir build\bt
copy /y "g:\multi_blt\winusb\pkg\DualBltWinUSB.inf" build\ >nul 2>&1
copy /y "g:\multi_blt\winusb\cert.pfx" build\winusb_cert.pfx >nul 2>&1
copy /y "g:\multi_blt\assets\DualBlt.ico" build\ >nul 2>&1
copy /y "g:\multi_blt\assets\fa-solid-900.ttf" build\ >nul 2>&1
copy /y "g:\multi_blt\assets\ui.html" build\ >nul 2>&1
copy /y "g:\multi_blt\webview2\x64\WebView2Loader.dll" build\ >nul 2>&1

set BTSTACK=g:\multi_blt\third_party\btstack
set PORT=g:\multi_blt\btstack_port
set CFLAGS=/nologo /O2 /EHsc /std:c++17 /utf-8 /W3 /wd4819 /DNOMINMAX /D_UNICODE /DUNICODE

echo --- GUI ---
echo --- BTstack A2DP core (GUI 用) ---
cl %CFLAGS% /c /Fo:build\bt\ ^
  "%BTSTACK%\src\btstack_linked_list.c" "%BTSTACK%\src\btstack_memory.c" "%BTSTACK%\src\btstack_memory_pool.c" ^
  "%BTSTACK%\src\btstack_run_loop.c" "%BTSTACK%\src\btstack_run_loop_base.c" "%BTSTACK%\src\btstack_util.c" ^
  "%BTSTACK%\src\btstack_tlv.c" "%BTSTACK%\src\ad_parser.c" "%BTSTACK%\src\hci.c" "%BTSTACK%\src\hci_cmd.c" ^
  "%BTSTACK%\src\hci_event.c" "%BTSTACK%\src\hci_dump.c" "%BTSTACK%\src\l2cap.c" "%BTSTACK%\src\l2cap_signaling.c" ^
  "%BTSTACK%\src\btstack_ring_buffer.c" "%BTSTACK%\src\btstack_linked_queue.c" ^
  "%BTSTACK%\src\classic\sdp_util.c" "%BTSTACK%\src\classic\sdp_server.c" "%BTSTACK%\src\classic\sdp_client.c" ^
  "%BTSTACK%\src\classic\btstack_link_key_db_tlv.c" "%BTSTACK%\src\classic\device_id_server.c" ^
  "%BTSTACK%\src\classic\a2dp.c" "%BTSTACK%\src\classic\a2dp_source.c" ^
  "%BTSTACK%\src\classic\avdtp.c" "%BTSTACK%\src\classic\avdtp_acceptor.c" "%BTSTACK%\src\classic\avdtp_initiator.c" ^
  "%BTSTACK%\src\classic\avdtp_source.c" "%BTSTACK%\src\classic\avdtp_util.c" ^
  "%BTSTACK%\src\classic\avrcp.c" "%BTSTACK%\src\classic\avrcp_controller.c" "%BTSTACK%\src\classic\avrcp_target.c" ^
  "%BTSTACK%\src\classic\btstack_sbc_encoder_bluedroid.c" "%BTSTACK%\src\classic\btstack_sbc_bluedroid.c" ^
  "%BTSTACK%\src\classic\btstack_sbc_plc.c" ^
  "%BTSTACK%\platform\windows\btstack_run_loop_windows.c" "%BTSTACK%\platform\windows\hci_transport_h2_winusb.c" ^
  "%BTSTACK%\platform\windows\hci_dump_windows_stdout.c" "%BTSTACK%\platform\windows\hci_dump_windows_fs.c" "%BTSTACK%\platform\windows\btstack_tlv_windows.c" ^
  /I"%PORT%" /I"%BTSTACK%\src" /I"%BTSTACK%\src\classic" /I"%BTSTACK%\platform\windows" ^
  /I"%BTSTACK%\3rd-party\bluedroid\encoder\include" /I"%BTSTACK%\3rd-party\bluedroid\decoder\include"
if errorlevel 1 goto :fail

cl %CFLAGS% /c /Fo:build\bt\ ^
  "%BTSTACK%\3rd-party\bluedroid\encoder\srce\sbc_analysis.c" "%BTSTACK%\3rd-party\bluedroid\encoder\srce\sbc_dct.c" ^
  "%BTSTACK%\3rd-party\bluedroid\encoder\srce\sbc_dct_coeffs.c" "%BTSTACK%\3rd-party\bluedroid\encoder\srce\sbc_enc_bit_alloc_mono.c" ^
  "%BTSTACK%\3rd-party\bluedroid\encoder\srce\sbc_enc_bit_alloc_ste.c" "%BTSTACK%\3rd-party\bluedroid\encoder\srce\sbc_enc_coeffs.c" ^
  "%BTSTACK%\3rd-party\bluedroid\encoder\srce\sbc_encoder.c" "%BTSTACK%\3rd-party\bluedroid\encoder\srce\sbc_packing.c" ^
  "%BTSTACK%\3rd-party\bluedroid\decoder\srce\alloc.c" "%BTSTACK%\3rd-party\bluedroid\decoder\srce\bitalloc-sbc.c" ^
  "%BTSTACK%\3rd-party\bluedroid\decoder\srce\bitalloc.c" "%BTSTACK%\3rd-party\bluedroid\decoder\srce\bitstream-decode.c" ^
  "%BTSTACK%\3rd-party\bluedroid\decoder\srce\decoder-oina.c" "%BTSTACK%\3rd-party\bluedroid\decoder\srce\decoder-private.c" ^
  "%BTSTACK%\3rd-party\bluedroid\decoder\srce\decoder-sbc.c" "%BTSTACK%\3rd-party\bluedroid\decoder\srce\dequant.c" ^
  "%BTSTACK%\3rd-party\bluedroid\decoder\srce\framing-sbc.c" "%BTSTACK%\3rd-party\bluedroid\decoder\srce\framing.c" ^
  "%BTSTACK%\3rd-party\bluedroid\decoder\srce\oi_codec_version.c" "%BTSTACK%\3rd-party\bluedroid\decoder\srce\synthesis-sbc.c" ^
  "%BTSTACK%\3rd-party\bluedroid\decoder\srce\synthesis-dct8.c" "%BTSTACK%\3rd-party\bluedroid\decoder\srce\synthesis-8-generated.c" ^
  /I"%BTSTACK%\src" /I"%BTSTACK%\3rd-party\bluedroid\encoder\include" /I"%BTSTACK%\3rd-party\bluedroid\decoder\include"
if errorlevel 1 goto :fail

echo --- GUI ---
rem 注：src\btpanel3.cpp 为 WebView2 备用面板（未参与编译）。如需启用：
rem   1) 在下行 sources 中加入 src\btpanel3.cpp
rem   2) include 路径加 /I"g:\multi_blt\webview2\include"
rem   3) link 加 advapi32.lib "g:\multi_blt\webview2\x64\WebView2LoaderStatic.lib"
rem   4) gui.cpp 调用 ShowBtA2dpPanelWV(nullptr)
cl %CFLAGS% /Fo:build\ /Fe:build\DualBlt.exe src\engine.cpp src\gui.cpp src\btpanel.cpp src\btcore.cpp src\btsvc.cpp src\btpanel2.cpp src\loopcap.cpp src\adapter.cpp ^
  "%BTSTACK%\..\imgui\imgui.cpp" "%BTSTACK%\..\imgui\imgui_draw.cpp" "%BTSTACK%\..\imgui\imgui_tables.cpp" "%BTSTACK%\..\imgui\imgui_widgets.cpp" ^
  "%BTSTACK%\..\imgui\backends\imgui_impl_win32.cpp" "%BTSTACK%\..\imgui\backends\imgui_impl_dx11.cpp" ^
  /I"%PORT%" /I"%BTSTACK%\src" /I"%BTSTACK%\src\classic" /I"%BTSTACK%\platform\windows" ^
  /I"%BTSTACK%\3rd-party\bluedroid\encoder\include" /I"%BTSTACK%\3rd-party\bluedroid\decoder\include" ^
  /I"%BTSTACK%\..\imgui" /I"%BTSTACK%\..\imgui\backends" ^
  /link /SUBSYSTEM:WINDOWS build\bt\*.obj winusb.lib setupapi.lib ws2_32.lib d3d11.lib dxgi.lib
if errorlevel 1 goto :fail

echo --- CLI ---
cl %CFLAGS% /Fo:build\ /Fe:build\DualBltCli.exe src\engine.cpp src\main.cpp src\btsvc.cpp ^
  /I"%PORT%" /I"%BTSTACK%\src" /I"%BTSTACK%\src\classic" /I"%BTSTACK%\platform\windows" ^
  /I"%BTSTACK%\3rd-party\bluedroid\encoder\include" /I"%BTSTACK%\3rd-party\bluedroid\decoder\include" ^
  /link /SUBSYSTEM:CONSOLE build\bt\*.obj winusb.lib setupapi.lib ws2_32.lib
if errorlevel 1 goto :fail

echo --- BT radios probe ---
cl %CFLAGS% /Fo:build\ /Fe:build\BtRadios.exe src\btradios.cpp /link /SUBSYSTEM:CONSOLE
if errorlevel 1 goto :fail

echo --- HCI probe ---
cl %CFLAGS% /Fo:build\ /Fe:build\BtHci.exe src\bthci.cpp /link /SUBSYSTEM:CONSOLE
if errorlevel 1 goto :fail

echo --- BCCMD probe ---
cl %CFLAGS% /Fo:build\ /Fe:build\BtBccmd.exe src\btbccmd.cpp /link /SUBSYSTEM:CONSOLE
if errorlevel 1 goto :fail

echo --- HCI init + inquiry ---
cl %CFLAGS% /Fo:build\ /Fe:build\BtScan.exe src\btscan.cpp /link /SUBSYSTEM:CONSOLE
if errorlevel 1 goto :fail

echo --- user-mode BT connect ---
cl %CFLAGS% /Fo:build\ /Fe:build\BtConn.exe src\btcore.cpp src\btconn.cpp /link /SUBSYSTEM:CONSOLE
if errorlevel 1 goto :fail

echo --- L2CAP + AVDTP discovery ---
cl %CFLAGS% /Fo:build\ /Fe:build\BtAvdtp.exe src\btcore.cpp src\btavdtp.cpp /link /SUBSYSTEM:CONSOLE
if errorlevel 1 goto :fail

echo --- btcore self test ---
cl %CFLAGS% /Fo:build\ /Fe:build\BtTest.exe src\btcore.cpp src\bttest.cpp /link /SUBSYSTEM:CONSOLE
if errorlevel 1 goto :fail

echo [ok] build\DualBlt.exe (GUI)  build\DualBltCli.exe (CLI)  build\BtRadios.exe (probe)  build\BtHci.exe (HCI)  build\BtBccmd.exe (BCCMD)  build\BtScan.exe (scan)  build\BtConn.exe (connect)  build\BtAvdtp.exe (avdtp)  build\BtTest.exe (selftest)
endlocal
exit /b 0
:novc
echo [x] vcvars64.bat not found: %VCVARS%
exit /b 1
:vcfail
echo [x] vcvars64.bat failed
exit /b 1
:fail
echo [x] compile failed
exit /b 1
