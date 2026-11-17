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


if __name__ == "__main__":
    unittest.main()
