# The static between scenes, all around the player

Date: 2026-11-20. Status: approved by the user 2026-11-20; built the same day (see "As built"
at the end). The area card part is still waiting on a capture.

## Why

After a junction or a door, Killer7 fills the screen with blue static. The static then splits
from the middle and rolls up and down into two bars, and on some transitions an area card shows
between them: the persona in a tilted frame, with the area's name in pink dot-matrix letters
("Entrance", "セルティックビル"). In the headset today the full static is a flat picture on the
screen panel, a 2D plane floating in front of the player. The user wants it to feel like part of
the space instead: "more elegant and immersive", not "a 2D plane".

The user picked the "all around you" staging:
- the static fills the player's whole view, wherever they look;
- when it splits, the two halves roll away above and below the player as wide bands a few metres
  away, curving around them;
- the portrait stays ahead at a distance, with real depth between it and the bars.

## What the game draws (recon, 2026-11-19 and -20)

From per-draw captures (`capture`) of the opening's transition (`tools\scripts\tour.txt`, the
static after Garcian's corridor) and of Harman's Room -> lobby (`tour_save.txt`, "Hallway"):

1. **Static coming in** (3D frames): the picture redrawn in strips, with noise over it. Already
   handled: these draws stay in the eyes over the whole view (`samples_eye_image`, full-cover 2D
   draws; see docs/screens.md "static transition").
2. **Full static** (about 20-22 draws a frame, no perspective draw, so a 2D frame that goes to
   the screen panel whole). The noise is the frame's last game draw: a 4-vertex triangle strip
   with pixel shader **`f8dfae19c3a8200b`**, covering 0..640 x 0..480, sampling a 640x480 noise
   texture (format 87) through a small UV window that jumps every frame (e.g. u 0.29..0.44,
   v 0.18..0.33; the window is how the noise animates). Under it: two strips of a copy of the
   last picture (pixel shader `16706d6ac595886d`, y 0..334 and 334..480) and HUD pieces parked
   off-screen at x < 0 (also drawn in normal play: a hidden HUD, not the card).
3. **The split** (3D frames again: the next scene draws underneath). The noise becomes two draws
   with the same shader: a top quad 0..640 x 0..y_t and a bottom quad 0..640 x y_b..480. Over
   about six frames y_t goes 240 -> 64 and y_b 240 -> 416, i.e. the bars end as the top and
   bottom 64 of 480 lines. After the opening's cutscene transition the bars hand over to the
   cutscene's letterbox; after Harman's Room -> lobby they vanish into gameplay.
4. **The area card**: not reproduced. It didn't appear on either route above, nor with loads
   slowed to 15 s. Its draws (portrait frame, bar artwork if any, the dot-matrix letters) are
   unknown. The user can capture it with F10 while it's on screen in the headset (writes
   `k7vr\capture_NNN.txt/.png`).

## Goals

- Full static: in the eyes, on a sphere around the player, fixed in the world (it doesn't turn
  with the head). No screen panel during it.
- The split: the two noise quads become two zones of that sphere, above and below, that open
  from the horizon outward as the game's quads shrink. The next scene shows through the gap in
  3D.
- The area card, once captured: its text on the bands, the portrait as a framed picture ahead
  at about 2.5 m, nearer than the bands, fixed in the world.
- Works for every transition that uses this static, with no per-area tables.

## Non-goals

- The static coming in (part 1): stays as it is.
- Junction screens, the TV, menus, cutscene letterbox: unchanged.
- No new art: everything drawn uses the game's own textures and shaders.
- Flat play (VR off) is untouched.

## Anti-goals (a failure even if it works)

- The static or bands swimming with the head (head-locked) or showing a seam or edge where the
  player looks: the point is that it surrounds them.
- Catching other screens that use the same shader or textures (the TV's static, the junction):
  anything detected as "static" must be the transition's noise.
- A blank or black frame where the game showed something (the card, the next scene).

## Approaches considered (the user picked A)

- **A. Each piece placed in space.** Recognise the noise draw by its shader; draw it into each
  eye on a world-fixed sphere (whole sphere during full static, two polar zones during the
  split); later place the card's pieces (text on the bands, portrait ahead) the same way. Builds
  on the junction screen's per-draw projection (`draw_junction_projected`). Most immersive, most
  work, and the card part waits on a capture.
- B. The whole 2D frame on a big curved wall instead of the flat panel. Cheap, but stretches the
  card and is still one surface. Rejected: it works against the request.
- C. Static and bands around the player, the card on the flat panel. A cheaper first step of A,
  but leaves the centre of attention flat.

## Design

### Recognising it
- New setting `[vr] static_shaders`, default `f8dfae19c3a8200b` (same form as
  `junction_shaders`, loaded with `load_shader_list`). A draw with that pixel shader, a
  640x480 texture in slot 0 and a quad whose extent spans the full width (x 0..640) is a
  **static draw**.
- A frame whose static draw covers the whole screen (y 0..480) is a **full-static frame**; one
  with static draws that each touch the top or bottom edge but not both is a **split frame**.
- Before relying on the shader alone, check the TV channels' static (`tour_save.txt`, "TV,
  static") and the junction screens in a capture: if they share the shader, add the texture size
  and full-width extent to the test (as above) until only the transition matches. Record which
  screens were checked in docs/screens.md.

### The surround
- When the first full-static frame of a transition comes, store the head's pose (relative to the
  reference): the surround's anchor. The sphere is centred on the head position at that moment,
  radius 4 m (`[vr] static_radius`, metres), its "front" at the head's yaw then (pitch and roll
  levelled). It stays fixed until the transition ends (no static draw for 30 frames, or a frame
  with neither static draws nor a full-static frame).
- Full-static frame: the eyes get the frame instead of the screen panel; the static draw is
  replaced by a tessellated sphere drawn with the game's own shader, texture and states, its
  vertices mapped on the CPU to each eye's clip space (the junction's method: CPU-mapped
  vertices, then the inverse of the draw's ortho map so the game's vertex shader puts them
  there). Fine enough tessellation that the affine interpolation after the CPU divide doesn't
  show (start at 10 degrees per cell, check in pairs).
- Texture on the sphere: tiles of the frame's UV window, each tile spanning the angle the game's
  quad spans in the game view (its 640 x 480 over the game camera's frustum, `g_gv_tan`), so the
  noise's grain is the size it is in the flat game. Tiles take the current frame's window, so the
  noise animates as in the game. Seams between tiles are acceptable if they don't read as lines
  on the noise; check in a pair.
- Other draws of a full-static frame (the copy strips under the noise, the off-screen HUD
  pieces): as they are drawn in the eyes today for 2D frames over the whole view; the sphere
  covers the view, so what's under it only shows where the noise is translucent.
- Split frame: each static quad's inner edge (y_t for the top quad, y_b for the bottom) maps to
  an elevation on the sphere through the game camera's vertical half-angle:
  `elev = atan((1 - 2*y/480) * g_gv_tan[1])`; the top zone spans elev_t..+90°, the bottom
  -90°..elev_b. At the split's start both edges are at the horizon (the whole sphere); when the
  game's bars are 64 lines the zones start about atan(0.73 * tan_y) above and below (≈24° for a
  62° game view). The next scene draws in the eyes as normal underneath.
- Bands outlive the split only as long as the game draws them: when its static draws stop, the
  zones go.

### The area card (blocked on a capture)
Once a capture of the card exists: the dot-matrix letters and any bar artwork are mapped onto the
band zones at the elevation and azimuth their screen position maps to (the same y -> elevation
rule; x -> azimuth through `g_gv_tan[0]` around the anchor's front); the portrait frame is a
flat quad facing the player at 2.5 m ahead along the anchor's front, at the angular size it has
in the game view. This part gets its own short design addendum after the capture, with the
draws' shaders and textures.

### Until the card is captured (my call)
The first build ships the surround without the card. Draws in a static frame that this design
doesn't recognise are not lost:
- drawn **before** the noise in a full-static frame (the copy strips, the 12-index scanline
  draw, the parked HUD pieces): drawn into the eyes as today, under the sphere. The flat game
  covers them with the same noise, so nothing visible is lost. If the noise turns out to blend
  enough that the copy strips show through and move with the head, they are left out of
  full-static frames (in the flat game they are already hidden by the noise);
- drawn **after** the noise (where a card would be): routed as today (textured pieces to the HUD
  panel, full-cover draws to the eyes). The card, if it comes, looks as it does now (flat), never
  blank, until the addendum places it;
- a full-cover 2D draw after the noise (a scanline or tint layer over the static) goes on the
  sphere with the noise, not flat over it.

### Which camera (my call)
The game-view tangents used for the noise's grain and the band elevations (`g_gv_tan`) are
frozen when the anchor is set: the last 3D frame's values before the static. A full-static frame
has no perspective draw of its own and split frames already show the next scene's camera (a
cutscene's is letterboxed), so live values would make the grain or the band edges jump. The
bands end where the game's bars end in that frozen view, also on the cutscene route, where the
cutscene then fills the eyes as it does today.

### Through a load
The game keeps presenting frames during the static: with loads slowed to 15 s (2026-11-19) the
full-static frames kept coming at the normal rate and per-frame captures every 200 ms worked
throughout. So the eyes don't go blank where the panel used to cover a stall; no overlay fallback
is planned. If a headset check shows SteamVR's loading view during a real load, revisit.

### Settings and docs
- `[vr] static_shaders`, `[vr] static_radius` in the settings table of docs/development.md;
  `static_surround=0` turns it off (back to today's screen panel).
- docs/screens.md "static transition" updated; status.md gotchas if anything surprising turns up.

## Testing
- Unit tests (tests/test_headmath.cpp): the y -> elevation mapping (y=240 -> 0, y=64 and 416
  symmetric, monotonic) and the sphere-direction -> eye clip mapping (the anchor's front maps to
  the eye's centre when the head hasn't moved; a 90° head turn moves it out of view; directions
  behind the eye are clipped).
- Harness (sim profile): `pair` captures during the full static and the split on the opening
  route (tour "static transition" timing) with the head turned 0°, 60° and 150° (`head`): the
  static fills each eye at every angle, the eye images differ (stereo) only by parallax, and the
  bands stay put when only the head turns. A `sweep` during full static shows no edge.
- check.ps1 tour stays 19/19; the 2D comparison for "static transition" will flag (the VR picture
  now differs from the flat one by design): update that screen's expectation.
- The user's headset check: the static surrounds them, the split opens at the horizon, nothing
  catches on the TV or junctions.

## Left to the builder
- Exact tessellation and tile layout, the end-of-transition rule's frame counts, how the sphere's
  vertices carry the draw's colour/extra fields (copy from the game's vertex as the junction
  code does), and where the code lives (a new file `src/proxy/static_surround.cpp` beside
  hooks_d3d.cpp is preferred; hooks_d3d.cpp is already 3,900 lines).

## As built (2026-11-20)
- The code is in `src/proxy/hooks_d3d.cpp` ("The static between scenes, around the player",
  `static_draw`, `draw_static_surround`, `static_surround_frame_end`), next to the junction code
  it reuses (vertex access, the ortho map, twins), not a file of its own. The math
  (`band_elevation`, `surround_anchor`, `sphere_point`, `point_to_eye_clip`) is in
  `src/vr/headmath.cpp` with unit tests.
- Changed from the design: full-screen noise over a 3D frame also goes on the sphere once the
  surround is anchored. On the opening's route the next scene (a cutscene) starts under the
  full noise, and the old routing clipped it to the letterbox band, a head-locked rectangle.
- Tiles alternate mirrored (and there is an even number of columns), so neighbouring tiles meet
  on the same texels: the tiles' seams read as lines otherwise.
- The shader (`f8dfae19c3a8200b`) is a general textured 2D shader (HUD pieces use it too); the
  640x480 texture, the full-width extent and touching the top or bottom edge are what pick out
  the static. The same static plays going into the map, the pigeon memo and out of the pause
  menu; those get the surround too. The TV's static channel doesn't match.
- Two boot-time statics come before any 3D frame (no game-view tangents to freeze); they stay on
  the screen panel as before.
- The harness gained `waitlog [ms] TEXT` (wait for a log line), and the log names each static
  draw's zone as it changes (`stereo: static draw frame N top|bottom|full ndc y ...`).
