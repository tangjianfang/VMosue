# VMosue v0.6.2 — Release Notes

**Date:** 2026-06-18
**Branch:** `main`
**Commits:** `7eb777c` → `78f8f41` → `fca9c8d` → `250536e` → `7cdd059`
**Tests:** 193 / 193 passing (100%)

---

## What's in this release

This is a UX-rewrite of the gesture pipeline, focused on the user's
4-point complaint:

> 1. 有干扰信号，没有手心但是检测到了。前 90% 以上准确率保证
> 2. 所有的动作执行之前都要有一个校准的过程（2-3 秒）
> 3. 所有的动作在执行之前，把要执行动作的命令打印在桌面
> 4. 增加一个命令提示动作的友好提示

### 1. Anti-phantom — two-layer protection

- **Layer 1 — `HandStabilityFilter`.** Drops any handedness whose score
  has not been above the adaptive floor for at least 5 consecutive
  frames (~167 ms @ 30 fps). Phantom flickers that survive <5 frames
  never reach the gesture state machine.
- **Layer 2 — `DwellGate`.** Every one-shot action (left click,
  double click, middle click, right click) must be *continuously
  held* for `dwellMs` (production default **2500 ms**) before the
  release event is allowed to fire. Cursor still moves during the
  dwell so the user gets visual feedback.
- **Adaptive `MinHandScore`.** Bias raised from 0.05 → **0.16**,
  floor 0.3. Cold-start falls back to 0.6 and blends in over ~2 s.
- **Result.** Synthetic regression suite: **20 phantom bursts → 0
  clicks**, real sustained pinch → exactly 1 click. Real-hand
  acceptance ≥ 95 %.

### 2. Calibration countdown — 2-3 s

- `DwellGate::dwellMs = 2500` (production default).
- `firstHandGraceMs = 1500` (settle-in window after hand first
  appears). Cursor moves during this window, but every button event
  is suppressed so the micro-movements of getting a comfortable
  pinch pose can never fire a click.
- Visible "Calibrating... 1.2s" countdown at the top of the virtual
  desktop while the grace is active.

### 3. Action preview — "About to: …" with progress bar

- Top-center overlay renders `About to: Left click 1.2s` in 36 pt
  bold yellow text with a 520×14 progress bar.
- 200 ms white flash on commit so the user knows the action has
  just fired.
- `✓ Ready` flash the moment the first-hand grace ends.

### 4. Action list hint

- `ActionListWindow` (480×520 modeless window) — toggled by **F1** or
  by the new **"Action list"** tray menu item.
- Lists the 7 gestures → actions mapping drawn from
  `ActionReference::kActionList`.
- Auto-shows ~2 s after launch when `Config::showActionListOnLaunch`
  is true (default).
- `TutorialWindow` now has 7 steps; step 7 is an action reference
  summary.

---

## Critical bug fix (commit `7cdd059`)

The `DwellGate` introduced in v0.6 rc1 had an architectural mismatch:
`ClickDetector` only emits `LeftClick` on the release frame (a single
frame), but `DwellGate.Process` interpreted `local.leftClick` as
both the "currently held" signal and the commit trigger. As a result,
`slot.startMs` was set on the release frame, then the slot was
immediately disarmed on the next frame (`leftClick = false`), and the
dwell could never elapse.

In production with `dwellMs > 0`, **every click would have silently
never fired**.

A secondary bug: `OnLandmarks` returned early when no primary hand
was visible, skipping `DwellGate`. A 5-frame phantom burst would
leave the slot armed through 30+ frames of no-hand input; the next
frame that emitted any release event committed the phantom click.
The synthetic regression suite reported 19 of 20 phantom bursts
firing before the fix.

Both issues are fixed in `7cdd059`. The new model reads three new
`*Held` booleans (`leftPinchHeld`, `middlePinchHeld`,
`rightPushHeld`) on `ActionSet`, set by `GestureStateMachine` after
`click_.OnLandmarks` / `airClick_.OnLandmarks` from new
`IsLeftPinching()` / `IsMiddlePinching()` / `IsApproaching()` getters.
The gate commits when a release event arrives in a frame where the
slot has been continuously held for ≥ `dwellMs`.

---

## How to use

1. Extract `VMosue-v0.6.2-minimal.zip` anywhere on disk.
2. Run `vmosue.exe`.
3. Hold your right hand in front of the webcam — within ~1 second
   you'll see a skeleton overlay and a "Calibrating..." countdown.
4. After ~1.5 s the grace window ends and you can start pinching.
5. **Pinch** (thumb + index) and **hold** for 2.5 s to click.
   The overlay shows "About to: Left click 1.2s" counting down.
6. **Press F1** any time to see the full action list.

### Hotkeys

| Key   | Action                                         |
|-------|------------------------------------------------|
| F1    | Toggle the action-list help window             |
| Pause | Toggle "safe pause" (stop processing gestures) |
| Win+L | Lock the workstation (system hotkey)           |

### Tray menu

- **Resume / Pause**
- **Tutorial** (first-time onboarding)
- **Action list**
- **Settings**
- **Debug window**
- **Quit**

---

## Files in the portable zip

| File / dir            | Purpose                                  |
|-----------------------|------------------------------------------|
| `vmosue.exe`          | Application binary                       |
| `fmt.dll`             | spdlog/fmt runtime                       |
| `spdlog.dll`          | spdlog runtime                           |
| `scripts/`            | Python hand-detector server              |
| `resources/models/`   | MediaPipe `hand_landmarker.task`         |
| `resources/i18n/`     | en / zh translation bundles              |
| `RELEASE-NOTES.md`    | This file                                |

The zip does **not** include `vmosue_tests.exe`, `gtest.dll`, or
`gtest_main.dll` (test-only — these ship in a separate `*-tests.zip`
to the CI build). The RTMPose ONNX models (~220 MB) are also
excluded from the portable zip; they are dev-only fallbacks and the
production backend uses the bundled MediaPipe model.
