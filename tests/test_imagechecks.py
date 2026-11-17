"""Tests for tools/imagechecks.py (run from the repo root: python -m unittest discover -s tests -p "test_*.py")."""
import sys
import unittest
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import imagechecks  # noqa: E402


def picture(w=640, h=480, bar=False):
    """A game picture with a few shapes; `bar`: a 0.8-bright band over the bottom 15%."""
    a = np.full((h, w, 3), 30, np.uint8)
    a[60:200, 80:260] = (200, 60, 60)
    a[250:420, 380:600] = (60, 180, 90)
    a[100:160, 400:560] = (240, 240, 240)
    if bar:
        a[int(h * 0.85):, :] = 204
    return Image.fromarray(a)


def eye_with(game, rect, size=(400, 400)):
    """An eye image holding `game` scaled into `rect`, dark elsewhere."""
    eye = Image.new("RGB", size, (10, 10, 10))
    l, t, r, b = rect
    eye.paste(game.resize((r - l, b - t), Image.BOX), (l, t))
    return eye


class SameFrameTest(unittest.TestCase):
    RECT = (100, 100, 300, 250)

    def test_same_frame_matches_when_crop_equals_game(self):
        game = picture()
        share, diff_map = imagechecks.same_frame(eye_with(game, self.RECT), self.RECT, None, None, game)
        self.assertLess(share, 0.01)
        self.assertIsNotNone(diff_map)

    def test_same_frame_flags_missing_bar(self):
        share, _ = imagechecks.same_frame(eye_with(picture(), self.RECT), self.RECT, None, None, picture(bar=True))
        self.assertGreater(share, 0.10)

    def test_same_frame_hud_fills_in(self):
        game = picture(bar=True)
        bar_rgb = np.zeros((480, 640, 3), np.uint8)
        bar_rgb[int(480 * 0.85):, :] = 204
        alpha = np.zeros((480, 640, 3), np.uint8)
        alpha[int(480 * 0.85):, :] = 255
        share, _ = imagechecks.same_frame(eye_with(picture(), self.RECT), self.RECT, Image.fromarray(bar_rgb),
                                          Image.fromarray(alpha), game)
        self.assertLess(share, 0.01)

    def test_same_frame_not_comparable(self):
        rect = (360, 100, 560, 250)  # 80% of it is right of the 400-wide eye
        game = picture()
        self.assertEqual(imagechecks.same_frame(eye_with(game, self.RECT), rect, None, None, game), (None, None))


def textured(w=400, h=300, seed=1):
    """A busy picture: random blocks of colour, so shifted copies are easy to tell apart."""
    rng = np.random.default_rng(seed)
    cells = rng.integers(20, 235, size=(h // 10, w // 10, 3), dtype=np.uint8)
    return np.kron(cells, np.ones((10, 10, 1), np.uint8))


def shifted_left(a, px):
    """`a` moved px to the left (what a world-locked scene does when the head turns right)."""
    b = np.zeros_like(a)
    b[:, : a.shape[1] - px] = a[:, px:]
    return b


class HeadLockedTest(unittest.TestCase):
    def test_head_locked_world_scene(self):
        a = textured()
        share = imagechecks.head_locked(Image.fromarray(a), Image.fromarray(shifted_left(a, 30)), 30)
        self.assertLess(share, 0.05)

    def test_head_locked_overlay(self):
        a = textured()
        b = shifted_left(a, 30)
        # A detailed overlay (text, a portrait) at the same place in both. (A flat one can't be
        # told from flat-shaded world inside its edges, so it only shows at its edges.)
        overlay = textured(seed=7)[:90, :160]
        a[100:190, 120:280] = overlay
        b[100:190, 120:280] = overlay
        share = imagechecks.head_locked(Image.fromarray(a), Image.fromarray(b), 30)
        block = (90 * 160) / (300 * 400)  # the block's share of the (non-black) picture
        self.assertAlmostEqual(share, block, delta=0.03)


class HeadLockedPerspectiveTest(unittest.TestCase):
    def test_head_locked_wide_view_world(self):
        # A wide view turning: the world moves further towards the edges (here 12 px on the
        # left, 24 px on the right, for a predicted 18), and Killer7's flat shading gives little
        # contrast (blocks within +-0.1 of mid grey). None of it is fixed to the view.
        rng = np.random.default_rng(3)
        cells = rng.integers(105, 155, size=(30, 40, 1), dtype=np.uint8).repeat(3, axis=2)
        a = np.kron(cells, np.ones((10, 10, 1), np.uint8))
        b = np.zeros_like(a)
        b[:, :200 - 12] = a[:, 12:200]
        b[:, 200:400 - 24] = a[:, 224:400]
        self.assertLess(imagechecks.head_locked(Image.fromarray(a), Image.fromarray(b), 18), 0.05)


class HeadLockedEdgeTest(unittest.TestCase):
    def test_head_locked_flat_world(self):
        # A flat grey world turning: the strip the world moves in from at the left edge has
        # nothing to match where the world went, but that doesn't make it fixed to the view.
        a = np.full((300, 400, 3), 128, np.uint8)
        b = shifted_left(a, 18)
        b[:, 400 - 18:] = 128  # (flat: what comes in from the right looks the same)
        self.assertLess(imagechecks.head_locked(Image.fromarray(a), Image.fromarray(b), 18), 0.05)


class OneEyeTest(unittest.TestCase):
    def test_one_eye_disparity_ok(self):
        left = textured()
        right = np.roll(left, 8, axis=1)
        self.assertLess(imagechecks.one_eye(Image.fromarray(left), Image.fromarray(right), 12), 0.02)

    def test_one_eye_missing_object(self):
        left = textured(seed=2)
        right = left.copy()
        left[100:160, 150:210] = 255  # a bright square in the left eye only
        self.assertGreater(imagechecks.one_eye(Image.fromarray(left), Image.fromarray(right), 12), 0.02)


class SweepFlagsTest(unittest.TestCase):
    def test_sweep_flags_culled(self):
        views = {(0, 0): {"draws": 100}, (45, 0): {"draws": 50}}
        self.assertEqual(imagechecks.sweep_flags(views), ["culled"])

    def test_sweep_flags_balanced(self):
        views = {(0, 0): {"draws": 100}, (45, 0): {"draws": 90}, (-90, 0): {"draws": 20}}
        self.assertEqual(imagechecks.sweep_flags(views), [])


class SceneMovesTest(unittest.TestCase):
    def test_scene_still(self):
        a = textured()
        self.assertLess(imagechecks.scene_moves(Image.fromarray(a), Image.fromarray(a.copy())), 0.01)

    def test_scene_animating(self):
        a = textured(seed=4)
        b = a.copy()
        b[:, 200:] = textured(seed=5)[:, 200:]  # the right half changed on its own
        # (random against random differs by over CELL_DIFF in about 60% of cells: ~0.3 overall)
        self.assertGreater(imagechecks.scene_moves(Image.fromarray(a), Image.fromarray(b)), 0.2)


if __name__ == "__main__":
    unittest.main()
