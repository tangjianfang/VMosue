# Gesture→Action Map, Per-Action Verification, and Latency Work

**Date:** 2026-06-17
**Status:** Approved (design); implementation plan to follow
**Owner:** tangjianfang

## Problem

Three related problems, reported from real use:

1. **No authoritative gesture→action table.** There is no single
   source of truth that says, precisely, *which hand pose / motion
   produces which mouse action*, with the landmark indices, thresholds,
   and state-machine semantics that define it. The CHANGELOG and
   `docs/user/gestures.md` describe gestures for end users, but nothing
   ties each gesture to the exact `ActionSet` field it sets and the
   `InputInjector` call it drives.
2. **Complex actions are hard to recognize precisely.** The user
   observes that fast / complex motions mis-register. The confirmed
   contributor is **latency causing cross-frame mismatch** (the hand
   has moved to the next pose by the time a frame is processed); other
   causes (threshold/state-machine misjudgement, multi-gesture
   conflict) are *suspected but unconfirmed* and must be measured, not
   guessed.
3. **No per-action verification.** The existing end-to-end test
   (`test_pipeline_e2e.cpp`) walks one combined 30-frame sequence
   through all gestures, but there is no isolated, per-action check that
   a given motion produces *that action and no other*.

## Goals

- **G1.** An authoritative, maintained gesture→action map document.
- **G2.** One self-contained synthetic-landmark fixture per action,
  each asserting the action fires (positive) **and** that no other
  action fires (negative / anti-cross-talk). These fixtures are the
  measuring stick for "recognition correctness" before and after any
  tuning.
- **G3.** Measure the real per-frame latency chain (capture → IPC →
  inference → gesture → injection) with data, confirm the dominant
  source, then reduce it. Validate that optimizations do not regress
  recognition using the G2 fixtures.

## Non-goals

- No external/public gesture-dataset videos. Public datasets are static
  hand-pose *classification* (👍✌️✊); VMosue's gestures are private
  *dynamic interaction sequences* (pinch, push-to-click, two-finger
  scroll) whose labels do not exist in any public set. Verification uses
  **synthetic landmark sequences** instead — deterministic, CI-able, no
  external assets.
- No new ML model, no gesture-recognition rewrite.
- The large performance rewrites (SIMD, shared-memory IPC) are gated on
  G3's measurement and tracked separately (ROADMAP D2/D3); this spec
  commits only to the measurement (D1) plus low-risk wins it justifies.

## The nine actions (authoritative list)

From `ActionSet` (`src/gesture/GestureStateMachine.h`) and the
detectors. The map document (G1) is the normative version; this is the
inventory the fixtures (G2) must each cover:

| # | Gesture | Hand | Trigger (landmarks / semantics) | ActionSet field | Injector |
|---|---------|------|----------------------------------|-----------------|----------|
| 1 | Cursor move | Right | Index-MCP (5) translation | `cursorDx/Dy` | mouse move |
| 2 | Left click | Right | Thumb(4)–index(8) pinch < threshold, quick release | `leftClick` | LMB click |
| 3 | Double click | Right | Two pinches within system double-click window | `leftDoubleClick` | LMB ×2 |
| 4 | Left drag | Right | Pinch held > `holdForDragMs`, then move, then release | `leftDown`→`cursorD`→`leftUp` | LMB down/up |
| 5 | Middle click | Right | Thumb(4)–middle(12) pinch + release | `middleClick` | MMB click |
| 6 | Right click | Right | Index-tip `world[8].z` push toward camera then retract within window | `rightClick` | RMB click |
| 7 | Scroll | Left | Index(8)+middle(12) tips close, vertical/horizontal motion | `wheel`/`hWheel` | wheel / hwheel |
| 8 | Pause/Resume | Left | Open left hand held > `holdMs` | state → Paused/Active | (no inject) |
| 9 | Emergency stop | Either | Both hands open > `twoHandOpenHoldMs` | `safeRelease`, state → Stopped | release all |

## Design

### Part 1 — Authoritative map (G1)

New doc `docs/user/gesture-action-map.md` (developer/QA-facing,
linked from `docs/user/gestures.md`). One row per action with columns:

- **Gesture name** and hand.
- **Trigger definition**: exact landmark indices, the distance/Z
  metric, the threshold source (adaptive vs config-fixed vs system),
  and the state-machine phase transitions.
- **Output**: the `ActionSet` field(s) set, with sign conventions.
- **Injection**: the `InputInjector` call.
- **Anti-cross-talk note**: which other action it must NOT trigger, and
  the arbitration rule that guarantees it (e.g. "left wins → right-click
  suppressed same frame").
- **Fixture**: the name of the G2 fixture that proves the row.

The doc is the contract; every row must map to a passing fixture.

### Part 2 — Per-action fixtures + tests (G2)

Reuse the existing fixture mechanism — high-level semantic JSON
(`pinch`/`open`/`scroll_dy`) expanded into 21-point landmarks by a
builder in the test TU. Two changes:

1. **Extend the fixture schema + builder** to express the two motions
   the current builder cannot:
   - `middle_pinch` (bool): thumb tip (4) at middle tip (12) → middle
     click.
   - `push_z` (float): set `world[8].z` relative to `world[0].z` so the
     air-click approach/retract can be driven. The builder must
     populate `world[]` (not just `points[]`), since `AirClickDetector`
     reads `world`.
2. **One fixture per action** under `tests/fixtures/actions/`
   (`cursor_move`, `left_click`, `double_click`, `left_drag`,
   `middle_click`, `right_click`, `scroll_vertical`, `scroll_horizontal`,
   `pause_toggle`, `emergency_stop`), each a minimal sequence.

New test TU `tests/integration/test_action_map.cpp`, one test per
fixture, each doing **two assertions**:

- **Positive:** the expected `ActionSet` field / state transition occurs.
- **Negative (anti-cross-talk):** no *other* action field is set across
  the whole sequence (e.g. a pinch fixture asserts `rightClick==false`,
  `wheel==0`, etc.).

Each test calls `GetSignalObserver().Reset()` in `SetUp()` (matching the
existing e2e isolation) so cold-start thresholds apply and tests are
order-independent. Registered in `tests/CMakeLists.txt` and run by
`ctest` (and thus CI).

This makes Part 1's table executable: every row is backed by a test.

### Part 3 — Latency measurement, then reduction (G3)

**Measure first (ROADMAP D1).** Instrument the four hot-path segments
with the existing `ProfileGuard` and emit P50/P95 per segment:

- `capture` — frame acquisition + NV12→BGRA.
- `ipc_write` — metadata + BGRA pipe write.
- `ipc_rtt` — write→response round-trip (the suspected dominant cost).
- `gesture` — state-machine + injection.

Add a build-gated microbenchmark target (or a `--selftest` mode) that
reports these without a camera, and document a one-line way to dump the
live P50/P95 (the DebugWindow already shows an FPS graph; extend its
readout or log a periodic line). The output is the evidence that ranks
the bottleneck.

**Then reduce, by evidence.** Likely order, but the data decides:

1. Confirm/quantify the pipe-IPC round-trip. If it dominates (expected),
   the high-value fix is replacing the per-frame 3.6 MB synchronous pipe
   write with a faster transport (shared memory) — large, tracked as
   ROADMAP D2/D3, **not** committed by this spec.
2. Low-risk wins this spec *can* land if measurement supports them:
   sending the downscaled inference frame over IPC rather than full-res
   (less bytes/frame); overlapping inference with capture so the gesture
   thread isn't stalled on the round-trip.

**Latency→precision link.** Cross-frame mismatch is a latency symptom.
After each latency reduction, re-run the G2 fixtures (which are
timing-aware via `ts_ms`) plus add one **timing-stress fixture** that
spaces frames at a high cadence to confirm fast motion still maps to the
correct action. If, after latency is addressed, specific actions still
mis-register, that isolates a threshold/state-machine issue — to be
handled as a follow-up with the now-trustworthy fixtures, not pre-
emptively.

## Testing

- G2 fixtures are themselves the functional tests; all run under `ctest`.
- The latency instrumentation is verified by a unit test asserting the
  ProfileGuard segments record samples, plus manual inspection of the
  P50/P95 dump.
- Full suite must stay green (currently 133/133) after each change.

## Rollout / order

1. G1 map doc + G2 fixtures & tests (defines the contract and the
   measuring stick). Low risk, locally verifiable.
2. G3 D1 measurement instrumentation + dump.
3. Evidence review → land low-risk latency wins; escalate shared-memory
   IPC to a separate spec if justified.

## Open items

- Exact P50/P95 dump surface (DebugWindow readout vs periodic log line)
  to be finalized in the plan; both are cheap, pick one in
  implementation.
