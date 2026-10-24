# Installs the Killer7 VR mod into a Killer7 PC install, or removes it again.
#
#   powershell -File tools\install_game.ps1 -Game "D:\Steam\steamapps\common\killer7"
#   powershell -File tools\install_game.ps1 -Game "D:\Steam\steamapps\common\killer7" -Uninstall [-Purge]
#
# Adds to the game folder: XINPUT1_3.dll (the mod), openvr_api.dll (SteamVR's API) and k7vr\
# (settings k7vr.ini, log, the VR copy of your key bindings). Nothing else in the game folder or
# your saves is changed. A different XINPUT1_3.dll / openvr_api.dll already there is kept as
# *.k7vr-backup and put back on uninstall. -Purge also deletes k7vr\ (settings and log).
#
# Play: start SteamVR, then the game as usual. Holding Shift while the game starts plays flat
# (safe mode). [vr] mode=off in k7vr\k7vr.ini turns VR off for good.
param(
    [Parameter(Mandatory)] [string]$Game,
    [switch]$Uninstall,
    [switch]$Purge,
    [string]$From   # where the built DLLs are (default: the repo's out\)
)
$ErrorActionPreference = 'Stop'
if (-not $From) { $From = Join-Path (Split-Path $PSScriptRoot -Parent) 'out' }
if (-not (Test-Path -LiteralPath (Join-Path $Game 'Killer7Win.exe'))) { throw "no Killer7Win.exe in $Game" }
if (Get-Process Killer7Win -ErrorAction SilentlyContinue) { throw 'close the game first' }

$k7 = Join-Path $Game 'k7vr'
$record = Join-Path $k7 'installed.txt'
# Exact case matters: some Steam library drives are case-sensitive.
$files = @('XINPUT1_3.dll', 'openvr_api.dll')

function Test-Ours([string]$path) {
    # The mod's DLL names its log file (UTF-16) in its strings.
    $bytes = [IO.File]::ReadAllBytes($path)
    $text = [Text.Encoding]::Unicode.GetString($bytes)
    return $text.Contains('k7vr.log')
}

if ($Uninstall) {
    foreach ($f in $files) {
        $path = Join-Path $Game $f
        $backup = "$path.k7vr-backup"
        $ours = (Test-Path -LiteralPath $path) -and (($f -eq 'openvr_api.dll' -and (Test-Path -LiteralPath $record)) -or
                                                     ($f -eq 'XINPUT1_3.dll' -and (Test-Ours $path)))
        if ($ours) { Remove-Item -LiteralPath $path; Write-Output "removed $f" }
        if (Test-Path -LiteralPath $backup) { Move-Item -LiteralPath $backup $path -Force; Write-Output "restored the original $f" }
    }
    Remove-Item -LiteralPath (Join-Path $Game 'XINPUT1_3.pdb') -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $record -ErrorAction SilentlyContinue
    if ($Purge -and (Test-Path -LiteralPath $k7)) { Remove-Item -LiteralPath $k7 -Recurse; Write-Output 'removed k7vr\' }
    Write-Output "uninstalled from $Game"
    return
}

foreach ($f in $files) {
    if (-not (Test-Path -LiteralPath (Join-Path $From $f))) { throw "missing $f in $From (build first: build.cmd)" }
}
New-Item -ItemType Directory -Force $k7 | Out-Null
foreach ($f in $files) {
    $path = Join-Path $Game $f
    $backup = "$path.k7vr-backup"
    if ((Test-Path -LiteralPath $path) -and -not (Test-Path -LiteralPath $backup)) {
        $same = (Get-FileHash -LiteralPath $path).Hash -eq (Get-FileHash -LiteralPath (Join-Path $From $f)).Hash
        $mine = $f -eq 'XINPUT1_3.dll' -and (Test-Ours $path)
        if (-not $same -and -not $mine) { Copy-Item -LiteralPath $path $backup; Write-Output "kept the existing $f as $f.k7vr-backup" }
    }
    Copy-Item -LiteralPath (Join-Path $From $f) $path -Force
    Write-Output "installed $f"
}
Set-Content -LiteralPath $record -Value ($files -join "`r`n") -Encoding ascii

$cfg = Join-Path $k7 'k7vr.ini'
if (-not (Test-Path -LiteralPath $cfg)) {
    $lines = @(
        '; Killer7 VR settings. Delete this file to get the defaults back.',
        '[vr]',
        '; openvr = VR through SteamVR, off = play flat (Shift held at start also plays flat)',
        'mode=openvr',
        '; metres per game unit (world size)',
        'unit_m=0.25',
        '; eye image size: auto = SteamVR''s recommended size times eye_scale (try 0.8 if it stutters)',
        'eye_res=auto',
        'eye_scale=1.0',
        '; aim mode: degrees the aim is tilted up from the controller, grip offset (metres, x right y up z back)',
        'hand_pitch=0',
        'hand_offset=0,0,0',
        'gun_scale=0.6',
        'aim_converge_m=10',
        'laser_m=30',
        '[probe]',
        '; lets the VR controllers reach the game when its window is not in front',
        'auto_activate=1'
    )
    [IO.File]::WriteAllLines($cfg, $lines, [Text.Encoding]::ASCII)
    Write-Output "wrote default settings to $cfg"
}
Write-Output "installed into $Game"
Write-Output 'In VR the game uses its own copy of your key bindings (k7vr\killer7.ini); your saves and killer7.ini are untouched.'
