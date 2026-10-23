# Tiles screenshots into one contact sheet (4 per row), e.g.
#   tools\sheet.ps1 -Out sheet.png shot_003.png shot_004.png
# Relative names resolve against the game's k7vr folder.
param(
    [Parameter(Mandatory)] [string]$Out,
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)] [string[]]$Files,
    [string]$Game = 'C:\Users\Virtual\killer7-vr-game',
    [int]$TileWidth = 480,
    [int]$Last = 0   # instead of -Files: the newest N shot_*.png
)
Add-Type -AssemblyName System.Drawing
$k7 = Join-Path $Game 'k7vr'
if ($Last -gt 0) { $Files = Get-ChildItem $k7 -Filter 'shot_*.png' | Sort-Object LastWriteTime | Select-Object -Last $Last | ForEach-Object Name }
$resolve = { param($f) if ([IO.Path]::IsPathRooted($f)) { $f } else { Join-Path $k7 $f } }
$imgs = $Files | ForEach-Object { [Drawing.Image]::FromFile((& $resolve $_)) }
$tileH = [int]($TileWidth * $imgs[0].Height / $imgs[0].Width)
$cols = [Math]::Min(4, $imgs.Count)
$rows = [Math]::Ceiling($imgs.Count / $cols)
$sheet = New-Object Drawing.Bitmap ($TileWidth * $cols), ($tileH * $rows)
$g = [Drawing.Graphics]::FromImage($sheet)
$font = New-Object Drawing.Font 'Consolas', 11
for ($i = 0; $i -lt $imgs.Count; $i++) {
    $x = ($i % $cols) * $TileWidth; $y = [Math]::Floor($i / $cols) * $tileH
    $g.DrawImage($imgs[$i], $x, $y, $TileWidth, $tileH)
    $g.DrawString((Split-Path $Files[$i] -Leaf), $font, [Drawing.Brushes]::Yellow, $x + 4, $y + 4)
}
$sheet.Save((& $resolve $Out))
$imgs | ForEach-Object Dispose
$g.Dispose(); $sheet.Dispose()
