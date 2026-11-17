# Checks what a headset would be handed, without one: boots the test copy in the simulated
# headset (sim profile: eye targets resized like a headset's, the same copies to the eyes and
# panels), plays new_game.txt and scripts\check_vr.txt, and reports each `expect` in it.
# Takes about 5 minutes. Exit code 0 when every check passed.
#   tools\check.ps1              installs out\ first
#   tools\check.ps1 -NoInstall   checks what is installed
# k7vr.ini is put back as it was afterwards; the run's log is kept as k7vr\check.log.
param(
    [string]$Game = 'C:\Users\Virtual\killer7-vr-game',
    [switch]$NoInstall,
    [int]$TimeoutMinutes = 10
)
$ErrorActionPreference = 'Stop'
$k7 = Join-Path $Game 'k7vr'
$ini = Join-Path $k7 'k7vr.ini'
$log = Join-Path $k7 'k7vr.log'
$scripts = Join-Path $PSScriptRoot 'scripts'

if (Get-Process Killer7Win -ErrorAction SilentlyContinue) { throw 'Killer7Win is already running: stop it first (tools\run.ps1 -Stop)' }

$lines = @('wait 8000', 'mark boot', 'expect 2d 2000', 'wait 2500') +
         @(Get-Content (Join-Path $scripts 'new_game.txt')) +
         @(Get-Content (Join-Path $scripts 'check_vr.txt'))
$expected = @($lines | Where-Object { $_ -match '^\s*expect\s' }).Count

$savedIni = if (Test-Path $ini) { [IO.File]::ReadAllBytes($ini) } else { $null }
$outcome = 'timed out'
try {
    if (-not $NoInstall) { & (Join-Path $PSScriptRoot 'install.ps1') -Game $Game | Out-Null }
    & (Join-Path $PSScriptRoot 'profile.ps1') sim -Game $Game | Out-Null
    & (Join-Path $PSScriptRoot 'run.ps1') -Game $Game | Out-Null
    $id = [int](Get-Content (Join-Path $k7 'pid.txt'))
    & (Join-Path $PSScriptRoot 'cmd.ps1') -Game $Game -Lines $lines
    Write-Output "running the checks (PID $id, about 5 minutes)"
    $deadline = (Get-Date).AddMinutes($TimeoutMinutes)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 2
        if (-not (Get-Process -Id $id -ErrorAction SilentlyContinue)) { $outcome = 'the game exited'; break }
        if ((Test-Path $log) -and (Select-String -Path $log -Pattern '\] mark: check done' -Quiet)) { $outcome = 'done'; break }
    }
}
finally {
    & (Join-Path $PSScriptRoot 'run.ps1') -Game $Game -Stop | Out-Null
    if ($savedIni) { [IO.File]::WriteAllBytes($ini, $savedIni) }
    if (Test-Path $log) { Copy-Item $log (Join-Path $k7 'check.log') -Force }
}

# Each result under the mark before it.
$section = '(start)'
$results = foreach ($line in Get-Content (Join-Path $k7 'check.log')) {
    if ($line -match '\] mark: (.*)$') { $section = $Matches[1].Trim() }
    elseif ($line -match '\] expect (\S+): (pass|FAIL)(?:: (.*?))? \((.*)\)$') {
        $details = if ($Matches[3]) { "$($Matches[3]) ($($Matches[4]))" } else { $Matches[4] }
        [pscustomobject]@{ Section = $section; Expect = $Matches[1]; Result = $Matches[2]; Details = $details }
    }
}
$results | Format-Table -Wrap -AutoSize | Out-String -Width 200 | Write-Output
$passed = @($results | Where-Object Result -eq 'pass').Count
Write-Output "$passed of $expected checks passed ($outcome); log: $(Join-Path $k7 'check.log')"
if ($outcome -ne 'done' -or $passed -ne $expected) { exit 1 }
exit 0
