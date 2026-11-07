# Usage: run.ps1 <Script.java> [args...]   -> prints the script's output (log prefixes stripped)
param([Parameter(Mandatory = $true)][string]$Script, [Parameter(ValueFromRemainingArguments = $true)][string[]]$ScriptArgs)
# Ghidra / JDK locations: set $env:GHIDRA and $env:JAVA_HOME, or put them in paths.local.ps1 next to this script
# (not in git), e.g.  $env:GHIDRA = 'C:\ghidra_12.2_PUBLIC'; $env:JAVA_HOME = 'C:\Program Files\...\jdk'
if (Test-Path "$PSScriptRoot\paths.local.ps1") { . "$PSScriptRoot\paths.local.ps1" }
if (-not $env:GHIDRA -or -not (Test-Path "$env:GHIDRA\support\analyzeHeadless.bat")) { throw 'set $env:GHIDRA to the Ghidra folder (see README)' }
$re = Split-Path $PSScriptRoot      # the re folder
$out = & "$env:GHIDRA\support\analyzeHeadless.bat" "$re\ghidra" Dweep -process Dweep.exe -noanalysis -readOnly `
    -scriptPath "$re\scripts" -postScript $Script @ScriptArgs 2>&1 | ForEach-Object { "$_" }
$on = $false
foreach ($l in $out) {
    if ($l -match "^INFO  $([regex]::Escape($Script))> (.*?)( \(GhidraScript\))?\s*$") { $on = $true; $matches[1]; continue }
    if ($l -match '^(INFO|WARN|ERROR) ') { $on = $false; if ($l -match '^ERROR') { $l }; continue }
    if ($on) { $l -replace ' \(GhidraScript\)\s*$', '' }
}
