"""Builds the screen catalogue from tools\\check.ps1's tour runs.

    python tools\\catalog.py [--game C:\\Users\\Virtual\\killer7-vr-game] [--out DIR]

Reads <game>\\k7vr\\tour\\vr (the simulated-headset run: pairs and checks) and, when present,
<game>\\k7vr\\tour\\flat (the same tour with VR off: the reference picture of each screen), plus
docs\\screens.md (a status and notes per screen, written by hand). Writes DIR (default
<game>\\k7vr\\tour\\catalog): index.html and its images, one entry per `screen` in the tour.

Each entry's "headset view" is approximate: the left eye as sent, with the HUD panel and the
screen panel drawn where they float for a head looking straight ahead (the panels' sizes and
distances are vr.cpp's g_hud_panel and g_screen_panel; the eye's field of view comes from the
pair's log line).
"""
import argparse
import html
import math
import re
from pathlib import Path

import numpy as np
from PIL import Image

from imagechecks import (FLAG_CELLS, HEAD_LOCKED_FLAG, ONE_EYE_FLAG, SAME_FRAME_FLAG, SCENE_MOVES_LIMIT, compare,
                         head_locked, one_eye, same_frame, scene_moves, sweep_flags)

REPO = Path(__file__).resolve().parent.parent

# vr.cpp: Panel{width_m, distance_m, drop_m}; pictures inside are 4:3.
HUD_PANEL = (2.13, 2.0, 0.1)
SCREEN_PANEL = (3.2, 3.0, 0.0)
CONTENT_ASPECT = 4 / 3
# vr.cpp fallbacks when a pair's log line has no tangents: sim_fov 100, game_xy_ratio 1.18.
DEFAULT_TAN_Y = math.tan(math.radians(50))
DEFAULT_TAN_X = DEFAULT_TAN_Y * 1.18

STATUSES = {
    "broken": "Broken",
    "wrong": "Looks wrong",
    "unsure": "Needs a look",
    "ok": "Looks right",
}


def slug(name):
    return re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-")


def parse_run(log_path):
    """Screens in tour order: name, pair or shot number, frame kind, eye tangents, checks."""
    screens, pending = [], []
    if not log_path.exists():
        return screens
    for line in log_path.read_text(errors="replace").splitlines():
        m = re.search(r"\] screen: (.*)$", line)
        if m:
            screens.append({"name": m.group(1).strip(), "pair": None, "shot": None, "kind": None,
                            "tan": None, "rect": None, "view": None, "sweep": [], "checks": pending})
            pending = []
            continue
        m = re.search(r"\] expect (\S+): (pass|FAIL)(?:: (.*?))? \((.*)\)$", line)
        if m:
            pending.append({"expect": m.group(1), "pass": m.group(2) == "pass",
                            "why": m.group(3) or "", "stats": m.group(4)})
            continue
        if not screens:
            continue
        cur = screens[-1]
        m = re.search(r"\] pair (\d+) written \((\w+) frame[^;)]*(?:; eye tangents x ([\d.]+) y ([\d.]+))?", line)
        if m and cur["pair"] is None:
            cur["pair"] = int(m.group(1))
            cur["kind"] = m.group(2)
            if m.group(3) and float(m.group(3)) > 0:
                cur["tan"] = (float(m.group(3)), float(m.group(4)))
            r = re.search(r"; game view in left eye (-?\d+) (-?\d+) (-?\d+) (-?\d+)", line)
            if r:
                cur["rect"] = tuple(int(v) for v in r.groups())
            v = re.search(r"; game viewport (-?[\d.]+) (-?[\d.]+) ([\d.]+) ([\d.]+) band (-?\d+) (-?\d+) (-?\d+) (-?\d+)"
                          r" of (\d+) (\d+)", line)
            if v:
                cur["view"] = tuple(float(x) for x in v.groups())
            continue
        m = re.search(r"\] sweep (\d+) yaw (-?[\d.]+) pitch (-?[\d.]+): draws (\d+)", line)
        if m:  # (a sweep belongs to the screen before it; "not 3D" directions are left out)
            cur["sweep"].append({"n": int(m.group(1)), "yaw": round(float(m.group(2))), "pitch": round(float(m.group(3))),
                                 "draws": int(m.group(4))})
            continue
        m = re.search(r"screenshot shot_(\d+)\.png", line)
        if m and cur["shot"] is None:
            cur["shot"] = int(m.group(1))
    return screens


def parse_notes(path):
    """## name sections of docs/screens.md: {lowercased name: (heading, status, [paragraphs])}."""
    notes = {}
    if not path.exists():
        return notes
    name, status, lines = None, None, []

    def flush():
        if name is not None:
            paras = [p.strip() for p in "\n".join(lines).split("\n\n") if p.strip()]
            notes[name.lower()] = (name, status, [" ".join(p.split()) for p in paras])

    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("## "):
            flush()
            name, status, lines = line[3:].strip(), None, []
        elif name is not None and status is None and line.lower().startswith("status:"):
            status = line.split(":", 1)[1].strip().lower()
        elif name is not None:
            lines.append(line)
    flush()
    return notes


def load(path):
    return Image.open(path).convert("RGB") if path.exists() else None


def panel_rect(size, tan, panel):
    """Pixel box (l, t, r, b) of a panel in a WxH eye image, head looking straight ahead."""
    w, h = size
    tan_x, tan_y = tan
    width, dist, drop = panel
    height = width / CONTENT_ASPECT
    x0, x1 = -width / 2 / dist, width / 2 / dist
    y_top, y_bot = (-drop + height / 2) / dist, (-drop - height / 2) / dist
    to_u = lambda t: (0.5 + t / (2 * tan_x)) * w
    to_v = lambda t: (0.5 - t / (2 * tan_y)) * h
    return round(to_u(x0)), round(to_v(y_top)), round(to_u(x1)), round(to_v(y_bot))


def headset_view(run, n, kind, tan):
    """The left eye with the panels drawn in, as a headset would compose it (approximately)."""
    eye = load(run / f"pair_{n:03d}_L.png")
    hud = load(run / f"pair_{n:03d}_panel_hud.png")
    hud_a = load(run / f"pair_{n:03d}_panel_hud_alpha.png")
    screen = load(run / f"pair_{n:03d}_panel_screen.png")
    size = eye.size if eye else (hud or screen or Image.new("RGB", (2048, 2048))).size
    base = np.asarray(eye, dtype=np.float32) if eye else np.zeros((size[1], size[0], 3), np.float32)
    tan = tan or (DEFAULT_TAN_X, DEFAULT_TAN_Y)
    if screen is not None:
        l, t, r, b = panel_rect(size, tan, SCREEN_PANEL)
        pic = np.asarray(screen.resize((r - l, b - t), Image.LANCZOS), dtype=np.float32)
        base[max(t, 0):b, max(l, 0):r] = pic[max(-t, 0):, max(-l, 0):][: b - max(t, 0), : r - max(l, 0)]
    if hud is not None and hud_a is not None:
        l, t, r, b = panel_rect(size, tan, HUD_PANEL)
        rgb = np.asarray(hud.resize((r - l, b - t), Image.LANCZOS), dtype=np.float32)
        a = np.asarray(hud_a.resize((r - l, b - t), Image.LANCZOS), dtype=np.float32)[..., :1] / 255
        region = base[t:b, l:r]
        base[t:b, l:r] = rgb + (1 - a) * region  # the HUD is premultiplied (vr.cpp)
    return Image.fromarray(np.clip(base, 0, 255).astype(np.uint8))


def hud_on_grey(run, n):
    rgb = load(run / f"pair_{n:03d}_panel_hud.png")
    a = load(run / f"pair_{n:03d}_panel_hud_alpha.png")
    if rgb is None or a is None:
        return None
    rgb = np.asarray(rgb, dtype=np.float32)
    a = np.asarray(a, dtype=np.float32)[..., :1] / 255
    if a.max() < 0.01:
        return None  # nothing on the HUD this frame
    return Image.fromarray(np.clip(rgb + (1 - a) * 58, 0, 255).astype(np.uint8))


def save_jpg(img, path, width):
    if img is None:
        return None
    if img.width > width:
        img = img.resize((width, round(img.height * width / img.width)), Image.LANCZOS)
    img.save(path, "JPEG", quality=80, optimize=True)
    return path.name


def build(game, out):
    tour = game / "k7vr" / "tour"
    vr_run, flat_run = tour / "vr", tour / "flat"
    vr = parse_run(vr_run / "k7vr.log")
    flat = {s["name"].lower(): s for s in parse_run(flat_run / "k7vr.log")}
    notes = parse_notes(REPO / "docs" / "screens.md")
    img = out / "img"
    img.mkdir(parents=True, exist_ok=True)
    for old in img.glob("*.jpg"):
        old.unlink()

    entries = []
    for i, s in enumerate(vr, 1):
        key = f"{i:02d}-{slug(s['name'])}"
        e = {"n": i, "name": s["name"], "kind": s["kind"], "checks": s["checks"], "images": []}
        _, status, paras = notes.get(s["name"].lower(), (None, None, []))
        e["status"] = status if status in STATUSES else None
        e["notes"] = paras
        n = s["pair"]
        if n is not None:
            e["view"] = save_jpg(headset_view(vr_run, n, s["kind"], s["tan"]), img / f"{key}-view.jpg", 1200)
            for label, image in (
                ("Left eye as sent", load(vr_run / f"pair_{n:03d}_L.png")),
                ("HUD panel", hud_on_grey(vr_run, n)),
                ("Screen panel", load(vr_run / f"pair_{n:03d}_panel_screen.png")),
                ("Game view (the mod's flat pass)", load(vr_run / f"pair_{n:03d}_game.png")),
            ):
                name = save_jpg(image, img / f"{key}-{slug(label)}.jpg", 800)
                if name:
                    e["images"].append((label, name))
        else:
            e["view"] = None
        # 3D frames: the left eye (cropped to the game's view, HUD over it) against the game's
        # own view of the same frame.
        e["same_frame"] = None
        if n is not None and s["kind"] == "3D" and s["rect"]:
            eye, game = load(vr_run / f"pair_{n:03d}_L.png"), load(vr_run / f"pair_{n:03d}_game.png")
            if eye is not None and game is not None:
                view = None
                if s["view"]:  # the game's viewport and letterbox band, in its frame -> the picture's pixels
                    *vb, fw, fh = s["view"]
                    sx, sy = game.width / fw, game.height / fh
                    view = tuple(round(x * (sx if i % 2 == 0 else sy)) for i, x in enumerate(vb))
                e["same_frame"], diff_map = same_frame(eye, s["rect"], load(vr_run / f"pair_{n:03d}_hud.png"),
                                                       load(vr_run / f"pair_{n:03d}_hud_alpha.png"), game, view)
                if diff_map is not None:
                    name = save_jpg(diff_map, img / f"{key}-differences-from-game-view.jpg", 800)
                    e["images"].append(("Differences from the game's own view", name))
        look_around(e, s, vr_run, img, key)
        f = flat.get(s["name"].lower())
        e["flat"] = None
        e["differs"] = None  # share of cells that differ from VR off (2D and menu frames)
        if f and f["shot"] is not None:
            flat_pic = load(flat_run / f"shot_{f['shot']:03d}.png")
            e["flat"] = save_jpg(flat_pic, img / f"{key}-flat.jpg", 1200)
            panel = load(vr_run / f"pair_{n:03d}_panel_screen.png") if n is not None else None
            if flat_pic is not None and panel is not None and s["kind"] in ("2D", "menu"):
                e["differs"], diff_map = compare(panel, flat_pic)
                name = save_jpg(diff_map, img / f"{key}-differences.jpg", 800)
                e["images"].append(("Differences from VR off", name))
        entries.append(e)
    # Sections of the notes that aren't a screen (cross-cutting findings) go at the top.
    names = {e["name"].lower() for e in entries}
    findings = [(title, paras) for k, (title, _, paras) in notes.items() if k not in names and paras]
    (out / "index.html").write_text(page(entries, bool(flat), findings), encoding="utf-8")
    return entries


LOOK_FLAGS = {
    "head-locked": "Parts fixed to the view",
    "one eye": "Something in one eye only",
    "culled": "Scenery dropped when turning",
}


def look_around(e, s, run, img, key):
    """The sweep's views (a strip) and the look-around flags: head-locked parts (straight ahead
    against 10 degrees right, unless the scene moved on its own between the two straight-ahead
    captures), one-eye parts (the pair's two eyes) and culling."""
    e["sweep"], e["look_flags"] = [], []
    sweep = s["sweep"]
    again = sweep[-1] if len(sweep) > 1 and (sweep[-1]["yaw"], sweep[-1]["pitch"]) == (0, 0) else None
    views = {(v["yaw"], v["pitch"]): v for v in (sweep[:-1] if again else sweep)}
    for v in sweep:
        name = save_jpg(load(run / f"sweep_{v['n']:03d}_L.png"), img / f"{key}-sweep-{v['n']:03d}.jpg", 400)
        if name:
            e["sweep"].append(dict(v, file=name, again=v is again))
    straight, turned = views.get((0, 0)), views.get((10, 0))
    if straight and turned:
        a, b = load(run / f"sweep_{straight['n']:03d}_L.png"), load(run / f"sweep_{turned['n']:03d}_L.png")
        c = load(run / f"sweep_{again['n']:03d}_L.png") if again else None
        e["moving"] = scene_moves(a, c) if a is not None and c is not None else None
        if a is not None and b is not None and (e["moving"] is None or e["moving"] <= SCENE_MOVES_LIMIT):
            tan_x = (s["tan"] or (DEFAULT_TAN_X, DEFAULT_TAN_Y))[0]
            e["head_locked"] = head_locked(a, b, round(a.width * math.tan(math.radians(10)) / (2 * tan_x)))
            if e["head_locked"] > HEAD_LOCKED_FLAG:
                e["look_flags"].append("head-locked")
    if s["pair"] is not None and s["kind"] == "3D":
        left, right = load(run / f"pair_{s['pair']:03d}_L.png"), load(run / f"pair_{s['pair']:03d}_R.png")
        if left is not None and right is not None:
            # Eye separation shifts near things sideways; 2% of the width covers what the tour meets.
            e["one_eye"] = one_eye(left, right, round(left.width * 0.02))
            if e["one_eye"] > ONE_EYE_FLAG:
                e["look_flags"].append("one eye")
    e["look_flags"] += sweep_flags(views)


def sweep_caption(v):
    where = "straight ahead again" if v["again"] else f"yaw {v['yaw']:+d}&deg; pitch {v['pitch']:+d}&deg;"
    return f"{where} &middot; {v['draws']} draws"


def game_flagged(e):
    """Whether a 3D screen's same-frame score flags it (never while SAME_FRAME_FLAG is None)."""
    return SAME_FRAME_FLAG is not None and (e["same_frame"] or 0) > SAME_FRAME_FLAG


def esc(s):
    return html.escape(s, quote=True)


def page(entries, have_flat, findings=()):
    counts = {k: sum(1 for e in entries if e["status"] == k) for k in STATUSES}
    unreviewed = sum(1 for e in entries if not e["status"])
    failed = sum(1 for e in entries if any(not c["pass"] for c in e["checks"]))
    differ = sum(1 for e in entries if (e["differs"] or 0) > FLAG_CELLS)
    differ_game = sum(1 for e in entries if game_flagged(e))
    looked = sum(1 for e in entries if e["look_flags"])
    kind_label = {"3D": "3D", "menu": "Menu", "2D": "2D"}

    cards = []
    for e in entries:
        status = e["status"] or "unreviewed"
        status_text = STATUSES.get(e["status"], "Not reviewed")
        fails = [c for c in e["checks"] if not c["pass"]]
        checks = "".join(
            f'<li class="check {"pass" if c["pass"] else "fail"}"><span class="mono">expect {esc(c["expect"])}</span> '
            f'{"passed" if c["pass"] else "FAILED: " + esc(c["why"])}<span class="stats mono">{esc(c["stats"])}</span></li>'
            for c in e["checks"])
        notes = "".join(f"<p>{esc(p)}</p>" for p in e["notes"]) or '<p class="muted">No notes yet.</p>'
        flagged = (e["differs"] or 0) > FLAG_CELLS
        if e["differs"] is not None:
            checks += (f'<li class="check {"fail" if flagged else "pass"}">'
                       f'{"Differs from" if flagged else "Matches"} the picture with VR off'
                       f'<span class="stats mono">{100 * e["differs"]:.1f}% of cells differ '
                       f'(flagged over {100 * FLAG_CELLS:.0f}%)</span></li>')
        flagged_game = game_flagged(e)
        if e["same_frame"] is not None and SAME_FRAME_FLAG is None:
            checks += (f'<li class="check info">Compared with the game\'s own view of the same frame'
                       f'<span class="stats mono">{100 * e["same_frame"]:.1f}% of cells differ '
                       f'(information only: not yet a reliable flag)</span></li>')
        elif e["same_frame"] is not None:
            checks += (f'<li class="check {"fail" if flagged_game else "pass"}">'
                       f'{"Differs from" if flagged_game else "Matches"} the game\'s own view of the same frame'
                       f'<span class="stats mono">{100 * e["same_frame"]:.1f}% of cells differ '
                       f'(flagged over {100 * SAME_FRAME_FLAG:.0f}%)</span></li>')
        if e.get("moving") is not None and e["moving"] > SCENE_MOVES_LIMIT:
            checks += (f'<li class="check info">Head turned 10&deg;: not checked, the scene moves on its own'
                       f'<span class="stats mono">{100 * e["moving"]:.1f}% of the picture changed between the two '
                       f'straight-ahead views (checked up to {100 * SCENE_MOVES_LIMIT:.0f}%)</span></li>')
        if e.get("head_locked") is not None:
            flag = e["head_locked"] > HEAD_LOCKED_FLAG
            checks += (f'<li class="check {"fail" if flag else "pass"}">Head turned 10&deg;: '
                       f'{"parts stay fixed to the view" if flag else "the picture moves with the world"}'
                       f'<span class="stats mono">{100 * e["head_locked"]:.1f}% of the picture stays put '
                       f'(flagged over {100 * HEAD_LOCKED_FLAG:.0f}%)</span></li>')
        if e.get("one_eye") is not None:
            flag = e["one_eye"] > ONE_EYE_FLAG
            checks += (f'<li class="check {"fail" if flag else "pass"}">Left eye against right eye: '
                       f'{"parts are in one eye only" if flag else "the eyes match"}'
                       f'<span class="stats mono">{100 * e["one_eye"]:.1f}% of the left eye has no match '
                       f'(flagged over {100 * ONE_EYE_FLAG:.0f}%)</span></li>')
        look_chips = "".join(f'<span class="chip status unsure">{esc(LOOK_FLAGS[f])}</span>' for f in e["look_flags"])
        strip = "".join(
            f'<figure class="thumb"><button class="zoom" data-src="img/{v["file"]}" aria-label="Enlarge view yaw {v["yaw"]} pitch {v["pitch"]}">'
            f'<img src="img/{v["file"]}" alt="Head turned yaw {v["yaw"]} pitch {v["pitch"]}" loading="lazy"></button>'
            f'<figcaption class="mono">{sweep_caption(v)}</figcaption></figure>'
            for v in e["sweep"])
        strip_html = (f'<section class="sweep"><h3>Looking around (the views the original never shows)</h3>'
                      f'<div class="thumbs">{strip}</div></section>') if strip else ""
        view = (f'<figure class="main"><button class="zoom" data-src="img/{e["view"]}" aria-label="Enlarge headset view">'
                f'<img src="img/{e["view"]}" alt="Approximate headset view of {esc(e["name"])}" loading="lazy"></button>'
                f'<figcaption>Headset view (approximate)</figcaption></figure>') if e["view"] else \
            '<figure class="main missing"><div>No capture</div></figure>'
        flat = (f'<figure class="main"><button class="zoom" data-src="img/{e["flat"]}" aria-label="Enlarge reference">'
                f'<img src="img/{e["flat"]}" alt="{esc(e["name"])} with VR off" loading="lazy"></button>'
                f'<figcaption>The game with VR off</figcaption></figure>') if e["flat"] else \
            f'<figure class="main missing"><div>{"Not reached with VR off" if have_flat else "No reference run yet"}</div></figure>'
        thumbs = "".join(
            f'<figure class="thumb"><button class="zoom" data-src="img/{name}" aria-label="Enlarge {esc(label)}">'
            f'<img src="img/{name}" alt="{esc(label)}" loading="lazy"></button><figcaption>{esc(label)}</figcaption></figure>'
            for label, name in e["images"])
        cards.append(f"""
<article class="screen" id="s{e['n']}" data-status="{status}" data-failed="{1 if fails else 0}" data-differs="{1 if flagged else 0}" data-differs-game="{1 if flagged_game else 0}" data-look="{1 if e['look_flags'] else 0}">
  <header class="screen-head">
    <span class="num mono">{e['n']:02d}</span>
    <h2>{esc(e['name'])}</h2>
    <span class="chip kind">{kind_label.get(e['kind'], 'Not captured')}</span>
    <span class="chip status {status}">{status_text}</span>
    {'<span class="chip status broken">Check failed</span>' if fails else ''}
    {'<span class="chip status wrong">Differs from VR off</span>' if flagged else ''}
    {'<span class="chip status wrong">Differs from the game view</span>' if flagged_game else ''}
    {look_chips}
  </header>
  <div class="pair">{view}{flat}</div>
  <div class="detail">
    <div class="notes">{notes}{f'<ul class="checks">{checks}</ul>' if checks else ''}</div>
    <div class="thumbs">{thumbs}</div>
  </div>
  {strip_html}
</article>""")

    filters = [("all", "All", len(entries))] + [(k, v, counts[k]) for k, v in STATUSES.items()] + \
        [("unreviewed", "Not reviewed", unreviewed), ("failed", "Check failed", failed),
         ("differs", "Differs from VR off", differ), ("look", "Look-around flags", looked)] + \
        ([("differs-game", "Differs from the game view", differ_game)] if SAME_FRAME_FLAG is not None else [])
    filter_html = "".join(
        f'<button class="filter{" on" if k == "all" else ""}" data-filter="{k}" type="button">{esc(label)} '
        f'<span class="count mono">{n}</span></button>' for k, label, n in filters)
    finding_html = "".join(
        f'<section class="finding"><h2>{esc(title)}</h2>{"".join(f"<p>{esc(p)}</p>" for p in paras)}</section>'
        for title, paras in findings)
    return TEMPLATE.replace("{{FILTERS}}", filter_html).replace("{{CARDS}}", "".join(cards)) \
        .replace("{{TOTAL}}", str(len(entries))).replace("{{FINDINGS}}", finding_html)


TEMPLATE = """<title>Killer7 VR Screens</title>
<meta name="claude:viewport-fit-sides" content="cover">
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Archivo+Narrow:wght@500;700&family=IBM+Plex+Sans:wght@400;600&family=IBM+Plex+Mono:wght@400;500&display=swap">
<style>
/* Layout: one column of screens in tour order; each pairs the headset view with the game's own picture. */
:root {
  --bg: #f3f2ef; --surface: #ffffff; --fg: #141313; --muted: #5d5a57; --line: #d9d6d1;
  --accent: #b3121e;
  --ok: #2f7a3d; --wrong: #b45309; --broken: #c0262d; --unsure: #6b5bb5; --none: #7a7672;
  --shade: rgba(20, 19, 19, 0.86);
  --display: "Archivo Narrow", "Arial Narrow", "Helvetica Neue", Arial, sans-serif;
  --body: "IBM Plex Sans", "Segoe UI", Roboto, Arial, sans-serif;
  --mono: "IBM Plex Mono", Consolas, "Courier New", monospace;
}
@media (prefers-color-scheme: dark) { :root:not([data-theme="light"]) {
  --bg: #0e0d0d; --surface: #181716; --fg: #ece9e4; --muted: #a29d97; --line: #2e2b29;
  --accent: #e0414b; --ok: #5fb46e; --wrong: #e6923a; --broken: #ef5a61; --unsure: #a597f0; --none: #8f8a85;
  --shade: rgba(0, 0, 0, 0.9); color-scheme: dark } }
:root[data-theme="dark"] {
  --bg: #0e0d0d; --surface: #181716; --fg: #ece9e4; --muted: #a29d97; --line: #2e2b29;
  --accent: #e0414b; --ok: #5fb46e; --wrong: #e6923a; --broken: #ef5a61; --unsure: #a597f0; --none: #8f8a85;
  --shade: rgba(0, 0, 0, 0.9); color-scheme: dark }
body { background: var(--bg); color: var(--fg); font: 15px/1.55 var(--body); }
.wrap { max-width: 1180px; margin: 0 auto; padding-block: 28px 64px;
  padding-left: max(16px, env(safe-area-inset-left, 0px)); padding-right: max(16px, env(safe-area-inset-right, 0px)); }
.mono { font-family: var(--mono); font-variant-numeric: tabular-nums; }
.muted { color: var(--muted); }
.masthead { display: grid; gap: 8px; border-bottom: 3px solid var(--accent); padding-bottom: 18px; }
.masthead h1 { font: 700 clamp(30px, 5vw, 46px)/1.05 var(--display); letter-spacing: 0.01em; margin: 0; text-wrap: balance; }
.masthead h1 span { color: var(--accent); }
.masthead p { margin: 0; max-width: 68ch; color: var(--muted); }
.finding { margin-top: 18px; padding: 14px 16px; background: var(--surface); border: 1px solid var(--line);
  border-left: 4px solid var(--accent); max-width: 80ch; }
.finding h2 { font: 700 19px/1.2 var(--display); margin: 0 0 6px; letter-spacing: 0.01em; }
.finding p { margin: 0 0 8px; }
.finding p:last-child { margin-bottom: 0; }
.filters { display: flex; flex-wrap: wrap; gap: 8px; margin: 18px 0 8px; }
.filter { font: 600 13px var(--body); color: var(--fg); background: var(--surface); border: 1px solid var(--line);
  border-radius: 999px; padding: 6px 12px; cursor: pointer; display: inline-flex; gap: 6px; align-items: center; }
.filter .count { color: var(--muted); font-weight: 500; }
.filter.on { background: var(--fg); color: var(--bg); border-color: var(--fg); }
.filter.on .count { color: var(--bg); opacity: 0.75; }
.filter:focus-visible, .zoom:focus-visible { outline: 2px solid var(--accent); outline-offset: 2px; }
.list { display: grid; gap: 28px; margin-top: 18px; }
.screen { display: grid; gap: 14px; padding-top: 18px; border-top: 1px solid var(--line); }
.screen-head { display: flex; flex-wrap: wrap; align-items: baseline; gap: 8px 12px; }
.screen-head h2 { font: 700 24px/1.15 var(--display); margin: 0; letter-spacing: 0.01em; text-wrap: balance; min-width: 0; }
.num { color: var(--accent); font-size: 14px; font-weight: 500; }
.chip { font: 600 11px var(--body); letter-spacing: 0.06em; text-transform: uppercase; padding: 3px 8px;
  border-radius: 4px; border: 1px solid var(--line); color: var(--muted); }
.chip.status { color: var(--surface); border-color: transparent; background: var(--none); }
.chip.status.ok { background: var(--ok); } .chip.status.wrong { background: var(--wrong); }
.chip.status.broken { background: var(--broken); } .chip.status.unsure { background: var(--unsure); }
.pair { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; }
figure { margin: 0; display: grid; gap: 6px; min-width: 0; }
figcaption { font-size: 12px; color: var(--muted); letter-spacing: 0.04em; text-transform: uppercase; }
.zoom { padding: 0; border: 0; background: #000; cursor: zoom-in; display: block; width: 100%; }
.zoom img { display: block; width: 100%; aspect-ratio: 1 / 1; object-fit: contain; max-width: 100%; }
.main.missing div { aspect-ratio: 1 / 1; display: grid; place-items: center; background: var(--surface);
  border: 1px dashed var(--line); color: var(--muted); font-size: 13px; }
.detail { display: grid; grid-template-columns: minmax(0, 1fr) minmax(0, 1fr); gap: 16px; align-items: start; }
.notes { min-width: 0; max-width: 68ch; }
.notes p { margin: 0 0 8px; }
.checks { list-style: none; margin: 8px 0 0; padding: 0; display: grid; gap: 6px; font-size: 13px; }
.check { padding-left: 10px; border-left: 3px solid var(--ok); }
.check.fail { border-left-color: var(--broken); }
.check.info { border-left-color: var(--none); }
.sweep { display: grid; gap: 8px; }
.sweep h3 { font: 600 12px var(--body); letter-spacing: 0.06em; text-transform: uppercase; color: var(--muted); margin: 0; }
.sweep .thumbs { grid-template-columns: repeat(auto-fill, minmax(130px, 1fr)); }
.check .stats { display: block; color: var(--muted); font-size: 12px; overflow-wrap: anywhere; }
.thumbs { display: grid; grid-template-columns: repeat(auto-fill, minmax(120px, 1fr)); gap: 8px; }
.thumb figcaption { font-size: 11px; text-transform: none; letter-spacing: 0; }
.lightbox { position: fixed; inset: 0; background: var(--shade); display: grid; place-items: center; z-index: 20;
  padding: max(16px, env(safe-area-inset-top, 0px)) max(16px, env(safe-area-inset-right, 0px))
           max(16px, env(safe-area-inset-bottom, 0px)) max(16px, env(safe-area-inset-left, 0px)); cursor: zoom-out; }
.lightbox img { max-width: 100%; max-height: 100%; object-fit: contain; }
@media (max-width: 720px) { .pair, .detail { grid-template-columns: 1fr; } }
@media (prefers-reduced-motion: no-preference) { .filter { transition: background 120ms, color 120ms; } }
</style>
<div class="wrap">
  <header class="masthead">
    <h1>Killer7 VR <span>Screens</span></h1>
    <p>{{TOTAL}} screens from the scripted tour, in the order the game reaches them. Left: what the
    simulated headset is handed, with the floating HUD and screen panels drawn where they sit for a
    head looking straight ahead (100&deg; view; approximate). Right: the same moment with VR off,
    which is what the screen should look like. Click any picture to enlarge it.</p>
  </header>
  {{FINDINGS}}
  <nav class="filters" aria-label="Filter screens">{{FILTERS}}</nav>
  <main class="list">{{CARDS}}</main>
</div>
<div class="lightbox" id="lightbox" hidden><img id="lightbox-img" alt=""></div>
<script>
(function () {
  var box = document.getElementById("lightbox"), big = document.getElementById("lightbox-img");
  document.addEventListener("click", function (ev) {
    var z = ev.target.closest(".zoom");
    if (z) { big.src = z.dataset.src; big.alt = z.getAttribute("aria-label") || ""; box.hidden = false; return; }
    if (!box.hidden && ev.target.closest("#lightbox")) { box.hidden = true; big.removeAttribute("src"); }
  });
  document.addEventListener("keydown", function (ev) { if (ev.key === "Escape") box.hidden = true; });
  var screens = document.querySelectorAll(".screen");
  document.querySelectorAll(".filter").forEach(function (b) {
    b.addEventListener("click", function () {
      document.querySelectorAll(".filter").forEach(function (o) { o.classList.toggle("on", o === b); });
      var f = b.dataset.filter;
      screens.forEach(function (s) {
        s.hidden = !(f === "all" || (f === "failed" ? s.dataset.failed === "1" :
                     f === "differs" ? s.dataset.differs === "1" :
                     f === "differs-game" ? s.dataset.differsGame === "1" :
                     f === "look" ? s.dataset.look === "1" : s.dataset.status === f));
      });
    });
  });
})();
</script>
"""


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--game", default=r"C:\\Users\\Virtual\\killer7-vr-game", type=Path)
    ap.add_argument("--out", type=Path)
    args = ap.parse_args()
    out = args.out or args.game / "k7vr" / "tour" / "catalog"
    entries = build(args.game, out)
    print(f"{len(entries)} screens -> {out / 'index.html'}")
    for e in entries:
        if e["differs"] is not None:
            mark = "DIFFERS" if e["differs"] > FLAG_CELLS else "matches"
            print(f"  {e['n']:02d} {e['name']:32s} {mark:8s} {100 * e['differs']:5.1f}% of cells differ from VR off")
        if e["same_frame"] is not None:
            mark = "info" if SAME_FRAME_FLAG is None else "DIFFERS" if game_flagged(e) else "matches"
            print(f"  {e['n']:02d} {e['name']:32s} {mark:8s} {100 * e['same_frame']:5.1f}% of cells differ "
                  f"from the game's own view")
