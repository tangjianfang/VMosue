# Gesture → Action Map (authoritative)

Developer / QA reference. The normative definition of every gesture:
what landmarks and thresholds trigger it, which `ActionSet` field it
sets, the system action injected, the cross-talk it must avoid, and the
test fixture that proves the row.

Sources: `src/gesture/*` detectors, `src/gesture/GestureStateMachine.cpp`,
`src/input/InputInjector.cpp`. Landmark indices follow MediaPipe Hands
(0=wrist, 4=thumb tip, 5=index MCP, 8=index tip, 12=middle tip).

| # | Gesture | Hand | Trigger (landmarks / semantics) | ActionSet field | Injection | Must NOT also trigger | Fixture |
|---|---------|------|----------------------------------|-----------------|-----------|------------------------|---------|
| 1 | Cursor move | Right | Index-MCP (5) translation × sensitivity, past adaptive dead-zone | `cursorDx`, `cursorDy` | mouse move (relative) | any click | `cursor_move.json` |
| 2 | Left click | Right | dist(4,8) < pinch threshold then > release threshold within the double-click window | `leftClick` | LMB down+up | drag, middle, right | `left_click.json` |
| 3 | Double click | Right | Two qualifying pinch-releases within the system double-click window (`GetDoubleClickTime`, 200–900 ms) | `leftDoubleClick` | LMB ×2 | single click held | `double_click.json` |
| 4 | Left drag | Right | dist(4,8) < pinch held > `holdForDragMs`, then move, then release | `leftDown` → `cursorDx/Dy` → `leftUp` | LMB down, move, up | single/double click | `left_drag.json` |
| 5 | Middle click | Right | dist(4,12) < pinch then > release; suppressed if a left event fired same frame | `middleClick` | MMB down+up | left click | `middle_click.json` |
| 6 | Right click | Right | `world[8].z` < `world[0].z − zThreshold` (approach) then retract within `[minWindowMs, windowMs]`; suppressed if any left/middle event fired same frame | `rightClick` | RMB down+up | left, middle | `right_click.json` |
| 7a | Scroll (vertical) | Left | dist(8,12) below scroll-enter threshold, vertical midpoint motion | `wheel` (+ = up) | wheel | pause | `scroll_vertical.json` |
| 7b | Scroll (horizontal) | Left | as 7a, horizontal midpoint motion | `hWheel` (+ = right) | hwheel (tilt) | pause | `scroll_horizontal.json` |
| 8 | Pause / Resume | Left | Open left hand (4 fingertips above MCPs) held ≥ `holdMs` | state → Paused / Active | (none) | scroll | `pause_toggle.json` |
| 9 | Emergency stop | Either | Both hands open ≥ `twoHandOpenHoldMs` | `safeRelease`, state → EmergencyStopped | release all buttons | pause | `emergency_stop.json` |

## Arbitration rules

- **Left wins.** Within one frame the click detector evaluates the
  thumb-index pinch before the thumb-middle pinch; a left event
  suppresses the middle event (`ClickDetector::OnLandmarks`).
- **Right-click yields to left/middle.** The state machine drops
  `rightClick` if any of `leftClick / leftDoubleClick / leftDown /
  leftUp / middleClick` is set in the same frame
  (`GestureStateMachine.cpp`), preventing a conflicting `leftUp +
  rightClick` injection during a pinch-drag that ends on a forward push.
- **Pause short-circuits.** While `Paused`, only the pause detector runs;
  all other detectors are skipped until resumed.
- **Emergency stop is terminal.** Once `EmergencyStopped`, gestures are
  ignored until the hotkey / restart clears it.

## Thresholds: where each value comes from

- **Adaptive (observed):** pinch / release, scroll enter / exit,
  cursor dead-zone, air-click Z — derived by `AdaptiveController` from
  rolling signal statistics, with cold-start fallback to the v0.4
  constants. See `docs/superpowers/specs/2026-06-17-adaptive-parameters.md`.
- **System:** double-click window = `GetDoubleClickTime()` clamped to
  [200, 900] ms (`DisplayInfo::SystemDoubleClickTimeMs`).
- **Config-fixed:** `holdForDragMs`, air-click `windowMs / minWindowMs /
  cooldownMs`, `twoHandOpenHoldMs`.

Every row above is backed by a test in
`tests/integration/test_action_map.cpp`.
