# Starts or stops the test copy of the game. Its PID goes to k7vr\pid.txt so -Stop only
# ever ends the process this script started.
param(
    [string]$Game = 'C:\Users\Virtual\killer7-vr-game',
    [switch]$Stop
)
$ErrorActionPreference = 'Stop'
$pidFile = Join-Path $Game 'k7vr\pid.txt'

if ($Stop) {
    if (Test-Path $pidFile) {
        $id = [int](Get-Content $pidFile)
        $p = Get-Process -Id $id -ErrorAction SilentlyContinue
        if ($p -and $p.ProcessName -eq 'Killer7Win') {
            Stop-Process -Id $id -Force
            Wait-Process -Id $id -Timeout 60 -ErrorAction SilentlyContinue
            Write-Output "stopped $id"
        }
        else { Write-Output "process $id is not running" }
        Remove-Item $pidFile
    } else { Write-Output 'no pid file' }
    return
}

$running = Get-Process Killer7Win -ErrorAction SilentlyContinue
if ($running) { $running | Wait-Process -Timeout 60 -ErrorAction SilentlyContinue; $running = Get-Process Killer7Win -ErrorAction SilentlyContinue }
if ($running) { throw "Killer7Win is already running (PID $($running.Id -join ', '))" }
Remove-Item (Join-Path $Game 'k7vr\status.txt') -ErrorAction SilentlyContinue
# Shot and capture numbering restarts each run; clear the previous run's files.
Get-ChildItem (Join-Path $Game 'k7vr') -File | Where-Object Name -match '^(shot|capture)_\d+\.(png|txt)$' | Remove-Item
# The probe keeps the window minimized (k7vr.ini minimized=1); start it that way too.
$p = Start-Process (Join-Path $Game 'Killer7Win.exe') -WorkingDirectory $Game -WindowStyle Minimized -PassThru
Set-Content $pidFile $p.Id
Write-Output "started PID $($p.Id)"
