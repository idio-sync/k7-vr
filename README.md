# killer7-vr

Probe for a VR mod of the Killer7 PC port. Findings so far: [docs/findings.md](docs/findings.md).

The probe is a stand-in `XINPUT1_3.dll` (32-bit) that the game loads from its own folder.
It logs D3D11 frames, dumps the runtime-generated shaders, patches them for stereo, and takes
scripted input so the game can be driven while minimized.

## Use

- `build.cmd` builds `out\XINPUT1_3.dll` (VS 18 Build Tools x86, `C:\ninja.exe`).
- `tools\install.ps1` installs into a **copy** of the game (default `C:\Users\Virtual\killer7-vr-game`,
  refuses Steam folders) and seeds a private profile (Saved Games is redirected into `k7vr\`).
- `tools\run.ps1` / `tools\run.ps1 -Stop` start and stop the copy (minimized, muted).
- `tools\cmd.ps1 'key SPACE' 'wait 2000' 'shot'` or `tools\cmd.ps1 -File tools\scripts\new_game.txt`
  sends commands; the list is at the top of `src\proxy\control.cpp`.
- `tools\sheet.ps1 -Out sheet.png -Last 4` tiles the newest screenshots.

Output lands in `<game>\k7vr\`: `k7vr.log`, `status.txt`, `shot_*.png`, `capture_*.txt/png`
(per-draw lists), `pair_*_L/R.png` (stereo), `shaders\`.
