# Killer7 VR probe: findings (2026-10-22)

Probe of the 2018 PC port (Steam, with the October 2024 update) for a VR mod: stereo with
head look in the running sections (head at the game's camera for each shot), and a
motion-controller-aimed gun in first-person aim mode.

## Verdict so far

**Go.** Stereo rendering is easy here and proven in-game. Culling, the main risk, has a working
fix (FOV inflation through a code patch, framing restored in the shader). Remaining known
problems are view-dependent effect cards and empty space behind some fixed cameras. The aim
angles are found and writable (controller aiming), and the gun in aim mode is an ordinary
eye-space model.

## What the port is

- `Killer7Win.exe`: 32-bit, D3D11 (feature level 11.0), no Steam DRM, no `steam_api`. A copy
  runs standalone. Imports only `D3D11CreateDevice` from d3d11 and `D3DCompile` from
  `D3DCOMPILER_43`, plus `XINPUT1_3`.
- It is the GameCube game running on a GX→D3D11 layer. Shaders are generated at runtime from
  `Shaders\VertexShaderTemplate.hlsl` (+ pixel and geometry templates) and compiled with
  `D3DCompile`. `Shaders\pipeline.cache` caches them (delete it to force compiles).
- Vertex constants (cb0, 3344 bytes): GX position matrices are **model-view** (camera already
  folded in), and `projMatrix` is separate at byte 2560. Perspective draws have projection
  row 3 = (0,0,-1,0); orthographic (HUD/2D) draws have (0,0,0,1). Easy to tell apart per draw.
- Frame: everything (pre-pass, 3D scene, HUD) draws into one 1920x1440 (4:3) target with a
  depth buffer, then post passes, then a blit to the window. Before the scene, an ortho
  pre-pass renders into the same target and is copied out (GX "EFB copy" emulation).
- Typical cutscene projection: `[1.569 0 0 0 | 0 1.852 0 0 | 0 0 -1e-6 -0.1 | 0 0 -1 0]`
  (near 0.1, infinite far; ~57° vertical FOV).
- Level data is inside `data.vfs`; movies are `.webm` decoded by a built-in libvpx; voices
  `sound\CharaGeki\*.hps.ogg`.

## Proven in the probe

1. **Hooking** works through a stand-in `XINPUT1_3.dll` in the game folder that patches the
   exe's imports. d3d11 gives each context its own function table and rewrites draw slots in
   it as state changes, so vtable-patching draws does not stick; the probe hands the game a
   stand-in context whose slots are thunks into the real context's *current* table.
2. **Stereo**: the `D3DCompile` hook patches the generated vertex shaders with a per-eye
   eye-space transform (perspective draws only). Draws into the scene target are repeated into
   mirror targets for the right eye, clears too. Same-frame L/R pairs look right: whole scene
   in both eyes, correct parallax direction, HUD untouched.
3. **Head look**: the same transform takes yaw/pitch, so the view can turn away from the
   game camera.

## Problems found

- **Culling (main risk).** Turning away from the game camera shows the game only submits what
  its own camera sees: at ±45° large empty areas, at ±90° distant low-detail stand-ins (flat
  yellow/blue buildings), behind the camera nothing, looking 45° down no sidewalk. The world
  exists; it just isn't drawn. Fix needs the camera/culling code found in the exe. Blindly
  writing all heap floats equal to the FOV (56.8598) crashed the game; one of them is not the
  FOV. Next: find where `projMatrix` is built (the GX `C_MTXPerspective` equivalent) and the
  frustum test, by static analysis of the exe.
- **Units.** Parallax at a given separation is much larger than centimetre units predict; a GX
  unit looks like several centimetres. The IPD in game units needs calibrating (e.g. from a
  character's height in model space).
- **Post effects and EFB copies** read only the left-eye target in the mirror approach. A real
  renderer must give each eye its own post chain (or run those passes per eye).

## Round 2 (2026-10-23): camera code, aim mode

**Camera and projection, from static analysis + hardware write watches** (RVAs, exe base
0x400000 in the file; the probe logs the runtime base):

- GX state lives behind the global pointer at VA `0x86e32c`; the projection is at `+0xc18`
  (type at `+0xc58`). `7d1d0` is `GXSetProjection(matrix /*ecx*/, type /*edx*/)`; `7d250`
  copies it back out (`GXGetProjectionv`). 14 call sites set projections.
- The 3D projection comes from `c17f1`, which calls `c1390(camera /*ecx*/, out /*edx*/)`:
  camera `+0x50` = projection type (1 perspective via `c5200`, 2 off-axis frustum, 3 ortho),
  `+0x38` near, `+0x3c` far, `+0x40` vertical FOV in degrees, `+0x44` aspect.
  `c5200` is a plain perspective builder (`1/tan(fov·π/360)`, divided by aspect).
- Camera manager: global pointer at VA `0x86fa54`. `+0x64` FOV, `+0x74` source camera,
  `+0x7c` render camera. Each frame `ffd18`/`ffd27` write the FOV into the manager and the
  source camera; `10301a` copies it into the render camera, which then renders (`c15f0`).
- Experiment: NOP the camera FOV write at `ffd27` (`f30f114840` → `9090909090`) and set the
  source camera FOV to 110°: the game renders the wider view (projection x 0.59 vs 1.32), no
  crash. So the game FOV can be inflated from outside. Whether culling widens with it is still
  open: the first corridor is too enclosed to show culling; needs an open street section.
  (Near 0.1, far 10000, aspect 1.18 in the first section.)

**Culling, settled.** In the lobby (second running section), with the view held still, the
draw count rises with the game FOV: 153 at the normal ~60°, 167 at 120°, 170 at 160°. So the
game frustum-culls against its camera FOV, and inflating that FOV widens culling.
`fov_inflate DEG` implements it: a stub at `10301a` (where the manager copies the FOV into the
render camera) records the intended FOV and writes the inflated one; the patched vertex shader
scales clip x/y by `tan(inflated/2)/tan(intended/2)`, so the front view keeps its framing
(verified) while more of the world is submitted. In VR the headset projection replaces this.

Side effect: the extra draws at 150° are all 4-vertex quads with one pixel shader
(`d358d9e91e34870e`) that samples a screen-projected texture (`texCoord.xy /= texCoord.z`) over a
flat colour. They are view-dependent effect cards made for the game camera; from other angles
they show as flat blue slabs (the "cyan voids" seen earlier are likely these, not missing
geometry). They need special handling in VR (skip when off-axis, or draw them only for the
game view). Separately, fixed camera spots can sit at or behind walls, so some directions
genuinely have no geometry.

**Aim mode** (hold Space): first person, the gun and hand are the first perspective draws
after the clear, with the character pixel shader and the same projection as the world,
~2–7 units in front of the camera. The gun is an ordinary eye-space model, so moving it to a
controller is an extra transform on those draws in the patched vertex shader.

**Aim state (controller aiming lead, verified by writing it).** Aim mode is driven by a global
aim controller at RVA `0x46bde8` (VA `0x86bde8`); `1b3a0f`–`1b3b3c` turns it into the camera:

| offset | meaning |
|---|---|
| `+0xc4` / `+0xc8` | aim pitch / yaw, radians (`+0xd0`/`+0xd4` hold a second copy) |
| `+0xdc`..`+0xe4` | eye position (copied to camera manager `+0x0c`) |
| `+0xf4`..`+0xfc` | look-at target = eye + direction·100 (to manager `+0x24`) |
| `+0x108` | aim FOV (44°; copied to the manager and source camera) |
| `+0x10c` | camera manager pointer |
| `+0x150` / `+0x154` | aim velocities, decayed into pitch / yaw each frame (`1b3b40`) |
| `+0x160` | thought to be a reference yaw; reads ≈0 (see Round 3) |

Writing pitch/yaw (both copies) during aim mode turns the view and reticle; the game clamps
them (pitch −0.35 settled at −0.25; yaw 3.6 came back as −2.82). There is a small automatic
drift (sway). So controller aim = write these each frame from the controller direction
relative to the body, within the game's limits, and zero the velocities. Because the aim-mode
camera follows the aim, the VR head transform must be relative to the game camera's rotation
(world-locked head), and the gun, being attached to the game camera, already points along the
aim; only its position needs moving to the hand. Not verified yet: that shots go along this aim.
Keyboard aim in aim mode is the arrow keys.

**Units:** the held gun sits ~2.5 units in front of the eye, so a unit is roughly 20–30 cm,
which matches the stereo parallax measurements. IPD in game units ≈ 0.2–0.3 (calibrate).

## Round 3 (2026-10-24): stripes, aim mapping, pitch clamp

**The "vertical scanlines" seen in the headset are in the game's own frame.** Many surfaces
(lit 3D materials, the point-sprite kanji on the persona cards) come out as alternating
1-pixel columns: odd columns lose red and green, or alternate black/colour. It is there with
VR off, with `EnableAA = false`, and in orthographic draws the stereo patch never touches, from
the very first scene draws on. On a monitor it is invisible because the 1920-wide frame is
scaled down to the window, which blends each column pair; the headset shows the eye image near
1:1. The mod now blends it on the way to the headset (horizontal 1-2-1 filter on the eye and
screen images, `[vr] destripe`, compute shader in `vr.cpp`). Where in the material it comes from
was not pinned down (the per-draw snapshots — `capture N...` — show it in every lit draw).

**Aim mapping (measured).** The aim camera looks along
`(sin yaw·cos pitch, sin pitch, cos yaw·cos pitch)` from a fixed aim eye; yaw π is world −z,
LEFT raises yaw, world is right-handed with +y up. Yaw is **not** clamped: it wraps through
360° (the earlier "3.6 came back as −2.82" was the wrap). `+0x160` read ≈0 throughout, so it is
not a reference yaw.

**Pitch clamp.** ±π/5 (36°), applied by `maxss/minss` against two shared `.rdata` constants
(`0x82c214` = −0.628, `0x82b494` = +0.628) at RVAs `1b0391/1b0399` (the aim controller update,
`edi+0xc4`) and `1b3c89/1b3c91` (a second camera path, `esi+0x15c`). `+0.628` is also used as a
multiplier elsewhere (`2292f3`, `25b07b`), so the mod repoints only those four instructions'
operands at its own limits (`aim_pitch_limit`, default 80°; `aim.cpp`). Verified: 57° up and
59° down held.

**M4 in sim mode (headless).** With the right hand steering the aim (look-at point
`aim_converge_m` along the hand ray), the corridor stays still across ±30–60° of aim
(world lock from the camera manager's eye/target at the first 3D draw), the arm and gun follow
the hand, the forearm is clipped behind the grip (`gun_clip_z`, an `SV_ClipDistance` on the
arm/gun draws only), and the game's reticle (centred, full-height lines) is masked out of the
HUD (`aim_hud_mask`).

**Laser-pointer screens (the silhouette shot that starts a target).** The dot is a heap object
(found with `scanpoint`, which scans memory for the dot draw's vertex): x, y at +0/+4 in game
camera eye space, drawn at (x, y, `+0x20` = 27.3) moved 50 units forward, so it lies on a plane
22.7 units ahead; velocities (idle sway) at `+0x24/+0x28`; screen x/y (640×480) at
`+0x0c/+0x10`, which the hit test uses against a CPU readback of the frame. Updated by
`173e40` (owner `esi`, dot at `esi+0x31a8`; pointer input from `[[0x86e328]+0x260]+0x1c4`,
an absolute 640×480 position, moves the dot towards it), clamped at `237700` (±10.8, ±8.7,
ecx = the dot). Injected arrows/mouse/fake cursor did not move it in the harness. The mod hooks
`237700` to set x, y from the right controller's ray ∩ that plane and zero the sway, draws its
laser from the controller to the dot, hides the game's 2D line, and suspends FOV inflation
while the screen is up: the game projects the dot with its camera's FOV and reads the frame back,
so both must use the FOV it really renders (with inflation the readback silhouette was tiny and
the hit missed). The game-view pass now renders with the game's own projection for the same
reason. Verified in sim: a miss does nothing, an off-centre hit (the head) starts the target.

## Driving the game unattended (for later testing)

- Run minimized (user's request), muted via the process audio session, 60 fps cap while
  minimized. The game reads keyboard/mouse via raw input and ignores input unless it thinks
  it is active; the probe posts activation and injects `WM_INPUT`.
- Injected raw mouse **motion** works; injected mouse **buttons** don't (neither raw nor
  `WM_LBUTTONDOWN`), so the test profile binds keyboard fire to `KEY_J`.
- The language screen shows on every boot. The Target 00 opening is an aiming tutorial: put
  the laser dot on Garcian's silhouette and fire (the dot starts on his chest).
- `tools\scripts\new_game.txt` gets from boot through the tutorial into the Christopher Mills
  cutscene; `tools\scripts\to_gameplay.txt` continues into the first running section. Timings
  around the boot logos are a bit fragile. In the test profile Esc is freed from exit_game
  (moved to F12), but Esc still doesn't skip these cutscenes.
- Menus: the Yes/No prompts need menu_left/right (A/D) before Space.
- Probe commands for reverse engineering: `scanf`, `scanpair`, `scanproj`, `peekf/peekd`,
  `pokef`, `patch ADDR HEX`, `watch ADDR` / `watchptr PTR OFF` (debug-register write watch that
  logs the writer and return addresses), addresses as hex or `exe+RVA`.
  `tools\disasm.py RVA [--func]` and `tools\xref.py` (disp/addr/float/calls) read the exe.

## Next steps (proposed)

1. Aim angles found (above). Still to do: confirm shots follow them, and tag the gun/hand
   draws reliably (first character-shader draws after the clear in aim mode is the lead).
2. Decide how to treat the effect cards (`d358d9e91e34870e` quads) and empty directions.
3. Calibrate units/IPD (e.g. from a character's height).
4. Then the real design doc: OpenVR output, per-eye post chain, HUD panel, aim mapping.
