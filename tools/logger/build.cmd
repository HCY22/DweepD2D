@echo off
rem Builds the 32-bit logging proxies DDraw.dll and DSOUND.dll into .\out
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist out mkdir out
cl /nologo /O2 /MT /W3 /LD /D_CRT_SECURE_NO_WARNINGS ddraw_log.cpp /Foout\ /Feout\DDraw.dll /link /DEF:ddraw.def user32.lib dxguid.lib || exit /b 1
cl /nologo /O2 /MT /W3 /LD /D_CRT_SECURE_NO_WARNINGS dsound_log.cpp /Foout\ /Feout\DSOUND.dll /link /DEF:dsound.def user32.lib dxguid.lib || exit /b 1
echo BUILD OK
