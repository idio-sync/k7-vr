# Screen notes

What each screen of the tour (`tools\scripts\tour.txt`) looks like in the headset, and what
needs fixing. `python tools\catalog.py` puts these next to the pictures; each `## name` must
match a `screen` line in the tour. `Status:` is one of `broken` (a part is missing or unusable),
`wrong` (all there, but laid out or placed wrongly), `unsure` (needs a headset look or a
decision) or `ok`.

Reviewed 2026-11-17 from the simulated headset (100° vertical view, head straight ahead) against
the same tour with VR off.

## Split overlays (fixed 2026-11-17)

Screen-sized 2D overlays used to be split: their textured parts (text, portraits, icons) went
to the HUD panel, small in the middle of the view, and their untextured parts (dims, prompt
boxes, frame lines) stayed in the eye images, stretched across the whole view. Now every
untextured 2D quad after the 3D scene that isn't a full-screen fade goes to the HUD panel with
the overlay's text, in the game's order (the mod reads each quad's corners from the game's
vertex buffer to tell a fade, over 95% of the screen, from the rest). The HUD panel grew to the
screen panel's angle (2.13 m wide at 2 m, about 56°), so overlays made for the full screen are
1.5 times larger than before. Confirmed on the LT eye view, the quit prompt, the silhouette
tutorial's header and the opening cutscene's subtitles. Cutscene letterbox bars aren't drawn in
VR: they are the background colour around the cutscene's picture, not quads, so subtitles float
over the scene without their dark red bar (readability is a headset check).

## Cutscene close-ups don't line up with the game's own view

Each 3D screen is also compared with the mod's own flat picture of the same frame (the left eye
cropped to where the game camera's view lands, the HUD laid over). In play they agree (0.2-4%
of the picture differs), but in cutscene close-ups the eye inside that crop shows a wider shot
than the game's view at the same zoom: Garcian whole and about three times smaller where the
game shows his face (16-33% differs). Either cutscenes reach the eyes differently, or the crop is
placed wrongly when the camera zooms; not investigated yet. Until then the score is shown as
information only.

## language select
Status: ok

Flat on the screen panel, the same as with VR off.

## content warning
Status: ok

## logos
Status: ok

## press start
Status: ok

## title menu
Status: ok

The cursor differs from the VR-off run (it starts wherever it was), nothing else.

## title options
Status: ok

The TV shows its green options text, bent onto the curved screen as with VR off (fixed
2026-11-17; 2D comparison 9.7% -> 0.0%). The game draws the text flat, copies the frame, and
draws the copy onto the TV's curved screen mesh; the mod skipped those copy draws as "a backdrop
made from last frame's image", a rule meant for 3D frames. It now applies only while 3D frames
are being drawn. Note: "More..." starts the separate configuration tool.

## difficulty
Status: ok

## silhouette tutorial
Status: ok

The laser and silhouette are in 3D; the "TARGET: 00 Angel" bar is whole on the HUD panel with
its red square (it was split before the overlay fix).

## moon red
Status: ok

## moon blue
Status: ok

## opening cutscene
Status: unsure

3D and head-tracked. The VR-off picture is usually a few seconds off (the two runs drift apart
in cutscenes), so compare the scene, not the moment. Wider than the original framing: watch for
set edges.

## opening cutscene, later
Status: unsure

As above.

## opening cutscene, subtitles
Status: unsure

The subtitles are on the HUD panel, in its lower part, 1.5 times larger than before the overlay
fix ("Our information tells us that there's 14 of 'em" in a targeted run). There's no dark red
bar behind them in VR (it is the background around the cutscene's letterboxed picture), so check
in the headset whether white text over the scene reads well enough or needs a backing.

## chapter title card
Status: ok

The Japanese title card ("a free-for-all fight in the Celtic building") is a 2D frame on the
screen panel. If it's flagged as differing from VR off, look at both pictures: its text fades
in, and the two runs reach it a moment apart.

## corridor
Status: ok

Garcian's first corridor in 3D. The compass/eye icon is on the HUD panel, near the middle.

## LT eye view
Status: ok

Holding LT (the left grip) dims the scene and shows Garcian's name and portrait. The dim, its
frame lines, the name and the portrait are whole on the HUD panel (the dim was in the eyes,
across the view, before the overlay fix; same-frame difference 12.7% -> 1.5%).

## aim
Status: unsure

First person with the gun in 3D. There is no reticle in VR, where the game shows an orange
target ring and crosshair lines. That part is deliberate: aim mode masks the HUD's centre band
(`aim_hud_mask`) because the controller aims. But bb07915 also dropped the aim laser, so
nothing marks where a shot will land. Decide whether a dot where the barrel's ray meets the
scene is wanted.

## aim with scan
Status: unsure

As aim; the scan lines that cross the flat picture don't show in VR either.

## camera changed
Status: ok

## map
Status: unsure

Flat on the screen panel and complete, but small: the panel is 3.2 m wide at 3 m (about 56°),
and the map's labels are fine print at that size. Check legibility in the headset.

## quit to title prompt
Status: ok

"Return to title screen?", Yes/NO and the prompt's dark red box are whole on the HUD panel (the
box was a band across the bottom of the eye view before the overlay fix; same-frame difference
10.1% -> 1.9%).

## pause menu
Status: ok

Whole on the screen panel (b1b70f6), identical to VR off.

## Smiths page (Garcian)
Status: ok

## Items page
Status: ok

## photo viewer
Status: ok

## Memos page
Status: ok

## pigeon memo
Status: wrong

A 3D scene (curtains, the pigeon). The set only covers the original framing: above it, the top
third of the wider VR view is black. A case of looking where the flat camera never points: the
look-around strip shows how far it goes (no check detects it; the set is dark by design).

## Blood page
Status: ok

## cutscene starts
Status: unsure

3D; the letterbox bars of the original aren't in the VR view (good or bad depends on whether
cutscenes should be framed at all in VR).

## static transition
Status: wrong

The static between scenes. When the tour catches it as a 2D frame it's on the screen panel, as
with VR off; when caught over a 3D frame it shrinks to a rectangle in the middle of the view (a
textured full-screen effect, so it goes to the HUD panel). Not covered by the overlay fix, which
handles untextured quads; full-screen textured effects need the same "fade" treatment.

## subtitled cutscene
Status: unsure

As the opening cutscene's subtitles: text on the HUD panel, larger since the overlay fix, no bar
behind it in VR. The tour's capture often falls between lines.

## red flower cutscene
Status: ok

The petals and the scene are 3D and fine.

## Dan Smith title card
Status: unsure

Before the overlay fix a dark band crossed the whole eye view with "Dan Smith" small in the
middle. The band is an untextured quad, so it should now be on the HUD panel with the name; the
tour's capture since then fell just before the card, so it isn't confirmed.

## lobby (Dan)
Status: ok

## Smiths page (persona list)
Status: ok

The persona list (Dan, Kaede, Kevin, Coyote, Con, Mask) whole on the screen panel.

## persona change effect
Status: ok

## junction choice
Status: unsure

Reached with VR off (Kaede at the first junction: "Hallway" and "Iwazaru" labels, arrows, a
picture-in-picture view); the VR run, slowed by its captures, was still a moment earlier, with
Kaede in the lobby. The tour now waits longer before this capture. This screen is where
traversal looked wrong in the first headset session, so it's the next one to look at.
