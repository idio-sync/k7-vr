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

- **Harman's TV, persona channels: broken.** VR off shows the persona's face close up across
  the TV, "Serum", "Vitality", the name, a scrolling help line, a "SELECT" box and a
  "<name> Change? Yes / No" dialog. In VR the face is small and seen from further off, and all
  the text is gone (empty green and yellow boxes; the frames count as menus, so the screen panel
  shows the game view without it). Likely cause, unconfirmed: the TV's picture is drawn into a
  4:3 frame-sized target that is then mapped onto the TV mesh; `hk_CreateTexture2D` makes such
  targets at eye size, so the portrait is drawn with the eyes' frustum and the text is taken to
  the HUD panel instead of into the TV picture. A `capture` on the TV would settle it.
- **Harman's TV, Blood Room channel (B):** counted as a 3D frame, so it is split: TV set, dial
  and text on the HUD panel, the 3D room inside the TV loose in the eyes (unframed, at the game
  view's size), the glass's scan curve stretched across the eye. Turning the dial flips between
  this split, the HUD panel (the static) and the screen panel (channel 2 counts as a menu). The
  eye tangent changes while on the TV (x 1.41 -> 1.59) along with the game's projection.
- **Junctions (every one): wrong.** The red striped mask with its window cut-outs and the
  grey picture-in-picture preview are full-screen 2D draws of the eye's picture, so they stay in
  the eyes stretched over the whole 100° view; the white outlines and labels go to the HUD
  panel (56°). In the headset the outlines frame nothing (about half the windows' size) and the
  preview's outline is far from the preview. Same-frame check: 12-23% (flagged).
- **HUD panel against the game's FOV.** The HUD panel is a fixed 56°; the game view is 60° in
  the lobby and 42° in Harman's Room. Overlays that point into the scene (junction lines meeting
  at the persona) line up only when the two angles match. Drawing such overlays at the game
  view's size, or sizing the HUD panel to it each frame, would fix both this and the junctions.
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

## Next steps

1. **Fix the Harman TV** (capture it first: which targets are resized, where the text goes),
   then the junctions and the HUD panel's size against the game view (see above).
2. **A save tour** (`tools\scripts\tour_save.txt`): from CONTINUE as above through Harman's
   Room, the TV channels, a junction and the Elevator Hall; `expect` and `screen` steps as in
   the tour. Then the fight (aim and scan before the Smile reaches you), game over, FMVs.
3. **After a headset session**, fold the user's findings into `docs\screens.md` and fix what's
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
