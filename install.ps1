# DweepD2D installer. Run it through install.cmd (or: powershell -ExecutionPolicy Bypass -File install.ps1 "<game folder>").
#  - copies DDraw.dll, DSOUND.dll, DWGFX.dll (and DweepD2D.ini if you have none yet) into the game folder
#  - makes Dweep_D2D.exe from YOUR Dweep.exe: a copy whose DirectDraw import "DDRAW.dll" is renamed to "DWGFX.dll"
# Dweep.exe itself is never modified. Your settings (DweepD2D.ini) and progress (DweepSave.ini) are kept.
param([string]$GameDir)
$ErrorActionPreference = 'Stop'

# SHA-256 of the Dweep Gold 1.40 Dweep.exe this was developed and tested with
$KnownExe = '7DD4A22D90DF81261F892B7BE1F9A2F1C0119B9E5DBC686695C2D31531C15DFB'

function Fail($msg) { Write-Host "ERROR: $msg" -ForegroundColor Red; exit 1 }

# the built files: next to this script (release zip) or in .\out (source tree)
$src = $PSScriptRoot
if (-not (Test-Path "$src\DWGFX.dll") -and (Test-Path "$src\out\DWGFX.dll")) { $src = "$PSScriptRoot\out" }
foreach ($f in 'DDraw.dll', 'DSOUND.dll', 'DWGFX.dll') {
    if (-not (Test-Path "$src\$f")) { Fail "$f not found next to install.ps1 (run build.cmd first when installing from source)" }
}

if (-not $GameDir) {
    Add-Type -AssemblyName System.Windows.Forms
    $dlg = New-Object System.Windows.Forms.FolderBrowserDialog
    $dlg.Description = 'Select the Dweep Gold folder (the one that contains Dweep.exe)'
    if ($dlg.ShowDialog() -ne 'OK') { Write-Host 'cancelled'; exit 1 }
    $GameDir = $dlg.SelectedPath
}
$GameDir = $GameDir.TrimEnd('\', '"')
$exe = Join-Path $GameDir 'Dweep.exe'
if (-not (Test-Path $exe)) { Fail "Dweep.exe not found in `"$GameDir`"" }
if (Get-Process Dweep, Dweep_D2D -ErrorAction SilentlyContinue) { Fail 'Dweep is running - close the game first' }

# ---- check the game version
$ver = (Get-Item $exe).VersionInfo.FileVersion
$hash = (Get-FileHash $exe -Algorithm SHA256).Hash
if ($hash -eq $KnownExe) { Write-Host 'Dweep.exe: Dweep Gold 1.40 (verified)' }
elseif ($ver -match '^1\.40') { Write-Host "Dweep.exe: version $ver, but not the tested build - the GPU layer checks the code itself and turns off if it does not match" -ForegroundColor Yellow }
else { Write-Host "Dweep.exe: version '$ver' is not 1.40 - DDraw.dll/DSOUND.dll may still work, the GPU layer (Dweep_D2D.exe) will most likely turn itself off" -ForegroundColor Yellow }

# ---- copy the DLLs and the settings template
foreach ($f in 'DDraw.dll', 'DSOUND.dll', 'DWGFX.dll') { Copy-Item "$src\$f" $GameDir -Force; Write-Host "copied $f" }
$ini = if (Test-Path "$src\DweepD2D.ini") { "$src\DweepD2D.ini" } else { "$PSScriptRoot\config\DweepD2D.ini" }
if (-not (Test-Path "$GameDir\DweepD2D.ini")) { Copy-Item $ini $GameDir; Write-Host 'copied DweepD2D.ini' }
else { Write-Host 'kept your DweepD2D.ini' }

# ---- make Dweep_D2D.exe: rename the DDRAW.dll import (same length, nothing else changes)
$d = [IO.File]::ReadAllBytes($exe)
function U16($o) { [BitConverter]::ToUInt16($d, $o) }
function U32($o) { [BitConverter]::ToUInt32($d, $o) }
$pe = U32 0x3C
if ((U32 $pe) -ne 0x4550) { Fail 'Dweep.exe is not a valid PE file' }
$nsec = U16 ($pe + 6); $optsz = U16 ($pe + 20); $opt = $pe + 24
$secs = foreach ($i in 0..($nsec - 1)) {
    $o = $opt + $optsz + 40 * $i
    [pscustomobject]@{ va = U32 ($o + 12); size = [Math]::Max((U32 ($o + 8)), (U32 ($o + 16))); raw = U32 ($o + 20) }
}
function Off($rva) {
    foreach ($s in $secs) { if ($rva -ge $s.va -and $rva -lt $s.va + $s.size) { return $rva - $s.va + $s.raw } }
    Fail ('address {0:X} is outside every section' -f $rva)
}
$o = Off (U32 ($opt + 104))         # import directory
$done = $false
while ($true) {
    $nameRva = U32 ($o + 12)
    if ($nameRva -eq 0) { break }
    $no = Off $nameRva
    $name = [Text.Encoding]::ASCII.GetString($d, $no, 9)
    if ($name -ieq 'DDRAW.dll' -and $d[$no + 9] -eq 0) {
        [Text.Encoding]::ASCII.GetBytes('DWGFX.dll').CopyTo($d, $no)
        $done = $true
    }
    $o += 20
}
if (-not $done) { Fail 'Dweep.exe does not import DDRAW.dll - unsupported version' }
[IO.File]::WriteAllBytes((Join-Path $GameDir 'Dweep_D2D.exe'), $d)
Write-Host 'made Dweep_D2D.exe (from your Dweep.exe; Dweep.exe itself is unchanged)'

Write-Host ''
Write-Host 'Done. Start Dweep.exe (Direct2D/XAudio2) or Dweep_D2D.exe (GPU drawing layer).' -ForegroundColor Green
Write-Host 'Settings: DweepD2D.ini   Progress: DweepSave.ini   Log: DweepD2D_log.txt'
