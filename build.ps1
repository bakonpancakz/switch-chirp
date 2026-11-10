# Builds Chirp (the sysmodule and its overlay) with devkitPro's bundled msys2
# and lays them out in dist/ as they go on the SD card (copy what's in dist to
# the root of the card).
# Usage: ./build.ps1 [all|sysmodule|clean]
param(
    [string]$Target = "all"
)

# toolchain.local.ps1 (next to build.ps1, not committed) can point at your
# toolchain: $env:DEVKITPRO_WIN = "D:\devkitPro"
$local = Join-Path $PSScriptRoot "toolchain.local.ps1"
if (Test-Path $local) { . $local }
$dkp = $env:DEVKITPRO_WIN
if (-not $dkp) { $dkp = "C:\devkitPro" }
$bash = Join-Path $dkp "msys2\usr\bin\bash.exe"
if (-not (Test-Path $bash)) { throw "devkitPro msys2 not found at $dkp (set DEVKITPRO_WIN)" }

function ToMsys([string]$p) { "/" + $p.Substring(0, 1).ToLower() + ($p.Substring(2) -replace '\\', '/') }
$root = ToMsys $PSScriptRoot
$env:DEVKITPRO_DIR = ToMsys $dkp

function Invoke-Make([string]$dir, [string]$makeArgs) {
    & $bash -lc "source '$root/scripts/devkitpro-env.sh' && cd '$root/$dir' && make -j8 $makeArgs"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

if ($Target -eq "clean") {
    Invoke-Make "sysmodule" "clean"
    Invoke-Make "overlay" "clean"
    if (Test-Path (Join-Path $PSScriptRoot "dist")) { Remove-Item -Recurse -Force (Join-Path $PSScriptRoot "dist") }
    exit 0
}

Invoke-Make "sysmodule" ""

# the sysmodule: starts with every boot (boot2.flag) and connects once token.txt is there
# (copied for the "sysmodule" target too: once it wasn't, and builds that never reached dist got "tested")
$sys = Join-Path $PSScriptRoot "dist\atmosphere\contents\420000000000C4A7"
New-Item -ItemType Directory -Force (Join-Path $sys "flags") | Out-Null
Copy-Item (Join-Path $PSScriptRoot "sysmodule\chirp.nsp") (Join-Path $sys "exefs.nsp")
New-Item -ItemType File -Force (Join-Path $sys "flags\boot2.flag") | Out-Null
# (for sysmodule managers such as Ultrahand's ovl-sysmodules: its name, and that it can be started without a reboot)
Set-Content -Encoding ascii (Join-Path $sys "toolbox.json") '{"name": "Chirp", "tid": "420000000000C4A7", "requires_reboot": false}'
Write-Host "the sysmodule is in $sys ($((Get-Item (Join-Path $sys 'exefs.nsp')).LastWriteTime))"
# the loading screens (loading/*.png, encoded once each: see scripts/loading.ps1)
& (Join-Path $PSScriptRoot "scripts\loading.ps1") -Root $PSScriptRoot
# the call's sounds (sounds/*.wav: joining and leaving, yours and other people's; swap them as you like)
$snd = Join-Path $PSScriptRoot "dist\config\sys-chirp\sounds"
New-Item -ItemType Directory -Force $snd | Out-Null
Copy-Item (Join-Path $PSScriptRoot "sounds\*.wav") $snd
if ($Target -eq "sysmodule") { exit 0 }
Invoke-Make "overlay" ""

# the overlay, for Ultrahand's menu
$ovl = Join-Path $PSScriptRoot "dist\switch\.overlays"
New-Item -ItemType Directory -Force $ovl | Out-Null
Copy-Item (Join-Path $PSScriptRoot "overlay\chirp.ovl") (Join-Path $ovl "chirp.ovl")
Write-Host "the overlay is in $ovl"

# where token.txt goes (not made: it's yours to put there)
New-Item -ItemType Directory -Force (Join-Path $PSScriptRoot "dist\config\sys-chirp") | Out-Null
Copy-Item (Join-Path $PSScriptRoot "extras\token.example.txt") (Join-Path $PSScriptRoot "dist\config\sys-chirp\token.example.txt")
