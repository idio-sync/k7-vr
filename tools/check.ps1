# Checks what a headset would be handed, without one, and photographs every screen on the way:
# boots the test copy in the simulated headset (sim profile: eye targets resized like a
# headset's, the same copies to the eyes and panels) and plays scripts\tour.txt, reporting each
# `expect` in it. Takes about 10 minutes. Exit code 0 when every check passed.
#   tools\check.ps1              installs out\ first
#   tools\check.ps1 -NoInstall   checks what is installed
#   tools\check.ps1 -Flat        the same tour with VR off: the reference picture of each screen
#                                (no checks), for tools\catalog.py to compare against
#   tools\check.ps1 -Script tour_save   another script in tools\scripts (tour_save: from the saved
#                                game in Harman's Room); its runs are kept in k7vr\tour\<script>\vr|flat
# k7vr.ini is put back as it was afterwards. The run's log and images are kept in
# k7vr\tour\vr\ (or k7vr\tour\flat\); tools\catalog.py builds the screen catalogue from them.
param(
    [string]$Game = 'C:\Users\Virtual\killer7-vr-game',
    [switch]$NoInstall,
    [switch]$Flat,
    [string]$Script = 'tour',
    [int]$TimeoutMinutes = 20
)
$ErrorActionPreference = 'Stop'
$k7 = Join-Path $Game 'k7vr'
$ini = Join-Path $k7 'k7vr.ini'
$log = Join-Path $k7 'k7vr.log'
$sub = if ($Script -eq 'tour') { 'tour\' } else { "tour\$Script\" }
$keep = Join-Path $k7 ($sub + $(if ($Flat) { 'flat' } else { 'vr' }))

if (Get-Process Killer7Win -ErrorAction SilentlyContinue) { throw 'Killer7Win is already running: stop it first (tools\run.ps1 -Stop)' }

$lines = @(Get-Content (Join-Path $PSScriptRoot "scripts\$Script.txt"))
$expected = if ($Flat) { 0 } else { @($lines | Where-Object { $_ -match '^\s*expect\s' }).Count }
$screens = @($lines | Where-Object { $_ -match '^\s*screen\s' }).Count

$savedIni = if (Test-Path $ini) { [IO.File]::ReadAllBytes($ini) } else { $null }
$outcome = 'timed out'
try {
    if (-not $NoInstall) { & (Join-Path $PSScriptRoot 'install.ps1') -Game $Game | Out-Null }
    & (Join-Path $PSScriptRoot 'profile.ps1') $(if ($Flat) { 'test' } else { 'sim' }) -Game $Game | Out-Null
    & (Join-Path $PSScriptRoot 'run.ps1') -Game $Game | Out-Null
    $id = [int](Get-Content (Join-Path $k7 'pid.txt'))
    & (Join-Path $PSScriptRoot 'cmd.ps1') -Game $Game -Lines $lines
    Write-Output "touring $screens screens (PID $id, about 10 minutes)"
    $deadline = (Get-Date).AddMinutes($TimeoutMinutes)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 2
        if (-not (Get-Process -Id $id -ErrorAction SilentlyContinue)) { $outcome = 'the game exited'; break }
        if ((Test-Path $log) -and (Select-String -Path $log -Pattern '\] mark: tour done' -Quiet)) { $outcome = 'done'; break }
    }
    Start-Sleep -Seconds 3  # the last capture's files
}
finally {
    & (Join-Path $PSScriptRoot 'run.ps1') -Game $Game -Stop | Out-Null
    if ($savedIni) { [IO.File]::WriteAllBytes($ini, $savedIni) }
    if (Test-Path $keep) { Remove-Item $keep -Recurse -Force }
    New-Item -ItemType Directory -Force $keep | Out-Null
    if (Test-Path $log) { Copy-Item $log (Join-Path $keep 'k7vr.log') }
    Get-ChildItem $k7 -File | Where-Object Name -match '^(shot_\d+|pair_\d+_\w+|sweep_\d+_L)\.png$' | Move-Item -Destination $keep
}

$shot = @(Select-String -Path (Join-Path $keep 'k7vr.log') -Pattern '\] screen: ').Count
if ($Flat) {
    Write-Output "$shot of $screens screens captured ($outcome); kept in $keep"
    if ($outcome -ne 'done' -or $shot -ne $screens) { exit 1 }
    exit 0
}
# Each result under the screen it was checked for (the next `screen` line).
$results = @(); $pending = @()
foreach ($line in Get-Content (Join-Path $keep 'k7vr.log')) {
    if ($line -match '\] expect (\S+): (pass|FAIL)(?:: (.*?))? \((.*)\)$') {
        $details = if ($Matches[3]) { "$($Matches[3]) ($($Matches[4]))" } else { $Matches[4] }
        $pending += [pscustomobject]@{ Screen = ''; Expect = $Matches[1]; Result = $Matches[2]; Details = $details }
    } elseif ($line -match '\] screen: (.*)$') {
        foreach ($p in $pending) { $p.Screen = $Matches[1].Trim() }
        $results += $pending; $pending = @()
    }
}
$results += $pending
$results | Format-Table -Wrap -AutoSize | Out-String -Width 220 | Write-Output
$passed = @($results | Where-Object Result -eq 'pass').Count
Write-Output "$passed of $expected checks passed, $shot of $screens screens captured ($outcome); kept in $keep"
# Aiming: moving only the hand must leave the world still in the eyes (tools\aimcheck.py, on the
# `<name>, hand ...` screens).
$aimOk = $true
if ($lines | Where-Object { $_ -match '^\s*screen .*, hand .+$' }) {
    $py = if (Get-Command py -ErrorAction SilentlyContinue) { 'py' } else { 'python' }
    & $py (Join-Path $PSScriptRoot 'aimcheck.py') $keep
    $aimOk = $LASTEXITCODE -eq 0
    Write-Output "aim check: $(if ($aimOk) { 'the world stays still when only the hand moves' } else { 'FAILED: the world moves with the hand' })"
}
if ($outcome -ne 'done' -or $passed -ne $expected -or -not $aimOk) { exit 1 }
exit 0
