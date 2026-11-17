"""Image checks for the screen catalogue (tools/catalog.py): does what the headset gets match
what the game draws?"""
import numpy as np
from PIL import Image

# Pictures are compared at GRID cells; a cell differs when its brightness is off by more than
# CELL_DIFF. 2D and menu screens are flagged when over FLAG_CELLS of the cells differ from the
# VR-off run. Measured on the first tours: matching screens 0-1% (a menu cursor in another
# place), the Options TV missing its text in VR 10%.
GRID = (64, 48)
CELL_DIFF = 0.15
FLAG_CELLS = 0.03

# A crop that keeps less than this share of the game camera's view isn't compared.
MIN_CROP = 0.25

# 3D screens would be flagged when over this share of cells differ from the game's own view of
# the same frame. None: not flagged, the score is shown as information only. Measured on the
# reviewed screens (2026-11-17 tour) the ranges overlap: screens that look right scored 0.2-3.9%
# in play but 16.6% (persona change) and 28% (red flower cutscene) in cutscenes, where the eye's
# crop shows a wider shot than the game's view at the same zoom (unexplained); screens that look
# wrong scored 2.7% (Dan Smith title card, dark) to 12.5% (LT eye view).
SAME_FRAME_FLAG = None


def compare(vr_pic, flat_pic):
    """How far a picture the headset gets is from the game's: (share of cells that differ,
    difference map: the game's picture dimmed, differing cells in red)."""
    a = np.asarray(vr_pic.convert("L").resize(GRID, Image.BOX), dtype=np.float32) / 255
    b = np.asarray(flat_pic.convert("L").resize(GRID, Image.BOX), dtype=np.float32) / 255
    off = np.abs(a - b) > CELL_DIFF
    base = np.asarray(flat_pic.convert("RGB").resize((960, 720), Image.LANCZOS), dtype=np.float32) * 0.45
    mask = np.asarray(Image.fromarray((off * 255).astype(np.uint8)).resize((960, 720), Image.NEAREST)) > 0
    base[mask] = base[mask] * 0.4 + np.array([230, 40, 40], np.float32) * 0.6
    return float(off.mean()), Image.fromarray(base.astype(np.uint8))


def same_frame(eye, rect, hud_rgb, hud_alpha, game):
    """A 3D frame as the headset shows it against the game's own view of the same frame: `eye`
    cropped to `rect` (where the game camera's view lands in it, clamped to the image), with the
    HUD (premultiplied colour and its alpha, game-frame shaped) laid over, compared with `game`.
    (None, None) when the clamped crop keeps less than MIN_CROP of `rect`."""
    l, t, r, b = rect
    w, h = eye.size
    cl, ct, cr, cb = max(l, 0), max(t, 0), min(r, w), min(b, h)
    if r <= l or b <= t or cr <= cl or cb <= ct or (cr - cl) * (cb - ct) < MIN_CROP * (r - l) * (b - t):
        return None, None
    canvas = Image.new("RGB", (r - l, b - t))  # the part of the view outside the eye stays black
    canvas.paste(eye.convert("RGB").crop((cl, ct, cr, cb)), (cl - l, ct - t))
    comp = np.asarray(canvas.resize(game.size, Image.LANCZOS), dtype=np.float32)
    if hud_rgb is not None and hud_alpha is not None:
        rgb = np.asarray(hud_rgb.convert("RGB").resize(game.size, Image.LANCZOS), dtype=np.float32)
        a = np.asarray(hud_alpha.convert("L").resize(game.size, Image.LANCZOS), dtype=np.float32)[..., None] / 255
        comp = rgb + (1 - a) * comp
    return compare(Image.fromarray(np.clip(comp, 0, 255).astype(np.uint8)), game)
