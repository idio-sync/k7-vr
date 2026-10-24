# Switches the test copy's k7vr.ini between the unattended test setup and a headset playtest.
#   tools\profile.ps1 test       minimized, muted, scripted input, fake cursor, VR off (default for automation)
#   tools\profile.ps1 playtest   normal window, sound on, real mouse/focus, OpenVR on (starts SteamVR)
#   tools\profile.ps1 sim        like test, but the simulated headset is on from boot
param(
    [Parameter(Mandatory)] [ValidateSet('test', 'playtest', 'sim')] [string]$Profile,
    [string]$Game = 'C:\Users\Virtual\killer7-vr-game'
)
$ErrorActionPreference = 'Stop'
$test = $Profile -ne 'playtest'
$b = { param($v) if ($v) { '1' } else { '0' } }
$vrMode = switch ($Profile) { 'playtest' { 'openvr' } 'sim' { 'sim' } default { 'off' } }
$lines = @(
    '[probe]',
    "fake_pad=$(& $b $test)",
    "minimized=$(& $b $test)",
    "leave_desktop_alone=$(& $b $test)",
    "auto_activate=$(& $b $test)",
    "mute=$(& $b $test)",
    'log_files=0',
    '',
    '[vr]',
    "mode=$vrMode",
    'unit_m=0.25',
    'ipd_m=0.064',
    'sim_fov=100',
    'fov_inflate=150'
)
$ini = Join-Path $Game 'k7vr\k7vr.ini'
[IO.File]::WriteAllLines($ini, $lines, [Text.Encoding]::ASCII)
Write-Output "k7vr.ini set to '$Profile'"
