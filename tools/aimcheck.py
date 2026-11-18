"""Checks that moving the hand while aiming leaves the world still in the eyes.

In aim mode the mod turns the game's camera to follow the hand; the eyes must keep showing the
world where it is (only the gun moves). The tours capture a screen named `<name>` while aiming
with the simulated hand straight ahead, then `<name>, hand <where>` with only the hand moved
(the head stays still). This compares each such pair's left eye: the share of pixels that
changed by more than CHANGE_LEVEL. The gun alone changes a few percent; a world that swings
with the hand changes most of the picture.

    python tools\\aimcheck.py <run dir>    (k7vr\\tour\\vr or k7vr\\tour\\<script>\\vr)
Prints one line per pair; exit code 1 when any pair changed more than LIMIT.
"""
import re
import sys
from pathlib import Path

import numpy as np
from PIL import Image

CHANGE_LEVEL = 24  # per-channel difference that counts as changed (0..255)
LIMIT = 0.15       # share of the picture allowed to change (the gun: ~3-6%)


def changed_share(a, b):
    """Share of pixels whose largest channel difference exceeds CHANGE_LEVEL."""
    x = np.asarray(a.convert("RGB"), dtype=np.int16)
    y = np.asarray(b.convert("RGB"), dtype=np.int16)
    if x.shape != y.shape:
        return 1.0
    return float((np.abs(x - y).max(axis=2) > CHANGE_LEVEL).mean())


def pairs(log_text):
    """(base screen index, moved screen index, moved screen name) for each `<name>, hand ...`."""
    names = re.findall(r"\] screen: (.*)", log_text)
    index = {}
    out = []
    for i, name in enumerate(n.strip() for n in names):
        m = re.match(r"^(.*), hand .+$", name)
        if m and m.group(1) in index:
            out.append((index[m.group(1)], i, name))
        index.setdefault(name, i)
    return out


def main(run):
    log = (run / "k7vr.log").read_text(encoding="utf-8", errors="replace")
    worst = 0.0
    found = pairs(log)
    for base, moved, name in found:
        a, b = run / f"pair_{base:03d}_L.png", run / f"pair_{moved:03d}_L.png"
        if not a.exists() or not b.exists():
            print(f"{name}: no pair images")
            worst = 1.0
            continue
        share = changed_share(Image.open(a), Image.open(b))
        worst = max(worst, share)
        print(f"{name}: {share * 100:.1f}% of the eye changed ({'FAIL' if share > LIMIT else 'pass'})")
    if not found:
        print("no aim hand screens in this run")
    return 1 if worst > LIMIT else 0


if __name__ == "__main__":
    sys.exit(main(Path(sys.argv[1])))
