# Development

Building the mod, the test copy of the game and the harness that drives it, the full settings
list, and the checklist for headset playtests. For what the mod is and how to play with it, see
the [README](../README.md); for where the work stands and what's next, see [status.md](status.md).

## Where things are

- `src\proxy\`: the stand-in `XINPUT1_3.dll`: Direct3D 11 and shader-compile hooks
  (`hooks_d3d.cpp`, `hooks_compile.cpp`), input (`hooks_input.cpp`, `xinput.cpp`), the game
  fixes for VR (`aim.cpp`, `fov_inflate.cpp`), the opt-in VR copy of `killer7.ini` with keyboard
  bindings (`vr_profile.cpp`, `ini_bindings.cpp`; off unless `vr_profile=1`), and the command file the harness talks to (`control.cpp`).
- `src\vr\`: the headset side: OpenVR or the simulated headset, the eyes, the HUD and screen
  panels (`vr.cpp`), the controllers as a gamepad (`input.cpp`), head and aim math
  (`headmath.cpp`). `vr.h` documents the main `[vr]` settings.
- `tools\`: install, run and test scripts (below), plus `disasm.py` and `xref.py` for reading
  `Killer7Win.exe`'s code.
- `tools\scripts\`: command scripts for `tools\cmd.ps1` and `tools\check.ps1` (`tour.txt`,
  `tour_save.txt`, `new_game.txt`, ...).
- `tests\`: unit tests (C++ for the head/aim math and the ini rewrite; Python for the image
  checks).
- `third_party\openvr\`: the OpenVR SDK (32-bit, 2.15.6).
- [status.md](status.md): the current state, how 2D screens are routed, open items, next steps,
  and the harness gotchas.
- [screens.md](screens.md): per-screen status and notes for the tour, shown in the screen
  catalogue.

The game addresses the mod relies on are documented where they're used (`aim.cpp`,
`fov_inflate.cpp`, `hooks_d3d.cpp`, `vr_profile.cpp`).

## Building

`build.cmd` configures and builds the 32-bit DLL into `out\` with MSVC (Visual Studio 18 Build
Tools, x86, through `vcvars32.bat`), CMake and Ninja (expected at `C:\ninja.exe`). It produces
`out\XINPUT1_3.dll` and copies `openvr_api.dll` next to it. The OpenVR import is delay-loaded,
so `openvr_api.dll` is only loaded when VR is wanted (`[vr] mode=auto` asks it whether a headset
is connected; `mode=off` never loads it). The version (`git describe` at configure time) is the
first line of `k7vr.log`; reconfigure (delete `out\`) to refresh it in local builds.

Unit tests: `ctest --test-dir out` (head/aim math, the VR profile's ini rewrite) and
`python -m unittest discover -s tests -p "test_*.py"` (the image checks; needs numpy and
Pillow, as do `tools\catalog.py` and `tools\imagechecks.py`).

To try a build in your own game folder, copy `out\XINPUT1_3.dll` and `out\openvr_api.dll` next
to `Killer7Win.exe` (exact file-name case: some Steam library drives are case-sensitive).

## Releases

`.github/workflows/build.yml` builds the DLL on every push and pull request (MSVC x86, CMake,
Ninja), runs the unit tests and uploads the zip as a build artifact. Pushing a tag `vX.Y.Z` does
the same and publishes a GitHub Release with the zip and its SHA-256 (a pre-release while the
version is 0.x). The zip extracts into the game folder: `XINPUT1_3.dll`, `openvr_api.dll`, and
`k7vr\` with `README.txt`, `LICENSE.txt` (the mod's, MIT) and `LICENSE-openvr.txt`.

Before tagging (CI can't run the game): both tours pass (`tools\check.ps1` and
`tools\check.ps1 -Script tour_save`), a short headset session (menus, a junction, aiming,
rumble), the README's known issues are current. Then `git tag v0.1.0` and `git push origin
v0.1.0`.

## The test copy

All automated runs use a **copy** of the game (`C:\Users\Virtual\killer7-vr-game`, the default
`-Game` of every script), never the Steam install. The copy has `k7vr\SavedGames\` (its own
saves and `killer7.ini`; the mod redirects Saved Games there whenever that folder exists) and
`k7vr\shaders\` (shader dumps, written only when that folder exists).

Running the game overwrites `k7vr\k7vr.log` and deletes the previous run's `pair_*`, `shot_*`,
`capture_*` and `sweep_*` files: copy what you need first. Launch the copy only through
`tools\run.ps1`, and check that no other `Killer7Win` is running first.

## The harness

- `tools\install.ps1` installs `out\` into the copy (refuses Steam folders) and seeds its
  private profile. `-FreshShaderCache` moves `Shaders\pipeline.cache` aside once.
- `tools\profile.ps1 test|playtest|sim` switches the copy's `k7vr.ini`: unattended (minimized,
  muted, scripted input, VR off), headset playtest (normal window, sound, OpenVR), or the
  simulated headset (its eye targets resized like a headset's, 2048x2048 by default, `-EyeRes`,
  so the copies to the eyes and panels run as in a headset). It rewrites the whole file.
- `tools\run.ps1` / `-Stop` / `-Normal` start and stop the copy. `-Stop` only ends the process
  `run.ps1` started (its PID is in `k7vr\pid.txt`); `-Normal` starts with a normal window for
  headset playtests.
- `tools\cmd.ps1 'key SPACE' 'wait 2000' 'shot'` or `tools\cmd.ps1 -File tools\scripts\new_game.txt`
  sends commands to the running game. The list is at the top of `src\proxy\control.cpp`: input
  (keys, mouse, pad), screenshots, stereo pairs, per-draw captures with `capture N...`, the
  simulated head and hand, live aim tuning, `expect` checks, `sweep`, memory probes.
- `tools\sheet.ps1 -Out sheet.png -Last 4` tiles the newest screenshots.

### check.ps1: the screen tour

`tools\check.ps1` tours the game's screens in the simulated headset (about 10 minutes, exit
code 1 on a failure), following `tools\scripts\tour.txt`: boot and title screens, the opening,
Garcian's corridor (LT view, aim, scan, camera, map, quit prompt), the pause menu and its pages,
the cutscene into Dan Smith's lobby, the persona list, a persona change and a junction choice.
After the `expect` results it runs `tools\aimcheck.py` on the `<name>, hand ...` screens: while
aiming, with the simulated head still and only the hand moved (`hand <yaw> <pitch>`), no more
than 15% of the eye may change (the gun alone is a few percent; a world that moves with the hand
is most of it). Both tours have such screens (Garcian in the corridor, Kaede in the lobby).

- Each `expect 3d|menu|2d` in the script checks which way the frames went, that the hand-off
  didn't keep making textures, and that what the eyes or the screen panel got isn't black and
  matches its source.
- Each `screen NAME` saves what the headset is handed.
- Each `sweep` after a 3D screen looks around it (8 head directions: the views the original
  never shows).

Options:

- `-Flat` runs the same tour with VR off, for the reference picture of each screen (no checks).
- `-NoInstall` checks what is already installed instead of installing `out\` first.
- `-Script tour_save` runs another script from `tools\scripts\`. `tour_save` (about 3 minutes)
  loads the saved game in Harman's Room and visits the TV's channels, the room's junction and
  back half, the lobby and its junction, and the Elevator Hall. The save and how to restore it
  are described at the top of `tools\scripts\tour_save.txt` and in [status.md](status.md).

`k7vr.ini` is put back as it was afterwards. The run's log and images are kept in
`k7vr\tour\vr\` (or `k7vr\tour\flat\`); other scripts' runs go to `k7vr\tour\<script>\vr` (or
`flat`).

A full check is `tools\check.ps1`, then `tools\check.ps1 -Flat -NoInstall`, then
`python tools\catalog.py`. Run it before a headset session; it can't judge comfort, scale, aim
feel or the real controllers' bindings.

### catalog.py: the screen catalogue

`python tools\catalog.py [--game DIR] [--out DIR]` builds the screen catalogue
(`<game>\k7vr\tour\catalog\index.html` by default) from the last two tours. Per screen: an
approximate headset view (the left eye with the HUD and screen panels drawn where they float),
the VR-off reference, the raw images, the checks, the look-around strip, and the status and
notes from [screens.md](screens.md) (each `## name` there must match a `screen` line in the
tour).

It flags 2D and menu screens that differ from VR off, and 3D screens whose look-around shows
parts fixed to the view, something in one eye only, or scenery dropped when turning. Where a set
simply ends isn't detected: that's for people to spot in the strip. 3D screens are also compared
with the game's own view of the same frame (cutscenes within their letterbox band) and flagged
over 8%.

### Output files

Output lands in `<game>\k7vr\`: `k7vr.log`, `status.txt`, `shot_*.png`, `capture_*` (per-draw
lists and snapshots), `pair_*` (both eyes as sent to the headset, the raw scene, HUD, screen,
the panels' own copies, the game's own view), `sweep_*`, `tour\vr\` and `tour\flat\` (the last
`tools\check.ps1` runs: log and captures), `tour\catalog\` (the screen catalogue).

## Settings

Under `[vr]` in `k7vr\k7vr.ini`. The installer writes the common ones; any other can be added.
The "default" column is the mod's own value when a key is missing.

| key | default | what |
|---|---|---|
| `mode` | auto | `auto` (OpenVR when `VR_IsHmdPresent` says a headset is connected, else flat), `openvr` (always VR through SteamVR), `sim` (the simulated headset, for testing) or `off` (flat). Shift held at start, or SteamVR failing to start, also means off |
| `unit_m` | 0.25 | metres per game unit (world size) |
| `ipd_m` | 0.064 | the simulated headset's eye separation (OpenVR uses the headset's) |
| `sim_fov` | 100 | the simulated headset's vertical field of view, degrees |
| `fov_inflate` | 150 | field of view the game culls with while stereo is on, degrees (0 = off) |
| `eye_res` | auto | size of the game's 4:3 render targets in stereo: `auto` (OpenVR: SteamVR's recommended size times `eye_scale`; sim: unchanged), `WxH`, or `game` (unchanged) |
| `eye_scale` | 1.0 | multiplier on the recommended size for `eye_res=auto` (try 0.8 if it stutters) |
| `destripe` | 1 | blend the game's alternating 1-pixel columns (fine vertical lines in the headset otherwise) |
| `hand_pitch` | 0 | degrees the aim is tilted up from the controller's pointing axis |
| `hand_offset` | 0,0,0 | grip point in the controller's frame (x right, y up, z back), metres |
| `gun_scale` | 0.6 | gun and hand size |
| `gun_clip` | 2 | how much forearm and sleeve shows behind the hand (game units) |
| `gun_pivot` | auto | the gun's grip in the game's aim view (x,y,z); found per character when aiming starts |
| `gun_align` | 1 | turn the gun straight in the hand, so its barrel points where the shot goes (the game angles each gun in towards its screen centre: Dan's ~10°); 0 = held as the game holds it, the shot still following the barrel |
| `aim_laser` | 0 | a laser from the gun in aim mode (the game has none) |
| `rumble` | 1 | the game's rumble (its XInputSetState for player 1, both motors) as SteamVR haptic pulses on the left and right controllers, 0..1; 0 = off |
| `no_laser_guns` | 1269 | with `aim_laser=1`: gun meshes (vertex counts, logged when aiming starts) that get no laser |
| `aim_depth` | 1 | aim where the controller's ray meets the scene, so shots land where you point at any range (0 = at `aim_converge_m`) |
| `aim_ignore_m` | 1.2 | surfaces nearer your eye than this don't count as the ray's hit (your gun and hand) |
| `aim_converge_m` | 10 | distance along the ray aimed at when the hit isn't known (ray out of view, `aim_depth=0`) |
| `laser_m` | 30 | laser length (the silhouette screen's pointer; aim mode with `aim_laser=1`); 0 = no laser |
| `aim_hud_mask` | 0.6 | width of the HUD's centre band hidden in aim mode (the game's crosshair) |
| `aim_pitch_limit` | 80 | the game's aim pitch limit, degrees (the game allows 36) |
| `texgen_ref` | 1 | in aim mode, the game's screen-space colour gradients stay as they were when aiming began instead of shifting as the hand turns the game camera (0 = the game's own) |
| `cards` | d358d9e91e34870e | pixel shaders (hashes) of view-dependent effect cards |
| `card_mode` | scissor | `scissor` clips those cards to where the game camera's view lands in each eye; `skip` leaves them out of the eyes |
| `junction_shaders` | 1da2b9dfa4382813 | pixel shaders (hashes) of the junction screen's tint, which switches on its handling (see [status.md](status.md)) |

Under `[probe]` (mostly for the test copy; `tools\profile.ps1` sets them):

| key | default | what |
|---|---|---|
| `auto_activate` | 1 | tell the game its window is active when the controllers or scripted input press something (it ignores input otherwise) |
| `minimized` | 0 | start minimized |
| `mute` | 0 | mute the game's audio session |
| `fake_pad` | 0 | show the game a connected pad without VR, for the command file's pad input |
| `leave_desktop_alone` | 0 | stop the game clipping and moving the real cursor |
| `log_files` | 0 | log the files the game opens |
| `stereo_patch` | 1 | patch the game's vertex shaders for stereo as they compile |
| `vr_profile` / `vr_profile_source` | 0 / none | turn on the VR copy of `killer7.ini` (`k7vr\killer7.ini`, keyboard bindings for the controllers' keys; off by default, since the controllers drive the game's gamepad), and seed it from another file (testing) |

## Playtest checklist (next headset session)

Setup with the test copy: SteamVR on, `tools\profile.ps1 playtest`, `tools\run.ps1 -Normal`, sit
facing forward. Afterwards `tools\profile.ps1 test` and keep `k7vr\k7vr.log` (it records the
controller types, button codes, the headset's projection and any OpenVR errors).

- [ ] Silhouette screen: does the dot sit where the laser points, and does a shot on the
      silhouette start the target (a miss does nothing, as in the game)?
- [ ] Sharpness: fine vertical lines gone? Sharp enough? (`destripe=0` to compare)
- [ ] Aim mode (first corridor, lobby; aim at walls, doors, lights): does the gun point the way
      you hold the controller? If too high or low, roughly how many degrees (`hand_pitch`)?
- [ ] Is the gun in your hand: position (`hand_offset`), size (`gun_scale`), sleeve length
      (`gun_clip`)? Check both Garcian and Dan.
- [ ] Turning the controller far left/right/up/down: the world must not swing.
- [ ] Fire at something: do hits land where the barrel points? Does the game's recoil animation
      feel right in the hand?
- [ ] Anything missing from the HUD in aim mode (`aim_hud_mask`)?
- [ ] Menus and title on the big screen; cutscene captions on the HUD panel, not glued to your
      face; head turning keeps the scene still (black beyond ~75° off the game
      camera is expected); comfort at camera cuts.
- [ ] The open headset questions in [status.md](status.md#open-items): subtitle readability, the
      larger HUD panel, the persona change's stripes, whether right-hand A flips play to the flat
      screen, an aim marker, map legibility, and how a junction looks.
