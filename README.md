# killer7-vr

A VR mod for the Killer7 PC port (2018/2024 Steam release), through SteamVR: the game in stereo
with head tracking, a HUD panel, the controllers as the gamepad, and first-person aiming with the
right controller (Dan's own gun in your hand, a laser sight).

Status: works in a simulated headset; the first real-headset session (2026-10-23) was before
controller aiming and the sharpness fix. v1 targets the first sections of Target 00 (Angel).
Design: [docs/superpowers/specs/2026-10-23-killer7-vr-design.md](docs/superpowers/specs/2026-10-23-killer7-vr-design.md).
Findings: [docs/findings.md](docs/findings.md). Playtest scripts: [docs/playtests/](docs/playtests/).

## Install into your game

1. `build.cmd` (VS 18 Build Tools x86, `C:\ninja.exe`) builds `out\XINPUT1_3.dll`.
2. Close the game, then
   `powershell -File tools\install_game.ps1 -Game "<your killer7 folder>"`.
   It adds `XINPUT1_3.dll`, `openvr_api.dll` and a `k7vr\` folder (settings in `k7vr\k7vr.ini`,
   log in `k7vr\k7vr.log`). It changes nothing else: not your saves, not your `killer7.ini`, not
   the game's shader cache.
3. Start SteamVR, then the game as usual. Sit facing forward; hold both stick clicks for a second
   to recentre.

- **Play flat:** hold Shift while the game starts (safe mode), or set `mode=off` in `k7vr\k7vr.ini`.
  If SteamVR can't start, the game also runs flat.
- **Key bindings in VR:** the controllers press keyboard keys, so in VR the game reads its
  settings from `k7vr\killer7.ini`, a copy of your `killer7.ini` (refreshed whenever yours
  changes) with the keyboard bindings the controllers need. Your own file is only read.
- **Uninstall:** `powershell -File tools\install_game.ps1 -Game "<folder>" -Uninstall` (`-Purge`
  also deletes `k7vr\`).
- Settings are listed at the top of `src\vr\vr.h` and in [docs/playtests/m4.md](docs/playtests/m4.md).

Controllers (index-style): right trigger fire, right grip hold = aim mode (then point the right
controller), A accept, B skip/decline, right stick aim/menus, right stick click reload; left
trigger walk, left grip scan, X special, Y map, left stick up walk / down turn / left-right
choose, left stick click menu.

## Development (the test copy)

All automated runs use a **copy** of the game (`C:\Users\Virtual\killer7-vr-game`), never the
Steam install. The copy has `k7vr\SavedGames\` (its own saves and `killer7.ini`; the mod redirects
Saved Games there whenever that folder exists) and `k7vr\shaders\` (shader dumps, written only
when that folder exists).

- `tools\install.ps1` installs into the copy (refuses Steam folders) and seeds its private profile.
- `tools\profile.ps1 test|playtest|sim` switches the copy's `k7vr.ini`: unattended (minimized,
  muted, scripted input, VR off), headset playtest, or the simulated headset.
- `tools\run.ps1` / `-Stop` / `-Normal` start and stop the copy.
- `tools\cmd.ps1 'key SPACE' 'wait 2000' 'shot'` or `tools\cmd.ps1 -File tools\scripts\new_game.txt`
  sends commands; the list is at the top of `src\proxy\control.cpp` (input, screenshots, stereo
  pairs, per-draw captures with `capture N...`, the simulated hand, live aim tuning, memory probes).
- `tools\sheet.ps1 -Out sheet.png -Last 4` tiles the newest screenshots.
- Unit tests: `ctest --test-dir out` (head/aim math, the VR profile's ini rewrite).

Output lands in `<game>\k7vr\`: `k7vr.log`, `status.txt`, `shot_*.png`, `capture_*` (per-draw
lists and snapshots), `pair_*` (both eyes as sent to the headset, the raw scene, HUD, screen).
