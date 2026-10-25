@echo off
call "G:\VS2022BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cl /nologo /EHsc /Fe:build\BtSnoopParse.exe src\btsnoopparse.cpp
