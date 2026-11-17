# Screen notes

What each screen of the tour (`tools\scripts\tour.txt`) looks like in the headset, and what
needs fixing. `python tools\catalog.py` puts these next to the pictures; each `## name` must
match a `screen` line in the tour. `Status:` is one of `broken` (a part is missing or unusable),
`wrong` (all there, but laid out or placed wrongly), `unsure` (needs a headset look or a
decision) or `ok`.

Reviewed 2026-11-17 from the simulated headset (100° vertical view, head straight ahead) against
the same tour with VR off.

## The pattern behind most of the "wrong" screens

Screen-sized 2D overlays are split. Their textured parts (text, portraits, icons) go to the HUD
panel, 1.4 m wide at 2 m: small, in the middle of the view. Their untextured parts (letterbox
bars, dim bands, prompt boxes, flashes) stay in the eye images, where they stretch across the
whole view and turn with the head. So subtitles float small in mid-scene without their bar, a
title card's dim band crosses the whole view with its name tiny in the middle, and a prompt's box
sits at the bottom of the view while its text is in the centre. The pause menu had the same
problem until b1b70f6 moved menus whole onto the screen panel; these overlays draw too few
untextured quads to count as menus (see kMenuQuads).

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
Status: broken

The TV's picture is empty in VR: the green options text (Reverse Controls, Rumble, More...,
Back to Title Screen) and the ticker never appear, though the menu still works blind. Likely the
text is drawn into an off-screen texture first, and resizing the game's targets for the eyes
breaks that pass. The tour's 2D comparison flags it (about 10% of the picture differs).

## difficulty
Status: ok

## silhouette tutorial
Status: wrong

The laser and silhouette are in 3D and fine, but the "TARGET: 00 Angel" bar is on the HUD panel,
small in the middle of the view, while the small red square from the same corner stays at the
eye's top right. One overlay, two places.

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
Status: wrong

The subtitles and their dark red bar don't read as in the original: the text is small in the
middle of the view (HUD panel) and the bar is drawn across the eye. See the pattern above.

## chapter title card
Status: ok

The Japanese title card ("a free-for-all fight in the Celtic building") is a 2D frame on the
screen panel. If it's flagged as differing from VR off, look at both pictures: its text fades
in, and the two runs reach it a moment apart.

## corridor
Status: ok

Garcian's first corridor in 3D. The compass/eye icon is on the HUD panel, near the middle.

## LT eye view
Status: wrong

Holding LT (the left grip) dims the scene and shows Garcian's name and portrait. In VR the grey
dim covers the upper left of the whole view, in the eyes, while the name and portrait are small
on the HUD panel: the same split as the menus had. (It no longer switches to the flat screen:
c9963a3.)

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
Status: wrong

"Return to title screen?" and Yes/NO are small in the middle (HUD panel); the prompt's dark red
box is a band across the bottom of the eye view, well below them.

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

The look-around flags it as fixed to the view (about 25%). Most likely a false flag: the
curtains are rows of repeating vertical stripes, which match themselves after a small turn.
The strip's views show the curtains moving with the world at 45 and 90 degrees.

## Blood page
Status: ok

## cutscene starts
Status: unsure

3D; the letterbox bars of the original aren't in the VR view (good or bad depends on whether
cutscenes should be framed at all in VR).

## static transition
Status: wrong

The full-screen static between scenes shrinks to a rectangle in the middle of the view (it is
drawn from the last frame's image and goes to the HUD panel), over the head-tracked scene.

## subtitled cutscene
Status: wrong

"My friends are all dead" is small in the middle of the scene (HUD panel), without its bar. See
the pattern above.

## red flower cutscene
Status: ok

The petals and the scene are 3D and fine.

## Dan Smith title card
Status: wrong

A dark band crosses the whole eye view and "Dan Smith" is small in the middle of it (HUD panel);
the original is a letterboxed close-up with the name.

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
