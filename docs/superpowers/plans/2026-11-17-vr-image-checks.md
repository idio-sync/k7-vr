# VR Image Checks Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Flag 3D screens whose headset picture doesn't match the game's own, and flag views the original camera never shows (head turned) that are broken, so the screen catalogue points at what needs fixing without a headset.

**Architecture:** Two checks, both offline in Python over what the tour saves, with the numbers the DLL logs at capture time. (1) Same frame: the left eye cropped to where the game camera's view lands, with the HUD panel's picture laid over it, against the mod's own flat view of the same frame (`pair_NNN_game.png`). (2) Look-around: at each 3D screen a `sweep` command turns the simulated head through fixed directions, saving the left eye and logging draws and how much of the view nothing was drawn into; Python turns those into flags for culling, head-locked parts and one-eye parts, and a strip of the views for the catalogue.

**Tech Stack:** C++20 DLL (`src/proxy`, `src/vr`), CMake/ctest; Python 3.11 with Pillow and numpy (`tools/`), stdlib `unittest`.

**Spec:** The design agreed in conversation on 2026-11-17, summarised under Background below; reviewed screens and their statuses in `docs/screens.md`.

## Background (the spec)

- 2D and menu frames are already compared with the VR-off run (`tools/catalog.py`: `compare`, GRID 64x48, CELL_DIFF 0.15, FLAG_CELLS 0.03). 3D frames can't be compared across runs (wider view, two eyes, cutscenes drift between runs), so they are compared within one frame.
- The commonest VR fault found (docs/screens.md, "The pattern behind most of the wrong screens"): screen-sized overlays split between the HUD panel (textured parts, small in the middle) and the eyes (untextured bars and dims across the whole view). The same-frame check must flag those screens.
- Looking where the flat camera never points has no reference picture. Automatic flags only for: culling (draws drop / undrawn area grows as the head turns), head-locked parts (pixels that stay put when the head turns), parts in one eye only. Whether a set "should" end there stays a human call: the catalogue shows the views.
- Flags mark a screen "needs a look" in the catalogue; they never fail `tools/check.ps1`.

## Global Constraints

- The DLL is 32-bit (vcvars32), built with `build.cmd`; unit tests run with `ctest --test-dir out` and `python -m unittest discover -s tests -p "test_*.py"` from the repo root.
- Python tools run with the system Python 3.11 (Pillow 11, numpy 2); no new dependencies.
- New log lines are parsed by Python: keep their formats exactly as written in the tasks.
- Captures are made in the simulated headset only (`vr::stereo()`); in flat runs the new commands log and do nothing else.
- Tour time: the sweep may add at most 3 minutes to a VR tour (about 10 minutes now).

## Review Focus

1. A 3D screen whose game camera's view lands partly outside the left eye (head turned, aim mode): the crop must be clamped to the eye image, and a crop smaller than 25% of the game view's area is reported as "not comparable", not flagged.
2. A capture where the HUD panel has nothing on it (alpha all zero): the same-frame composite is just the crop; no error.
3. Fast-moving 3D scenes (cutscene cuts, the persona change effect) between the eye and the game view: they are the same frame, so they must match. A mismatch there is a real routing fault, not noise.
4. A sweep started in a 2D or menu frame (the game left 3D before the sweep ran): it logs `sweep ...: not 3D` per direction and saves nothing; the catalogue shows no strip.
5. The depth buffer cleared to 0 (reversed depth) instead of 1: undrawn pixels are those equal to the recorded clear value, whatever it is.

---

### Task 1: Same-frame comparison of 3D frames

**Files:**
- Create: `tools/imagechecks.py` (pure image checks; `compare` moves here from catalog.py)
- Modify: `src/proxy/hooks_d3d.cpp` (`draw_scissored` ~line 1470, the pair log line in `end_frame`)
- Modify: `tools/catalog.py`
- Test: `tests/test_imagechecks.py`

**Interfaces:**
- Produces (C++, hooks_d3d.cpp): `static bool game_view_in_eye(int eye, int width, int height, int rect[4])`: the left/top/right/bottom pixels where the game camera's intended frustum lands in an eye image of that size, false when inflation is off or the frustum is out of view. `draw_scissored` uses it.
- Produces (log): the pair line gains `; game view in left eye L T R B` (integers, eye-image pixels) before its closing parenthesis.
- Produces (Python, tools/imagechecks.py):
  - `compare(vr_pic: Image, flat_pic: Image) -> tuple[float, Image]` (moved unchanged from catalog.py, with GRID, CELL_DIFF, FLAG_CELLS)
  - `same_frame(eye: Image, rect: tuple[int,int,int,int], hud_rgb: Image | None, hud_alpha: Image | None, game: Image) -> tuple[float | None, Image | None]`: crops `eye` to `rect` (clamped to the image), lays the premultiplied HUD over it (both scaled to `game`'s size), returns `compare(composite, game)`; `(None, None)` when the clamped crop covers under 25% of `rect`'s area.
  - `SAME_FRAME_FLAG: float` (set in Task 2)

- [ ] **Step 1: Write the failing tests** in `tests/test_imagechecks.py` (`unittest`; build images with numpy):
  - `test_same_frame_matches_when_crop_equals_game`: a 400x400 eye whose rect (100,100,300,250) holds a scaled copy of a 640x480 game picture with a few shapes, no HUD → `share < 0.01`.
  - `test_same_frame_flags_missing_bar`: same, but the game picture has a 0.8-brightness bar across its bottom 15% that the eye crop lacks → `share > 0.10`.
  - `test_same_frame_hud_fills_in`: the bar is missing from the eye but present on a HUD (rgb = bar, alpha = 1 on the bar) → `share < 0.01`.
  - `test_same_frame_not_comparable`: rect (350,100,550,250) on the 400-wide eye (75% outside) → `(None, None)`.
- [ ] **Step 2: Run** `python -m unittest tests.test_imagechecks -v` from the repo root. Expected: FAIL (`No module named 'imagechecks'`; the test inserts `tools/` into `sys.path`).
- [ ] **Step 3: Implement `same_frame` and move `compare`** into `tools/imagechecks.py`; catalog.py imports `compare` from it.
- [ ] **Step 4: Run** the tests. Expected: 4 passed.
- [ ] **Step 5: Extract `game_view_in_eye`** from `draw_scissored` (same arithmetic: intended FOV from `fov_inflate_intended()`, ratio from `g_last_proj`, `vr::frustum_scissor` on `g_eyes[eye]`), and append the rect to the pair log line for the left eye's submitted image size.
- [ ] **Step 6: Build and run unit tests:** `build.cmd`, then `ctest --test-dir out`. Expected: `built out\XINPUT1_3.dll`, 100% tests passed.
- [ ] **Step 7: Use it in catalog.py.** For 3D entries with `pair_NNN_L.png`, `pair_NNN_game.png` and a rect in the log: `same_frame(...)` with `pair_NNN_hud.png`/`pair_NNN_hud_alpha.png` (the HUD target, game-frame shaped, not the panel copy), store `e["same_frame"]`, add a "Differences from the game's own view" thumbnail and a check line, a chip "Differs from the game view" above `SAME_FRAME_FLAG`, and a filter.
- [ ] **Step 8: Commit** `feat: compare 3D frames with the game's own view of the same frame`.

### Task 2: Tune the same-frame threshold on the reviewed screens

**Files:**
- Modify: `tools/imagechecks.py` (`SAME_FRAME_FLAG`)
- Modify: `tools/catalog.py` (print the 3D scores like the 2D ones)

**Interfaces:**
- Consumes: Task 1's `same_frame`, the pair log rect.

- [ ] **Step 1: Run a VR tour** with Task 1's DLL: `tools\check.ps1`, then `python tools\catalog.py`. Expected: a same-frame score printed for each 3D screen.
- [ ] **Step 2: Set `SAME_FRAME_FLAG`** midway between the highest score of the 3D screens marked `ok` in docs/screens.md (corridor, camera changed, red flower cutscene, lobby (Dan), persona change effect) and the lowest of the split-overlay screens marked `wrong` (silhouette tutorial, LT eye view, quit to title prompt, static transition, subtitled cutscene, Dan Smith title card). Write both measured values in the constant's comment.
- [ ] **Step 3: Verify the separation.** Expected: every `ok` screen above scores below the flag and every `wrong` screen above scores above it. If the ranges overlap, stop and report the scores (the check isn't usable as designed); don't widen the grid or loosen CELL_DIFF to force it.
- [ ] **Step 4: Commit** `feat: tune the same-frame flag on the reviewed screens`.

### Task 3: The `sweep` command

**Files:**
- Modify: `src/proxy/control.cpp` (command list comment, `run_line`)
- Modify: `src/proxy/common.h` (`Requests`)
- Modify: `src/proxy/hooks_d3d.cpp` (`w_ClearDSV` ~line 2235, `end_frame`)

**Interfaces:**
- Produces (command): `sweep` queues, for each of (0,0) (10,0) (-45,0) (45,0) (-90,0) (90,0) (0,40) (0,-40) as (yaw, pitch): a `view` step, `wait 200`, a capture step; then `view 0 0`. Total about 2 s per sweep.
- Produces (`Requests`): `bool sweep = false; float sweep_yaw = 0, sweep_pitch = 0;`
- Produces (files and log, stereo only): `sweep_NNN_L.png` (the left eye as submitted) and the line `sweep NNN yaw Y pitch P: draws D, undrawn U` (U = share of the scene depth buffer still at its clear value, 3 decimals); in a frame that isn't 3D, `sweep NNN yaw Y pitch P: not 3D` and no file. NNN counts per run from 000.
- Produces (hooks_d3d.cpp): `static float g_scene_depth_clear` (the value of the last `ClearDepthStencilView` with the depth flag on the scene's DSV, default 1.0f) and `static ID3D11Texture2D* g_scene_depth` (the depth texture bound with the scene target this frame, next to `g_scene_tex`).

- [ ] **Step 1: Record the clear value and the depth texture.** In `w_ClearDSV`, when `D3D11_CLEAR_DEPTH` is set and the view's texture is the scene's depth (same size as `g_scene_tex`), set `g_scene_depth_clear`. In `draw_stereo`, where `g_scene_tex` is set, set `g_scene_depth` from `dsv`.
- [ ] **Step 2: Implement `static float undrawn_share(ID3D11Texture2D* depth, float clear)`**: staging readback (as `read_bgra` does), formats R32_TYPELESS/D32_FLOAT/R32_FLOAT read as float, R24G8_TYPELESS/D24_UNORM_S8_UINT as the low 24 bits / 0xFFFFFF; every 4th pixel each way; share equal to `clear` within 1e-6; -1 for other formats (logged once).
- [ ] **Step 3: Parse `sweep`** in `run_line` and handle the capture in `end_frame` after `vr::present`, writing the file and line above.
- [ ] **Step 4: Build and unit-test:** `build.cmd`, `ctest --test-dir out`. Expected: built, 100% passed.
- [ ] **Step 5: Verify in game:** `tools\profile.ps1 sim`, `tools\run.ps1`, `tools\cmd.ps1 -File tools\scripts\new_game.txt`, then after the corridor is reached (`tools\scripts\to_gameplay.txt` timing) `tools\cmd.ps1 sweep`. Expected in `k7vr.log`: 8 `sweep` lines with draws > 0 and undrawn between 0 and 1, the (0,0) undrawn below the (0,40) one; 8 `sweep_*_L.png` files. Then `tools\cmd.ps1 'press START' 'wait 3000' sweep`. Expected: 8 lines ending `: not 3D` and no new files. Then `tools\run.ps1 -Stop` and `tools\profile.ps1 playtest`.
- [ ] **Step 6: Commit** `feat: sweep command: look around a 3D screen, logging draws and undrawn area`.

### Task 4: Look-around flags and strip in the catalogue

**Files:**
- Modify: `tools/imagechecks.py`
- Modify: `tools/catalog.py`
- Modify: `tools/check.ps1` (keep `sweep_*.png` with the run's files)
- Test: `tests/test_imagechecks.py`

**Interfaces:**
- Consumes: Task 3's files and log lines.
- Produces (tools/imagechecks.py):
  - `head_locked(straight: Image, turned: Image, shift_px: int) -> float`: share of GRID cells (over non-black cells of `straight`) that match `turned` better unshifted than shifted by `shift_px` to the left (a world-locked scene moves left in the image when the head turns right).
  - `one_eye(left: Image, right: Image, max_disparity_px: int) -> float`: share of GRID cells of `left` with no match in `right` within ±`max_disparity_px` horizontally (cell difference over CELL_DIFF at every offset).
  - `sweep_flags(views: dict[tuple[int,int], dict]) -> list[str]`: from per-direction `{"draws": int, "undrawn": float}`: "culled" when any direction within ±45° yaw has draws under 0.6x those at (0,0); "set ends" when undrawn at any direction exceeds that at (0,0) by more than 0.25. Returns the flag names found.
  - Constants with their measured basis in comments: `HEAD_LOCKED_FLAG = 0.15`, `ONE_EYE_FLAG = 0.05` (re-check both against the tour in Step 6).
- The shift for 10° of yaw: `shift_px = round(width * tan(10°) / (2 * tan_x))` with tan_x from the pair log.

- [ ] **Step 1: Write the failing tests:**
  - `test_head_locked_world_scene`: a textured 400x300 image and the same image shifted 30 px left → `head_locked(a, b, 30) < 0.05`.
  - `test_head_locked_overlay`: same, but a white block at the same place in both → share ≈ the block's share of non-black cells (within 0.03).
  - `test_one_eye_disparity_ok`: right = left shifted 8 px → `one_eye(l, r, 12) < 0.02`.
  - `test_one_eye_missing_object`: right lacks a bright 60x60 square present in left → `one_eye(l, r, 12) > 0.02`.
  - `test_sweep_flags`: `{(0,0): {"draws": 100, "undrawn": 0.1}, (45,0): {"draws": 50, "undrawn": 0.1}}` → `["culled"]`; undrawn 0.5 at (0,40) → contains `"set ends"`; balanced values → `[]`.
- [ ] **Step 2: Run** `python -m unittest tests.test_imagechecks -v`. Expected: the new tests FAIL (not defined).
- [ ] **Step 3: Implement** the three functions and two constants.
- [ ] **Step 4: Run** the tests. Expected: all pass.
- [ ] **Step 5: Catalogue:** check.ps1 also moves `sweep_*_L.png`; catalog.py parses `sweep` lines (they follow the 3D `screen` they belong to), shows a strip of the 8 views (labelled "yaw Y pitch P · draws D · undrawn U%"), runs `head_locked` on (0,0)/(10,0), `one_eye` on the pair's L/R, `sweep_flags`, and shows each flag as a "needs a look" chip with a filter "Look-around flags".
- [ ] **Step 6: Commit** `feat: look-around flags and views in the screen catalogue`.

### Task 5: Sweep in the tour, rerun, publish

**Files:**
- Modify: `tools/scripts/tour.txt` (a `sweep` after the `screen` line of each 3D screen: opening cutscene, corridor, aim, camera changed, pigeon memo, cutscene starts, red flower cutscene, lobby (Dan), junction choice)
- Modify: `README.md` (check.ps1 and catalog.py paragraphs: the sweep and the new flags)
- Modify: `docs/screens.md` (notes for what the sweep shows)

- [ ] **Step 1: Add the sweeps** to the tour, and the README lines.
- [ ] **Step 2: Run both tours and the catalogue:** `tools\check.ps1`, `tools\check.ps1 -Flat -NoInstall`, `python tools\catalog.py`. Expected: all `expect` checks pass except ones already explained in docs/screens.md, 37 screens captured both times, the tour under 13 minutes, and a strip for each swept screen.
- [ ] **Step 3: Check the flags against the pictures.** Expected: the pigeon memo carries "set ends" (its top third is undrawn); LT eye view and the title card carry "head-locked" if their dims are in the sweep's frames; ok screens like the corridor carry none. Record any flag on an ok screen in docs/screens.md, and adjust a constant only with the measured values written beside it.
- [ ] **Step 4: Commit** `feat: sweep each 3D screen in the tour` and publish the catalogue page (same artifact URL).
