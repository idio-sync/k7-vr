# Killer7 VR: design

Date: 2026-10-23. Repo: `C:\Users\Virtual\Git\killer7-vr`. Evidence for every technical claim
below is in [docs/findings.md](../../findings.md) (the probe's write-up, with RVAs and offsets).

## Intent

A VR mod for the PC port of Killer7 (2018 Steam release with the October 2024 update) that the
user plays in a PCVR headset (a Steam Frame, streamed through SteamVR, on this PC).

What the user asked for, in their words: a stereoscopic view of the movement areas, "ideally
fully viewable if you move your head around"; and in first-person aim mode "you have to use
your controller to aim the gun like other VR ports, you should see the character's arm etc."
For the running sections, "the camera can be where it would be in 2D for now": the player's
head sits at the game's camera for each shot.

Why it works for this game: the port is the GameCube game running on a GX→D3D11 translation
layer that compiles its shaders at runtime from text templates, keeps the camera (model-view)
and projection separate, and draws everything through one vertex template we can patch. The
probe proved stereo, head look, a culling fix and writable aim angles in the running game.

## What success looks like (v1)

The user plays from the start of Target 00 (Angel) through its first sections in the headset:

- Running sections and real-time cutscenes are in stereo, at correct scale, with head rotation
  and position tracking around the game camera for each shot. Turning the head shows the world
  as far as FOV inflation reaches (my call: the game frustum at the default 150° inflation, about
  ±77° horizontally and ±75° vertically around the game camera; beyond that, gaps are accepted in
  v1). Finding and widening the game's frustum test itself is a stretch goal, not v1.
- Holding aim mode puts them at the game's first-person aim eye. They aim with the right
  controller; the game's own gun and hand model is in their hand and points where the
  controller points; shots land where they aim.
- The HUD and pre-rendered movies are readable on panels; menus are usable.
- The headset is fed at its own rate (SteamVR reprojection may fill frames the game doesn't
  render), without the game speeding up, and nothing the mod adds makes them sick.

## Goals, non-goals, anti-goals

**Goals (v1)**
1. Stereo rendering of the 3D scene with per-eye headset projections and full 6DoF head pose,
   anchored at the game camera.
2. Culling widened so head look shows the world (FOV inflation, proven).
3. Per-eye post-processing and per-eye EFB-copy effects, so both eyes look like the game.
4. HUD (all orthographic draws) on a panel; FMVs on a virtual screen; menus usable.
5. Controller input for all game actions, including aim mode and fire.
6. Controller aiming in aim mode with the game's gun/hand model moved to the controller.
7. A monitor mirror view, and a `k7vr.ini` with the tunables (IPD scale, world scale, FOV
   inflation, HUD panel distance/size, controller bindings, feature toggles).
8. An install/uninstall script for the user's real game folder (with backup), and a safe mode
   (ini flag or missing runtime → the mod steps aside and the game runs flat).

**Non-goals (v1)**
- Physics hands, full-body IK, holsters, motion-controlled reload or melee.
- Free locomotion: Killer7 moves on rails; the game's own movement stays.
- Re-staging cutscenes or replacing the game's camera choices (the head stays at the game
  camera; tracking shots move the player with the camera, as in the 2D game).
- Comfort features beyond not adding motion (no vignettes or snap-turn in v1; revisit after
  the user's first headset session).
- Seeing directly behind the game camera; parts of fixed shots have no geometry at all.
- OpenXR. (See "Decisions".)

**Anti-goals (failing these is failure even if the rest works)**
- The world rotating or sliding with anything but the player's head or the game's own camera
  (in particular: aiming must not rotate the world).
- Changing the game's difficulty or timing (aim assist, game speed, damage). One deliberate
  exception, the user's choice: the aim clamp is widened so the gun follows the hand.
- Touching the user's Steam install, real profile or Steam Cloud saves during development.
- Redistributing any game file.

## Constraints

- `Killer7Win.exe` is 32-bit, D3D11 feature level 11.0, no DRM, no anti-cheat. The mod is
  32-bit and loads as a stand-in `XINPUT1_3.dll` in the game folder (proven). The exe imports
  only `D3D11CreateDevice`, `D3DCompile` (D3DCompiler_43) and XInput ordinals 2–4.
- The 2024 update's exe is the target. Record its hash; the mod checks known code bytes before
  patching (as `fov_inflate` already does) and disables a feature, with a log line, on mismatch.
- d3d11 swaps draw entries in each context's own function table, so the immediate context must
  be wrapped (the probe's thunked stand-in context), not vtable-patched.
- The game builds shaders at runtime from `Shaders\*Template.hlsl`; `Shaders\pipeline.cache`
  bypasses compiles, so the mod must delete or ignore it when its shader patches change.
- Development and automated testing use the test copy `C:\Users\Virtual\killer7-vr-game`
  only, with Saved Games redirected into it. Test runs are minimized, muted (process audio
  session) and capped at 60 fps while minimized; the user asked for minimized runs.
- No controller is attached to this PC; the game reads keyboard/mouse through raw input and
  only acts on input while it believes it is the active window. Injected raw-input keys and
  mouse motion work; injected mouse buttons don't.
- The headset is only available in user playtests. Everything that can be checked without it
  must be (scripted runs, captures, stereo pairs, unit tests).

## Decisions (made with the user)

- New repo, this one; C++ for the mod; probe tools stay as the test harness.
- Head anchored at the game camera each shot, for now.
- **The view tilts with the shot**: the head takes the game camera's full orientation, pitch and
  roll included (no horizon levelling). The head transform is relative to the game camera's eye
  space, exactly as in the probe.
- Aim mode: controller aiming, the game's own gun model in the hand.
- **Aim limits are widened** so the gun goes where the hand points: patch the game's pitch/yaw
  clamp (or its limits) rather than stopping the gun at the original limit.
- **Persona scope for v1**: whoever is playable in Target 00's first sections (the corridor and
  lobby the probe reached, and the next rooms). Other personas' weapons (Mask's two guns,
  Kevin's knives, Kaede's scope zoom) are out of v1 and get generalised later.
- Unattended tests run minimized and muted.

## Decisions (my calls; the user can overturn them)

- **Runtime: OpenVR**, not OpenXR. 32-bit OpenVR is known to work with SteamVR and the user's
  other mods use it; a 32-bit OpenXR path is unproven here.
- **Rendering path: keep the game's own draws**, patched per eye, rather than recording and
  re-rendering geometry ourselves. The scene target is mirrored per eye (proven); the builder
  extends this to the post chain (below).
- **World-locked head in aim mode**: the head transform is computed relative to the game
  camera's rotation, so when the game camera turns with the aim, the shader counter-rotates
  and the world stays still. The view shows the world from the aim eye position.
- **HUD panel**: one world-locked panel in front of the seated position (about 2 m away,
  ~1.4 m wide), recentred with the view-reset button. The aim reticle is not on the panel; in
  aim mode the mod hides the game's reticle and shows a laser from the gun muzzle (in keeping
  with the game's laser sights), ending in a dot where the aim ray hits.
- **FMVs** on a large virtual screen, head look free; real-time cutscenes in stereo.
- **Effect cards** (the projected-texture quads, pixel shader `d358d9e91e34870e` in Target 00):
  draw them only in the region where the original game camera saw them, i.e. clip them to the
  game camera's frustum, so they keep their intended look head-on and never appear as flat
  slabs off-axis. If that proves impractical, skip them off-axis and log which draws.
- **Input**: VR controllers drive the game through injected raw-input keyboard/mouse events
  (proven), with the mod forcing its own key bindings at runtime rather than depending on the
  user's `killer7.ini` (keyboard fire is unbound by default). If the builder finds how the game
  detects pads, emulating an XInput pad is acceptable instead.
- **Default bindings** (index-style controllers; adjustable in `k7vr.ini`): right grip held =
  aim mode; right trigger = fire; left trigger held = walk/run; left stick back = turn around;
  left stick directions = branch choice at junctions; A = reload; B = scan; X = special;
  Y = map; left menu = game menu; right stick click = persona switch menu; both sticks clicked
  = recentre view.
- **Scale**: one GX unit is about 20–30 cm (from the held gun's distance and stereo parallax).
  The builder calibrates it once from a character's height and sets the default world scale and
  IPD from that.

## How it works (the parts already decided by evidence)

**Injection and hooks** (as in the probe): stand-in `XINPUT1_3.dll` → patch the exe's imports of
`D3D11CreateDevice`, `D3DCompile`; hand the game a wrapped immediate context; hook swap-chain
`Present`/`ResizeBuffers`.

**Stereo**: the `D3DCompile` hook patches every generated vertex shader: for perspective draws
(projection row 3 = (0,0,-1,0)), eye-space position is multiplied by a per-eye head transform
from a constant buffer (slot b12), then projected. v1 replaces the game's projection for
perspective draws with the headset's per-eye projection (asymmetric), keeping the game's depth
conventions (`pixelCenter.zw` mapping in the template). Orthographic draws are HUD.

**Per-eye targets**: draws into the scene target (a render target with a same-sized depth
buffer, 1920x1440 in the 2024 port) are issued once per eye: left into the game's targets,
right into mirrors (proven). v1 must extend this so the frame after the scene (post passes,
EFB-copy pre-passes that the scene samples, the final blit) also runs per eye with every
game-created render target and its shader-resource views swapped for the eye's copy. Each eye's
final image goes to OpenVR. Eye resolution: the builder chooses between keeping the game's
1920x1440 per eye and resizing the game's scene targets to the headset's recommended size.

**Culling**: FOV inflation (proven). The stub at exe RVA `0x10301a` records the intended FOV
and gives the render camera an inflated one (default 150°, ini). With headset projections the
intended FOV is only needed for the monitor mirror and the effect-card clip.

**Anchor and head pose**: eye space of the game camera is the origin. Head pose is taken
relative to a seated reference set at start and on recentre. Camera cuts move the player with
the game camera.

**Aim mode** (exe RVA `0x46bde8` aim controller: pitch `+0xc4`, yaw `+0xc8`, copies `+0xd0/+0xd4`,
velocities `+0x150/+0x154`, reference yaw `+0x160`, aim eye `+0xdc`): each frame, write pitch
and yaw from the right controller's direction expressed in the game's aim frame (relative to the
reference yaw), zero the velocities. The game's clamp is widened (find it: pitch −0.35 settled
at −0.25 in the probe, and yaw is limited around the reference yaw), so the written angles
stand. The head transform
counter-rotates by the game camera's resulting rotation (world-locked). The gun/hand draws are
the first character-shader perspective draws after the scene clear in aim mode; the builder
finds a robust tag (shader hash, vertex count, or the skinning matrix range) and applies an
extra eye-space transform that moves the model from its game position to the controller pose.

## Milestones (each ends with a check the user can do, or a headless check)

- **M1 Headset stereo, running sections.** OpenVR output, headset projection, 6DoF head
  around the game camera, FOV inflation, monitor mirror. Headless check: stereo pairs from
  scripted runs look right at the calibrated IPD. Playtest: Target 00 corridor and lobby.
- **M2 Looks like the game in both eyes.** Per-eye post chain and EFB copies, effect cards,
  HUD panel, FMV screen, menus. Headless: per-eye captures match the mono frame head-on.
- **M3 Controller input.** All game actions on the controllers, mod-owned bindings, recentre.
- **M4 Aim.** Controller aim, world-locked head in aim mode, gun in hand, laser, verify shots
  follow the aim (first enemies in Target 00).
- **M5 Settings, safe mode, install/uninstall** for the user's real install, README.

## Testing

- Keep the probe's harness: scripted runs (`tools\scripts\*.txt`), captures, stereo pairs,
  memory/patch commands, minimized and muted.
- Unit-test the math on the host (projection conversion, head/eye transforms, aim angle
  conversion and clamping) without the game.
- A no-headset mode that renders the two eyes side by side to a window from a scripted head
  pose, for headless checks of M1–M4.
- Headset playtests at the end of each milestone, by the user, with a short script of what to
  look at.

## Left to the builder

- Code layout (probe code vs mod code), threading, how OpenVR submission is synchronised with
  the game's frames.
- How to run the post chain per eye (replay vs resource substitution), and eye resolution.
- How the gun/hand draws are tagged and how the controller pose maps to the gun's grip.
- Frame pacing: measure whether game logic is tied to frame rate; keep game speed unchanged at
  headset rates (cap, decouple or rely on reprojection).
- The exact effect-card clip, and recognising other view-dependent draws in later stages.
- The ini format and defaults beyond those named above.

## Known risks

- Post chain and EFB copies per eye may be the largest piece of work.
- Later stages may have other view-dependent tricks (cards, backdrops) or cameras inside walls.
- If game logic runs per rendered frame, headset-rate rendering needs decoupling.
- In aim mode the game camera, and so culling, follows the gun while the head stays
  world-locked: aiming far to one side while looking ahead can cull what the player looks at.
  Inflation covers about ±77° around the aim direction; widening the aim limits makes this more
  likely. Mitigations if it shows up in playtests: more inflation, or the frustum-test stretch
  goal.
- Widening the aim clamp may expose animation or camera code that assumes the original limits.
