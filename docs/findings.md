# Killer7 VR probe: findings (2026-10-22)

Probe of the 2018 PC port (Steam, with the October 2024 update) for a VR mod: stereo with
head look in the running sections (head at the game's camera for each shot), and a
motion-controller-aimed gun in first-person aim mode.

## Verdict so far

**Go, with one big known problem: culling.** Stereo rendering is easy here and is proven
working in-game. The game's CPU-side culling against its own camera means looking around
shows holes; fixing that is the main reverse-engineering job before VR is worth wearing.
Not reached yet: real gameplay (running section, aim mode with the gun model).

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

## Driving the game unattended (for later testing)

- Run minimized (user's request), muted via the process audio session, 60 fps cap while
  minimized. The game reads keyboard/mouse via raw input and ignores input unless it thinks
  it is active; the probe posts activation and injects `WM_INPUT`.
- Injected raw mouse **motion** works; injected mouse **buttons** don't (neither raw nor
  `WM_LBUTTONDOWN`), so the test profile binds keyboard fire to `KEY_J`.
- The language screen shows on every boot. The Target 00 opening is an aiming tutorial: put
  the laser dot on Garcian's silhouette and fire (the dot starts on his chest).
- `tools\scripts\new_game.txt` gets from boot through the tutorial into the Christopher Mills
  cutscene; timings around the boot logos are a bit fragile.

## Next steps (proposed)

1. Get to the first running section and first-person aim mode: capture both; find the gun
   model's draws and where the aim direction lives (needed for controller aiming).
2. Static analysis of the exe for the projection build and culling test; widen culling.
3. Calibrate units/IPD.
4. Then the real design doc: OpenVR output, per-eye post chain, HUD panel, aim mapping.
