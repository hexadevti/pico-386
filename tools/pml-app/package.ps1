# package.ps1 -- build the PicoMultiLoader app folder: pico-386/ with both firmwares, app.ini and
# the icon, zipped for the root of the SD card (PicoMultiLoader docs/apps.md).
#
#   powershell -File tools/pml-app/package.ps1 -Version 0.1.0               # build both, package
#   powershell -File tools/pml-app/package.ps1 -Version 0.1.0 -SkipBuild    # package what is built
#
# Output: build-pml/pico-386/ and build-pml/pico-386-pml-v<ver>.zip. The folder is pico-386, never
# pc-386: that is the emulator's data folder, and FAT ignores case.
#
# The version goes into app.ini and, on the RP2350, into the UF2's binary_info (binfo_rp2350.c),
# which the loader's details screen reads. The P4 .bin's app descriptor version comes from the
# precompiled Arduino core, so the details screen shows the core's version there.
param(
  [Parameter(Mandatory)] [string]$Version,
  [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$Version = $Version.TrimStart('v')
$repo   = (Resolve-Path "$PSScriptRoot\..\..").Path
$sketch = Join-Path $repo 'pico386'
$out    = Join-Path $repo 'build-pml'
$rpOut  = Join-Path $out 'rp2350'
$p4Out  = Join-Path $out 'p4'

# The same boards and flags as the VS Code tasks (.vscode/tasks.json).
$RP_FQBN = 'rp2040:rp2040:rpipico2:arch=arm,os=freertos,freq=300,flash=4194304_65536,usbstack=picosdk,opt=Optimize2,exceptions=Enabled'
$P4_FQBN = 'esp32:esp32:esp32p4:PSRAM=enabled,FlashSize=16M,ChipVariant=postv3,PartitionScheme=huge_app,DebugLevel=error'

function Invoke-Compile([string[]]$ArgList, [hashtable]$Env = @{}) {
  $saved = @{}
  foreach ($k in $Env.Keys) { $saved[$k] = [Environment]::GetEnvironmentVariable($k); [Environment]::SetEnvironmentVariable($k, $Env[$k]) }
  try {
    # Warnings go to stderr, which Windows PowerShell 5.1 turns into terminating errors under 'Stop'
    # once the output is redirected; the exit code is what says whether it failed (as in emu8).
    $ErrorActionPreference = 'Continue'
    & arduino-cli compile @ArgList $sketch 2>&1 | ForEach-Object { "$_" }
    $ErrorActionPreference = 'Stop'
    if ($LASTEXITCODE) { throw "arduino-cli compile failed" }
  } finally {
    foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
  }
}

if (-not $SkipBuild) {
  $rpFlags = "-DBOARD_PICOCALC_RP2350 -DPICO386_VERSION=$Version -I{build.source.path}/src/hal/picocalc_rp2350/include"
  Write-Host "== RP2350"
  Invoke-Compile @('--fqbn', $RP_FQBN,
    '--build-property', "compiler.cpp.extra_flags=$rpFlags",
    '--build-property', "compiler.c.extra_flags=$rpFlags",
    '--build-path', "$HOME\.pico386-rp2350-build", '--output-dir', $rpOut)
  Write-Host "== ESP32-P4"
  Invoke-Compile @('--fqbn', $P4_FQBN,
    '--build-property', 'compiler.optimization_flags=-O2',
    '--build-property', 'compiler.cpp.extra_flags=-DBOARD_PICOCALC_P4',
    '--build-path', "$HOME\.pico386-p4-build", '--output-dir', $p4Out) `
    @{ ARDUINO_DIRECTORIES_DATA = "$HOME\.emu6502-p4\data"; ARDUINO_DIRECTORIES_USER = "$HOME\.emu6502-p4\user" }
}

$pml = Join-Path $out 'pico-386'
if (Test-Path $pml) { Remove-Item -Recurse -Force $pml }
New-Item -ItemType Directory -Force $pml | Out-Null
Copy-Item (Join-Path $rpOut 'pico386.ino.uf2') (Join-Path $pml 'pico386-rp2350.uf2')
Copy-Item (Join-Path $p4Out 'pico386.ino.bin') (Join-Path $pml 'pico386-p4.bin')   # the app alone, not .merged
$ini = (Get-Content -Raw (Join-Path $PSScriptRoot 'app.ini')) -replace '(?m)^(version\s*=\s*).*$', "`${1}$Version"
[IO.File]::WriteAllText((Join-Path $pml 'app.ini'), $ini)   # ASCII, no BOM: the loader reads it raw
Copy-Item (Join-Path $PSScriptRoot 'icon.bmp') $pml

$zip = Join-Path $out "pico-386-pml-v$Version.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
# Windows' bsdtar, not Compress-Archive: PowerShell 5.1 writes backslashes into entry names.
& "$env:SystemRoot\System32\tar.exe" -a -cf $zip -C $out pico-386
if ($LASTEXITCODE) { throw "zip failed: $zip" }
Write-Host "-> $pml"
Write-Host "-> $zip"
