@echo off
rem Builds everything into .\out (32-bit, Visual Studio C++ build tools required)
rem   DDraw.dll + DSOUND.dll   Direct2D / XAudio2 replacements, used by the original Dweep.exe
rem   DWGFX.dll                GPU drawing layer, used by Dweep_D2D.exe
rem   Dweep_D2D.exe            made from game\Dweep.exe (only if you put your own copy there)
setlocal
set VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat
if not exist "%VCVARS%" set VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat
if not exist "%VCVARS%" (echo vcvars32.bat not found - edit VCVARS in build.cmd & exit /b 1)
call "%VCVARS%" >nul || exit /b 1
cd /d "%~dp0"
if not exist out mkdir out
if not exist out\obj mkdir out\obj
set CL_OPTS=/nologo /O2 /MT /W3 /LD /EHsc /std:c++17 /Foout\obj\
cl %CL_OPTS% src\proxy\ddraw.cpp /Feout\DDraw.dll /link /DEF:src\proxy\ddraw.def user32.lib gdi32.lib d2d1.lib dxguid.lib advapi32.lib || exit /b 1
cl %CL_OPTS% src\proxy\dsound.cpp /Feout\DSOUND.dll /link /DEF:src\proxy\dsound.def user32.lib xaudio2.lib dxguid.lib || exit /b 1
cl %CL_OPTS% src\gfx\ddemu.cpp src\gfx\gfx.cpp /Feout\DWGFX.dll /link /DEF:src\gfx\dwgfx.def user32.lib gdi32.lib d3d11.lib dxgi.lib d2d1.lib dwrite.lib dxguid.lib winmm.lib advapi32.lib || exit /b 1
copy /y config\DweepD2D.ini out\ >nul
rem Dweep_D2D.exe is optional here: install.cmd makes it from the player's own Dweep.exe
where python >nul 2>nul || (echo note: python not found, Dweep_D2D.exe not made here - install.cmd makes it & goto done)
if exist game\Dweep.exe (
    python tools\patch_exe.py game\Dweep.exe out\Dweep_D2D.exe || exit /b 1
) else (
    echo note: game\Dweep.exe not found, Dweep_D2D.exe not made here - install.cmd makes it
)
:done
del out\*.lib out\*.exp 2>nul
echo BUILD OK
