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

# 3D screens are flagged when over this share of cells differ from the game's own view of the
# same frame (None would show the score as information only). Measured on the 2026-11-17 tours,
# comparing cutscenes within their letterbox band: screens that look right 0-4.9%; the split
# overlays before their fix 9.2% (subtitles) to 13.2% (title card); the persona change 16.5%
# (its stripe effect, softened in VR by de-striping). The silhouette tutorial's split header
# scored 3.4%, too small to catch.
SAME_FRAME_FLAG = 0.08


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


# Look-around (`sweep`). A screen is flagged when over HEAD_LOCKED_FLAG of its picture stays put
# when the head turns 10 degrees, or over ONE_EYE_FLAG of the left eye has no match in the right.
HEAD_LOCKED_FLAG = 0.15
ONE_EYE_FLAG = 0.05
# Head-locked isn't scored when over this share of the picture changed between the sweep's two
# straight-ahead captures (the scene moves on its own). Measured on the 2026-11-17 tour: still
# scenes 0-0.6%, the street cutscene's moving camera 13.5%, a camera still settling after a
# change 10.2%.
SCENE_MOVES_LIMIT = 0.05
# Pictures are compared at WORK size: GRID cells of 4x4 pixels.
WORK = (GRID[0] * 4, GRID[1] * 4)


def _grey(img):
    return np.asarray(img.convert("L").resize(WORK, Image.BOX), dtype=np.float32) / 255


def _cells(diff):
    """Per-pixel differences at WORK size -> their mean per GRID cell."""
    h, w = diff.shape
    return diff[: h - h % 4, : w - w % 4].reshape(h // 4, 4, w // 4, 4).mean(axis=(1, 3))


def head_locked(straight, turned, shift_px):
    """Share of the picture that stays put when the head turns: `turned` is the view with the
    head turned right, where the world moved left, shift_px at the centre (more towards the
    edges of a wide view, so half to twice that is searched). Only cells that clearly decide
    count: ones that match where they were and not where the world went (fixed to the view),
    and ones that match where the world went and not where they were (the world). Flat cells
    match both and are left out, as are black ones."""
    a, b = _grey(straight), _grey(turned)
    s0 = max(1, round(shift_px * WORK[0] / straight.width))
    stay = _cells(np.abs(a - b))
    moved = np.full(stay.shape, np.inf, np.float32)
    for s in range(max(1, s0 // 2), 2 * s0 + 1):
        diff = np.full_like(a, 1.0)
        diff[:, s:] = np.abs(a[:, s:] - b[:, :-s])
        moved = np.minimum(moved, _cells(diff))
    margin = CELL_DIFF / 3
    lit = _cells(a) > 0.02
    lit[:, : -(-2 * s0 // 4)] = False  # the strip the world moves in from: nothing to match where it went
    locked = lit & (stay < margin) & (moved - stay > margin)
    world = lit & (moved < margin) & (stay - moved > margin)
    return float(locked.sum() / max((locked | world).sum(), 1))


def one_eye(left, right, max_disparity_px):
    """Share of the left eye's cells that have no match in the right eye within
    +-max_disparity_px sideways."""
    a, b = _grey(left), _grey(right)
    d_max = max(1, round(max_disparity_px * WORK[0] / left.width))
    best = np.full(_cells(a).shape, np.inf, np.float32)
    for d in range(-d_max, d_max + 1):
        diff = np.full_like(a, 1.0)
        if d >= 0:
            diff[:, : a.shape[1] - d] = np.abs(a[:, : a.shape[1] - d] - b[:, d:])
        else:
            diff[:, -d:] = np.abs(a[:, -d:] - b[:, : a.shape[1] + d])
        best = np.minimum(best, _cells(diff))
    return float((best > CELL_DIFF).mean())


def sweep_flags(views):
    """Flags from a sweep's {(yaw, pitch): {"draws": n}}: "culled" when a direction within 45
    degrees of straight ahead draws under 0.6x what straight ahead does. (Where a set simply
    ends isn't flagged: neither the depth buffer, every sample of which holds geometry, nor
    the share of black, which Killer7's dark sets have anyway, told it apart. The views are
    there to look at.)"""
    base = views.get((0, 0))
    if not base:
        return []
    if any(abs(yaw) <= 45 and (yaw, pitch) != (0, 0) and v["draws"] < 0.6 * base["draws"]
           for (yaw, pitch), v in views.items()):
        return ["culled"]
    return []


def scene_moves(first, again):
    """Share of the picture (cells that aren't black) that changed between two captures from
    the same direction: how much the scene moves on its own (animation, a moving camera)."""
    a, b = _grey(first), _grey(again)
    lit = _cells(a) > 0.02
    changed = lit & (_cells(np.abs(a - b)) > CELL_DIFF)
    return float(changed.sum() / max(lit.sum(), 1))


def same_frame(eye, rect, hud_rgb, hud_alpha, game, view=None):
    """A 3D frame as the headset shows it against the game's own view of the same frame: `eye`
    cropped to `rect` (where the game camera's view lands in it, clamped to the image), with the
    HUD (premultiplied colour and its alpha, game-frame shaped) laid over, compared with `game`.
    `view` = the game's viewport (x, y, w, h) and the band it shows (left, top, right, bottom),
    in `game`'s pixels: cutscenes draw their shot into a letterboxed band, which the eyes stretch
    over the whole view, so only the band is compared. (None, None) when the clamped crop keeps
    less than MIN_CROP of `rect`."""
    if view:
        vx, vy, vw, vh, bl, bt, br, bb = view
        bl, bt, br, bb = max(bl, vx), max(bt, vy), min(br, vx + vw), min(bb, vy + vh)
        if vw <= 0 or vh <= 0 or br <= bl or bb <= bt:
            return None, None
        l, t, r, b = rect
        rect = (round(l + (bl - vx) / vw * (r - l)), round(t + (bt - vy) / vh * (b - t)),
                round(l + (br - vx) / vw * (r - l)), round(t + (bb - vy) / vh * (b - t)))
        if hud_rgb is not None and hud_alpha is not None:
            sx, sy = hud_rgb.width / game.width, hud_rgb.height / game.height
            hbox = (round(bl * sx), round(bt * sy), round(br * sx), round(bb * sy))
            hud_rgb, hud_alpha = hud_rgb.crop(hbox), hud_alpha.crop(hbox)
        game = game.crop((bl, bt, br, bb))
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
