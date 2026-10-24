# Installs the probe into the test copy of the game and seeds its private profile.
#   -FreshShaderCache  moves Shaders\pipeline.cache aside (once) so every shader goes through D3DCompile
param(
    [string]$Game = 'C:\Users\Virtual\killer7-vr-game',
    [switch]$FreshShaderCache
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$out = Join-Path $repo 'out'

if (-not (Test-Path (Join-Path $Game 'Killer7Win.exe'))) { throw "no Killer7Win.exe in $Game" }
if ($Game -like '*steamapps*') { throw "refusing to install into the Steam copy: $Game" }

Copy-Item (Join-Path $out 'XINPUT1_3.dll') $Game -Force
if (Test-Path (Join-Path $out 'XINPUT1_3.pdb')) { Copy-Item (Join-Path $out 'XINPUT1_3.pdb') $Game -Force }
Copy-Item (Join-Path $out 'openvr_api.dll') $Game -Force   # loaded only with [vr] mode=openvr

$k7 = Join-Path $Game 'k7vr'
$profileDir = Join-Path $k7 'SavedGames\killer7'
# These two folders switch on the test-copy behaviours: Saved Games redirected into k7vr\, and shader dumps.
New-Item -ItemType Directory -Force $profileDir | Out-Null
New-Item -ItemType Directory -Force (Join-Path $k7 'shaders') | Out-Null

# Private killer7.ini: the user's own bindings and volumes, but windowed. (The probe mutes
# the process's audio session instead of zeroing volumes, which may stall movie playback.)
$ini = Join-Path $profileDir 'killer7.ini'
if (-not (Test-Path $ini)) {
    $src = Join-Path $env:USERPROFILE 'Saved Games\killer7\killer7.ini'
    $lines = Get-Content $src
    $lines = $lines -replace '^EnableFullScreen = .*', 'EnableFullScreen = false' `
                    -replace '^WindowSize = .*', 'WindowSize = 1280,720' `
                    -replace '^WindowPos = .*', 'WindowPos = 80,80'
    [IO.File]::WriteAllLines($ini, $lines, [Text.Encoding]::ASCII)
    Write-Output "seeded $ini"
}

$cfg = Join-Path $k7 'k7vr.ini'
if (-not (Test-Path $cfg)) {
    [IO.File]::WriteAllLines($cfg, @('[probe]', 'fake_pad=1'), [Text.Encoding]::ASCII)
}

if ($FreshShaderCache) {
    $cache = Join-Path $Game 'Shaders\pipeline.cache'
    $orig = "$cache.orig"
    if ((Test-Path $cache) -and -not (Test-Path $orig)) { Move-Item $cache $orig; Write-Output 'pipeline.cache moved aside' }
    elseif (Test-Path $cache) { Remove-Item $cache; Write-Output 'pipeline.cache removed (original kept as .orig)' }
}
Write-Output "installed into $Game"
