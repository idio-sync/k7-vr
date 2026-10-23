# Sends commands to the running probe (see src/proxy/control.cpp), e.g.
#   tools\cmd.ps1 'press A' 'wait 2000' 'capture'
# Waits for the previous batch to be picked up, then writes atomically via rename.
param(
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)] [string[]]$Lines,
    [string]$File,   # a script of commands, e.g. tools\scripts\new_game.txt
    [string]$Game = 'C:\Users\Virtual\killer7-vr-game'
)
$ErrorActionPreference = 'Stop'
if ($File) { $Lines = @(Get-Content $File) + @($Lines | Where-Object { $_ }) }
$k7 = Join-Path $Game 'k7vr'
$cmd = Join-Path $k7 'cmd.txt'
$tmp = Join-Path $k7 'cmd.tmp'
$deadline = (Get-Date).AddSeconds(10)
while ((Test-Path $cmd) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 100 }
if (Test-Path $cmd) { throw 'previous commands were not picked up (is the game rendering?)' }
[IO.File]::WriteAllLines($tmp, $Lines, [Text.Encoding]::ASCII)
Move-Item $tmp $cmd -Force
