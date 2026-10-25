@echo off
setlocal
set VCVARS=G:\VS2022BuildTools\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" goto :novc
call "%VCVARS%"
if errorlevel 1 goto :vcfail

set BT=g:\multi_blt\third_party\btstack
set PORT=g:\multi_blt\btstack_port
set OUT=d:\btstack_build
if not exist "%OUT%" mkdir "%OUT%"
del /q "%OUT%\*.obj" 2>nul

set INC=/I"%PORT%" /I"%BT%\src" /I"%BT%\src\classic" /I"%BT%\platform\windows" /I"%BT%\3rd-party\rijndael" /I"%BT%\3rd-party\bluedroid\encoder\include" /I"%BT%\3rd-party\bluedroid\decoder\include"
set CFLAGS=/nologo /c /O2 /W2 /utf-8 /D_CRT_SECURE_NO_WARNINGS %INC% /Fo%OUT%\

cd /d "%OUT%"

set SRC="%BT%\src\btstack_linked_list.c" "%BT%\src\btstack_memory.c" "%BT%\src\btstack_memory_pool.c" "%BT%\src\btstack_run_loop.c" "%BT%\src\btstack_run_loop_base.c" "%BT%\src\btstack_util.c" "%BT%\src\btstack_tlv.c" "%BT%\src\ad_parser.c" "%BT%\src\hci.c" "%BT%\src\hci_cmd.c" "%BT%\src\hci_event.c" "%BT%\src\hci_dump.c" "%BT%\src\l2cap.c" "%BT%\src\l2cap_signaling.c" "%BT%\src\btstack_ring_buffer.c" "%BT%\src\btstack_linked_queue.c" "%BT%\src\classic\sdp_util.c" "%BT%\src\classic\sdp_server.c" "%BT%\src\classic\sdp_client.c" "%BT%\src\classic\btstack_link_key_db_tlv.c" "%BT%\src\classic\device_id_server.c" "%BT%\src\classic\a2dp.c" "%BT%\src\classic\a2dp_source.c" "%BT%\src\classic\avdtp.c" "%BT%\src\classic\avdtp_acceptor.c" "%BT%\src\classic\avdtp_initiator.c" "%BT%\src\classic\avdtp_source.c" "%BT%\src\classic\avdtp_util.c" "%BT%\src\classic\btstack_sbc_encoder_bluedroid.c" "%BT%\src\classic\btstack_sbc_bluedroid.c" "%BT%\platform\windows\btstack_run_loop_windows.c" "%BT%\platform\windows\hci_transport_h2_winusb.c" "%BT%\platform\windows\hci_dump_windows_stdout.c" "%BT%\platform\windows\btstack_tlv_windows.c"

set SRCENC="%BT%\3rd-party\bluedroid\encoder\srce\sbc_analysis.c" "%BT%\3rd-party\bluedroid\encoder\srce\sbc_dct.c" "%BT%\3rd-party\bluedroid\encoder\srce\sbc_dct_coeffs.c" "%BT%\3rd-party\bluedroid\encoder\srce\sbc_enc_bit_alloc_mono.c" "%BT%\3rd-party\bluedroid\encoder\srce\sbc_enc_bit_alloc_ste.c" "%BT%\3rd-party\bluedroid\encoder\srce\sbc_enc_coeffs.c" "%BT%\3rd-party\bluedroid\encoder\srce\sbc_encoder.c" "%BT%\3rd-party\bluedroid\encoder\srce\sbc_packing.c" "%BT%\3rd-party\bluedroid\decoder\srce\alloc.c" "%BT%\3rd-party\bluedroid\decoder\srce\bitalloc-sbc.c" "%BT%\3rd-party\bluedroid\decoder\srce\bitalloc.c" "%BT%\3rd-party\bluedroid\decoder\srce\bitstream-decode.c" "%BT%\3rd-party\bluedroid\decoder\srce\decoder-oina.c" "%BT%\3rd-party\bluedroid\decoder\srce\decoder-private.c" "%BT%\3rd-party\bluedroid\decoder\srce\decoder-sbc.c" "%BT%\3rd-party\bluedroid\decoder\srce\dequant.c" "%BT%\3rd-party\bluedroid\decoder\srce\framing-sbc.c" "%BT%\3rd-party\bluedroid\decoder\srce\framing.c" "%BT%\3rd-party\bluedroid\decoder\srce\oi_codec_version.c" "%BT%\3rd-party\bluedroid\decoder\srce\synthesis-sbc.c" "%BT%\3rd-party\bluedroid\decoder\srce\synthesis-dct8.c" "%BT%\3rd-party\bluedroid\decoder\srce\synthesis-8-generated.c" "%BT%\src\classic\btstack_sbc_plc.c"

set FULLSRC=%SRC%
set ARGSRC="%PORT%\a2dp_main.c"

echo --- compiling core ---
cl %CFLAGS% %FULLSRC%
if errorlevel 1 goto :fail

echo --- compiling sbc encoder ---
set CFLAGS_ENC=/nologo /c /O2 /W2 /utf-8 /D_CRT_SECURE_NO_WARNINGS /I"%PORT%" /I"%BT%\src" /I"%BT%\3rd-party\bluedroid\encoder\include" /I"%BT%\3rd-party\bluedroid\decoder\include" /Fo%OUT%\
cl %CFLAGS_ENC% %SRCENC%
if errorlevel 1 goto :fail

echo --- compiling app ---
cl %CFLAGS% /I"%BT%\3rd-party\bluedroid\encoder\include" %ARGSRC%
if errorlevel 1 goto :fail

echo --- linking ---
link /nologo /OUT:"%OUT%\BtA2dp.exe" /SUBSYSTEM:CONSOLE "%OUT%\*.obj" winusb.lib setupapi.lib ws2_32.lib
if errorlevel 1 goto :fail

echo [ok] %OUT%\BtA2dp.exe
endlocal
exit /b 0
:novc
echo [x] vcvars64.bat not found
exit /b 1
:vcfail
echo [x] vcvars64.bat failed
exit /b 1
:fail
echo [x] btstack build failed
exit /b 1
