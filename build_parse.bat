@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\Enterprise\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cl /nologo /EHsc /Fe:build\BtSnoopParse.exe src\btsnoopparse.cpp
