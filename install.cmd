@echo off
rem DweepD2D installer: double-click it (a folder picker opens), drop the game folder onto it,
rem or run: install.cmd "C:\path\to\Dweep Gold"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %1
echo.
pause
