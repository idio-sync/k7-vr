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

## Next steps

1. **Extend the tour past the first junction using the saved game.** The user approved loading
   the test copy's save (slot B, written 2026-10-22, in `<test copy>\k7vr\SavedGames\killer7`)
   on 2026-11-17. How CONTINUE and slot selection work hasn't been explored: boot to the title
   menu (as the tour does: W x4 puts the cursor on NEW GAME, CONTINUE is one below), take
   screenshots at each step. Targets: Harman's room and saving, the Iwazaru hint dialogs,
   combat with Heaven Smiles, game over, FMV cutscenes. Add `screen`/`expect`/`sweep` steps
   for each; keep a separate script (e.g. `tools\scripts\tour_save.txt`) if it starts from
   CONTINUE.
2. **After a headset session**, fold the user's findings into `docs\screens.md` and fix what's
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
