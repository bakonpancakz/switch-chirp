# Loading screens: each PNG (or JPG) in loading/ becomes one H.264 keyframe in
# dist/config/sys-chirp/loading/, which the sysmodule sends while the game shows nothing
# (no game, the HOME menu): a random one each time. Encoded with ffmpeg (libx264):
# 1280x720 (fitted, the rest black), SPS/PPS id 1 so they can't be mistaken for the
# recorder's (id 0), no B-frames, x264's info SEI left out. Only redone when the image
# is newer than its keyframe. ffmpeg: $env:FFMPEG (toolchain.local.ps1), else the PATH.
param([string]$Root = (Split-Path $PSScriptRoot -Parent))

$src = Join-Path $Root "loading"
$out = Join-Path $Root "dist\config\sys-chirp\loading"
if (-not (Test-Path $src)) { exit 0 }
$images = Get-ChildItem $src -File | Where-Object { $_.Extension -match '^\.(png|jpe?g)$' }
if (-not $images) { exit 0 }

$ff = $env:FFMPEG
if (-not $ff) { $cmd = Get-Command ffmpeg -ErrorAction SilentlyContinue; if ($cmd) { $ff = $cmd.Source } }
if (-not $ff -or -not (Test-Path $ff)) {
    Write-Warning "loading screens: ffmpeg not found (set `$env:FFMPEG in toolchain.local.ps1): skipped"
    exit 0
}
New-Item -ItemType Directory -Force $out | Out-Null

# (stale ones, whose image is gone)
Get-ChildItem $out -Filter *.h264 | Where-Object { -not ($images | Where-Object BaseName -eq $_.BaseName) } | Remove-Item

foreach ($img in $images) {
    $dst = Join-Path $out ($img.BaseName + ".h264")
    if ((Test-Path $dst) -and (Get-Item $dst).LastWriteTime -ge $img.LastWriteTime) { continue }
    & $ff -hide_banner -loglevel error -y -i $img.FullName `
        -vf "scale=1280:720:force_original_aspect_ratio=decrease,pad=1280:720:(ow-iw)/2:(oh-ih)/2,format=yuv420p" `
        -frames:v 1 -c:v libx264 -profile:v high -preset veryslow -crf 27 `
        -x264-params "sps-id=1:bframes=0:keyint=1" -bsf:v "filter_units=remove_types=6" -f h264 $dst
    if ($LASTEXITCODE -ne 0) { Write-Warning "loading screens: $($img.Name) didn't encode"; continue }
    $kb = [math]::Round((Get-Item $dst).Length / 1024)
    if ($kb -gt 200) { Write-Warning "loading screens: $($img.Name) is $kb KB (over 200: the sysmodule won't send it)" }
    Write-Host "loading screen: $($img.Name) -> $kb KB"
}
