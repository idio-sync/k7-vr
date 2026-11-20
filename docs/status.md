# Status and next steps

Where the work on "every screen looks right in the headset" stands, how to pick it up, and what
to watch out for. Last updated 2026-11-17 (main at the commit that added this file).

## Where things stand

The scripted tour (`tools\scripts\tour.txt`, 37 screens from the language select to Kaede's
first junction) passes all 18 `expect` checks in the simulated headset. The screen catalogue
(`python tools\catalog.py`, published at https://claude.ai/artifact/9HT4eYeGssdiYwG8AiA1pK)
shows each screen as the headset gets it next to the game with VR off; `docs\screens.md` holds
each screen's status and notes.

Fixed on 2026-11-17 (all pushed):

| Commit | What |
|---|---|
| c520a25 | Screen-sized 2D overlays whole on the HUD panel (LT view, quit prompt, tutorial header, subtitles); HUD panel 2.13 m wide at 2 m |
| f47c401 | Options TV shows its text (the backdrop skip now only while 3D frames are drawn) |
| 8e942b2 | Same-frame check compares cutscenes within their letterbox band; flags over 8% |
| 29ac431 | Full-screen effects stay in the eyes: the static between scenes, its scanlines, frame copies drawn in strips |
| 4dcb107 | Harman's TV whole on the screen panel with its text and portrait (flat frames); the save tour |

Earlier: b1b70f6 / c9963a3 (menus whole on the screen panel; the headset hand-off fix; LT no
longer flips to flat), 64256d2 / 0188cf3 / 4178d9d (check.ps1, tour, catalogue, look-around).

## How 2D draws are routed now (hooks_d3d.cpp, draw_stereo)

After the frame's first perspective draw, an orthographic (kind 2) draw into the scene target:

1. samples an eye-rendered picture or a frame-sized copy of one (`samples_eye_image`): a
   screen-image effect, drawn in the eyes; before any 3D draw in a frame that follows a 3D frame
   it's a backdrop and is skipped in the eyes;
2. covers 95% of the screen or more (`overlay_cover`: the bounding box of up to 1024 vertices
   read from the game's 32 MB dynamic vertex buffer at its Map address, under an exception
   guard; `kFadeCover`): a fade or full-screen effect, drawn in the eyes over the whole view;
3. otherwise textured, or points/lines: HUD panel;
4. otherwise untextured with a known extent: HUD panel too (dims, prompt boxes, frame lines);
   unknown extent (indexed draws): eyes.

Frames with 24+ untextured 2D quads after the scene are menus: the game view goes whole to the
screen panel (`kMenuQuads`). Frames with no perspective draw are 2D: screen panel.

## Open items

- **Headset session** (only the user can judge): subtitle readability (white text, no bar behind
  it in VR); the larger HUD panel; the persona change's stripes (softened by de-striping, flagged
  16.5% by the same-frame check); whether right-hand A still flips play to the flat screen (seen
  in the 2026-10-26 playtest; the log now records each switch's quad count); the aim marker
  (no reticle or laser while aiming: decide whether a dot where the barrel's ray lands is
  wanted); map legibility on the screen panel.
- **Pigeon memo**: the set ends above the original framing, so the top third of the wider VR
  view is black. A design question (e.g. a soft fade outside the original frustum in such
  scenes); no check detects it.
- **One boot hang** (log stopped 0.64 s after launch, no crash record) on 2026-11-17; not
  reproduced in a dozen launches since. If it recurs, note what ran just before.
- **Dan Smith title card**: should be whole on the HUD panel since c520a25; the tour's capture
  keeps landing just before the card.

## Past the first junction (explored 2026-11-17, simulated headset and VR off)

There was no earlier save (slot B was an empty folder). A new one was made in Harman's Room:
Slot A, file 1 ("[Angel] 11/17", `<test copy>\k7vr\SavedGames\killer7\Killer7eu_0`, copy in
`<test copy>\k7vr\save_backups\harman_room_2026-11-17`). Route from Kaede in the lobby: hold A
to the junction, "Iwazaru" (stick right + A) is a hint dialog, then "Harman's Room" (right).
Inside: "TV" (up-left), "Samantha" (up-right: save), "Room" (right: the back half, Iwazaru).
Out through "Hallway" (left), then "Elevator Hall" (up-left) at the lobby junction; Iwazaru
there first, then a Heaven Smile at the elevator door.

Loading it (VR off; keyboard): boot as the tour does, Space at PRESS START, W x4 then S
(CONTINUE), Space (Slot A), Space (file 1), "Load this file?" starts on NO: A then Space. It
opens on the silhouette aiming check (J fires), then the moon (pad A), then Kaede at Harman's
Room's junction. Junctions: hold the stick towards a label, A while holding (`padhold - X Y`,
`padhold A X Y`, release); a plain stick-up didn't take "Hallway" in the Elevator Hall before
Iwazaru had been talked to.

VR-only problems found (pictures from that run are not kept in the repo):

- **Harman's TV: fixed (see "Flat frames" below).** It was broken in VR: the persona's face
  small and seen from further off, all the text gone (empty green and yellow boxes), the Blood
  Room channel split between the HUD panel and the eyes, the dial flipping between panels.
- **Junctions: fixed, in 3D (see "The junction screen" below).** They were split: the tint,
  windows and preview stretched over the whole eye, the outlines and labels on the HUD panel at
  about half that size, so the outlines framed nothing (same-frame check 12-23%; now 2-5%).
- **HUD panel against the game's FOV.** The HUD panel is a fixed 56°; the game view is 60° in
  the lobby and 42° in Harman's Room. Overlays that point into the scene line up only when the
  two angles match. The junction's are now drawn at the game view's size in the eyes (below);
  others (the compass, subtitles, prompts) stay on the panel, where comfort matters more.
- **Letterboxed scenes: a 1-pixel line** in the letterbox colour (45,10,10) across the eye at
  row 136 of 2048, in every letterboxed cutscene and dialog of the run (pairs 28, 30-32, 39).
- **Iwazaru's first dialog**: the flat picture was tinted teal, the eye wasn't (the same-frame
  check compares grey levels, so it scored 0.6%). The second dialog matched; possibly a tint
  caught mid-transition. Unconfirmed.
- **Harman's Room**: the camera looks down from high up at 42°, so in VR the room is a small lit
  spot in the middle third of the view; the spotlight cones end at a hard edge just above the
  original framing (as the pigeon memo's set does). Headset look.
- Fine in VR: the save and load screens (2D, whole on the screen panel, text present), the
  corridors, the ceiling-light glows, the blood splash when a Heaven Smile reaches the persona
  (full screen in the eyes). The fight itself wasn't captured: the Smile reached Kaede during
  aiming; Kaede then has to be swapped on the TV.

## Flat frames (hooks_d3d.cpp, `g_flat_frame`)

Harman's TV draws its picture (the persona's portrait from its own camera, about 33°, not the
inflated one; the channel's text) into the scene target, copies that into a frame-sized texture,
clears the scene target and draws the copy onto the TV's curved glass inside the TV set's
artwork. A frame that does this (a perspective draw, then a draw copying the scene target into
another resized texture, then a clear of the scene target) makes the next frame flat: every draw
goes into both eyes from the game camera (the FOV it meant, or the draw's own projection for
cameras it didn't inflate), 2D draws as issued with their text, and the frame is handed off as a
2D frame (the screen panel). The log says "stereo: flat frames on/off". The game-view pass also
uses a draw's own projection now when its camera wasn't inflated (`g_own_cb`,
`draw_proj_inflated`), so the TV's flat picture keeps the portrait's framing. None of the main
tour's frames match the pattern (2026-11-17: 18/18, no flat frames).

## The junction screen (hooks_d3d.cpp, `draw_junction_projected`)

At a junction the game tints a copy of the frame red over the whole screen (pixel shader
`1da2b9dfa4382813`, `[vr] junction_shaders`), then draws the persona's highlight (another
full-screen filter), a preview inset in its own small viewport, a window per path (a quad that
shows the copy untinted, its texture coordinates are screen positions) with a white outline
strip, and the labels. The rest of the junction's draws run in every gameplay frame, invisible;
the tint only while a junction shows, so it switches the handling on for the rest of the frame
(log: "stereo: junction screen on/off"). In the eyes the full-screen filters cover the whole
view. Every later 2D draw goes into both eyes, never to the HUD panel, as if the game's picture
were a flat sign at infinity in front of the game camera: its vertices are mapped on the CPU
(`vr::game_ndc_to_eye_clip`, a homography: straight lines stay straight, the sign foreshortens
as the head turns), clipped where they fall behind the eye (and to the inset's scissor), and
drawn with the game's own shaders over the whole eye. Outlines and labels sit on the paths they
point at from any head direction (an earlier version stretched them into the game view's
bounding rectangle, which blew them up when looking 45 degrees away).

The windows and their outlines reach the edge of the view: each window is a quad whose near
corners sit by the persona and whose far corners lie just past the game's screen, its outline an
8-vertex strip of corner pairs; their far corners are pushed out along the side edges
(`kJuncReach`). A window samples the frame copy at its screen position: in each eye its texture
coordinates become that eye's own pixel, so its interior is the eye's 3D view. Anything else
that reads the copy (the preview inset, which uses a grey copy) gets a crop of the eye's copy
where the game camera's view lies (`eye_crop`). The junction's letterbox (an untextured draw of
two full-width bars, the top and bottom 64 of 480 lines, `is_letterbox_bars`) is left out of the
eyes. `draw_junction` (the older rectangle mapping) is the fallback for draw shapes the
projection doesn't handle. Same-frame check straight ahead: Harman's Room junction ~2%, lobby
4.9%, first junction ~10% (the faint picture under the red stripes differs, see screens.md).

## Aiming: the world stays still when the hand moves (toolsimcheck.py)

In aim mode the mod turns the game camera with the hand and re-places the eyes so the world
stays where it is; only the gun moves. The first headset session (2026-11-18) found the world
smearing with the hand, its lighting following it, as Kaede. Cause: the arm and gun are found as
the first run of 3D draws whose models sit within `kGunRunRadius` of the camera and follow the
hand; at 25 units Kaede's run took in lobby geometry 16.8 units out (Garcian's 2.0-9.5, Kaede's
2.7-6.7), so part of the room moved with the hand. Now 12 units, and farther draws before the
run (two at 17 units ahead of Garcian's arm) don't end it. Both tours check it: with the
simulated head still, `hand <yaw> <pitch>` moves only the hand, and `toolsimcheck.py` (run by
check.ps1 on the `<name>, hand ...` screens) fails when more than 15% of the eye changes
(Garcian 1-11%, all of it the gun; Kaede 6-7% with her scope's sway; before the fix 39-55%).

## Rumble (src/vr/input.cpp, proxy/xinput.cpp)

The game rumbles through XInputSetState (player 1, both motors the same): each shot is a short
full-strength kick, then a ramp from nothing back up over about a third of a second. The mod's
XInputSetState passes the values on to a real pad and to `vr::set_rumble`; each frame each
controller gets a SteamVR haptic pulse (`TriggerHapticPulse`, at most 3999 us) as long as its
motor's strength (`[vr] rumble` scales it). Seen in the log as "pad: rumble L R". Not yet felt
on a headset (the simulated one has no controllers).

## The save tour

`tools\check.ps1 -Script tour_save` (about 3 minutes) loads the save from CONTINUE and visits
Harman's Room, the TV's channels (`expect 2d` on the Blood Room, the persona and the change
prompt), the room's junction and back half, the lobby and its junction, the Elevator Hall; runs
are kept in `k7vr\tour\tour_save\vr` (or `flat`). 13/13 on 2026-11-17. `capture` now also prints
the four corners of 6-index quads.

## Next steps

1. **Headset look at a junction** (the overlay sits at infinity; the tint fills the whole view).
2. **HUD panel vs the game's FOV** and the 1-pixel letterbox line (above).
3. **Extend the save tour**: the fight (aim and scan before the Smile reaches you; in the tour
   so far it reached Kaede during aiming), game over, FMVs.
4. **After a headset session**, fold the user's findings into `docs\screens.md` and fix what's
   wrong; add checks so fixes stay fixed.

## Working on it

- Test copy: `C:\Users\Virtual\killer7-vr-game`. Launch only through `tools\run.ps1` (minimized,
  muted by the sim/test profiles). Running the game overwrites `k7vr\k7vr.log` and deletes the
  previous `pair_*`, `shot_*`, `capture_*`, `sweep_*` files: copy what you need first.
- Profiles: `tools\profile.ps1 sim` (simulated headset, eye targets 2048x2048), `test` (VR off),
  `playtest` (OpenVR, for the user's headset). Leave the copy on whatever profile it was on
  (normally `playtest`); `tools\check.ps1` puts it back by itself.
- Full check: `tools\check.ps1` (VR tour, ~10 min, exit 1 on a failed `expect`), then
  `tools\check.ps1 -Flat -NoInstall` (VR-off reference, ~10 min), then `python
  tools\catalog.py`. Unit tests: `ctest --test-dir out` and `python -m unittest discover -s
  tests -p "test_*.py"`.
- Publishing the catalogue: build it into a scratchpad folder (`python tools\catalog.py --out
  <dir>`) and publish `<dir>\index.html` with its `img/*.jpg` files to the same artifact URL;
  one publish takes at most 255 files, so send the images in two batches (~265 files).
- Draw-level investigation: `capture` (draw list with matrices, vertices for small non-indexed
  draws, vertex buffer kind), `capture N...` (render target snapshots after draws N), `hudlog N`
  (what goes to the HUD), `pair` (everything a frame hands the headset), `sweep` (look around).
  The pair log line carries eye tangents, the game view's rectangle in the left eye, the game's
  FOV and viewport/letterbox band.

## Gotchas

- Players start the game from Steam, which loads the Steam overlay into it; `run.ps1` alone
  doesn't. The overlay detours every new swap chain's methods as read from its vtable, one saved
  original per method, so the mod hooks only private vtable copies of the game's own factory and
  swap chain, never DXGI's shared vtables (v0.1.1 and older did: a stack overflow before the
  window opened, Steam launches only). Test D3D/DXGI hook changes with `run.ps1 -SteamOverlay`.

- `log` lines are written when the script is read; `mark` lines when their turn comes. Wait on
  `mark` lines (or log timestamps).
- The keyboard is ignored in play once the pad has been used; drive gameplay with the pad.
  Pad: A walk/accept, B turn/back, X map, BACK quit prompt, RB camera, RT aim, LT eye view
  (with RT: scan), START pause menu. Pause menu entries: hold the stick towards the entry, then
  A while holding (`padhold - 0 1`, wait, `padhold A 0 1`, release); plain presses are ignored.
- Title menu: the cursor doesn't start on NEW GAME (two downs from where it starts can hit EXIT,
  which quits). Options: ESC does nothing; leave through "Back to Title Screen"; Space on
  "More..." starts Killer7ConfigurationTool (close it if it appears).
- Timed scenes drift between runs (captures take time, more in VR), so VR and VR-off pictures of
  cutscenes are a few seconds apart; animated 2D screens get flagged by the 2D comparison for
  that (chapter title card, static transition).
- The simulated headset only exercises the eye-size resize when `eye_res` is set (the sim
  profile sets 2048x2048); hand-off bugs that need the real resolution or OpenVR overlays still
  need a headset (see 2026-10-26: the menu copy rebuilt every frame).
- Other Claude sessions use the same test copy; check for a running Killer7Win before launching.
- With a save present, VR-off boots reach PRESS START later than VR runs; take a `shot` at the
  title menu before choosing (a fixed script once started NEW GAME instead of CONTINUE).
- The same-frame check compares grey levels: a lost colour tint scores near 0.
