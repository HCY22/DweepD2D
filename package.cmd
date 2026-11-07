@echo off
rem Builds and packages a release: dist\DweepD2D-VERSION.zip plus dist\SHA256SUMS.txt
rem Usage: package.cmd 1.0
rem The zip holds only this project's own files - never Dweep_D2D.exe or anything else from the game.
setlocal
cd /d "%~dp0"
set VER=%~1
if "%VER%"=="" set VER=dev
call "%~dp0build.cmd" || exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ErrorActionPreference='Stop'; $v='%VER%'; $stage=\"dist\DweepD2D-$v\"; " ^
  "if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }; New-Item -ItemType Directory $stage | Out-Null; " ^
  "Copy-Item out\DDraw.dll,out\DSOUND.dll,out\DWGFX.dll,out\DweepD2D.ini,install.cmd,install.ps1,README.md,LICENSE $stage; " ^
  "$sums = Get-ChildItem $stage -File | Sort-Object Name | ForEach-Object { '{0}  {1}' -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower(), $_.Name }; " ^
  "Set-Content \"$stage\SHA256SUMS.txt\" $sums -Encoding ascii; " ^
  "$zip=\"dist\DweepD2D-$v.zip\"; if (Test-Path $zip) { Remove-Item $zip }; Compress-Archive \"$stage\*\" $zip; " ^
  "$z = Get-FileHash $zip -Algorithm SHA256; Set-Content dist\SHA256SUMS.txt ('{0}  {1}' -f $z.Hash.ToLower(), (Split-Path $zip -Leaf)) -Encoding ascii; " ^
  "Write-Host \"packaged $zip\"; Get-Content \"$stage\SHA256SUMS.txt\"" || exit /b 1
echo PACKAGE OK
