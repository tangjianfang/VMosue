# Test fixtures

This directory holds synthetic test inputs for the integration test in
`tests/integration/test_pipeline_e2e.cpp`.

## Why no `sample_video.mp4`?

The Task 34 spec originally called for a 10-second recorded MP4 clip
that the integration test would feed through `HandDetector` and then
the gesture pipeline. In practice this repo cannot run that test:

- MediaPipe (and `hand_landmarker.task`) is not installed in the dev
  environment, so `HandDetector::Detect()` is a stub that returns no
  landmarks.
- `vcpkg` is not available, so we cannot fetch MediaPipe or ffmpeg
  transitively.
- A real recorded clip would also be ~5-20 MB of binary data that has
  no useful life in source control.

Instead the integration test exercises the **post-inference** gesture
pipeline (cursor, click, drag, scroll, pause, two-hand-open emergency
stop) by feeding hand-crafted `HandLandmarks` sequences into
`GestureStateMachine::OnLandmarks`. This is the same downstream
contract `App` calls from its inference callback, so the test gives
full coverage of the state machine without requiring real video
processing.

If/when MediaPipe becomes available, the test can be extended to load
an actual `.task` model + recorded clip and replay frames into the
real `HandDetector::Detect()`. The current test will remain valid as
a pure state-machine regression.

## Files

- `sample_landmarks.json` - 30-frame synthetic landmark sequence (see
  the file header for the per-frame description). Frames 0-5 move the
  cursor; 6-10 trigger a left click; 11-15 trigger a drag (held
  pinch); 16-20 trigger scroll via the left hand's index/middle
  fingers; 21-25 hold the left hand open for the pause gesture; 26-29
  hold both hands open to trip the two-hand-open emergency stop.

## Per-action fixtures (`actions/`)

`tests/fixtures/actions/*.json` are minimal, single-action sequences
consumed by `tests/integration/test_action_map.cpp`. Each is the
executable proof of one row in `docs/user/gesture-action-map.md`.
Tests assert the action fires (positive) AND that no other action fires
(anti-cross-talk).

Frame schema (a superset of `sample_landmarks.json`):

**`right` fields:**
- `x_base`, `y_mcp` — index-MCP position (cursor pivot)
- `open` — all four fingertips above MCPs (open palm)
- `pinch` — thumb-index pinch (dist(4,8) ≈ 0.01)
- `middle_pinch` — thumb-middle pinch (dist(4,12) ≈ 0.01); keeps index far
- `push_z` — `world[8].z` relative to wrist; **negative = toward camera**
  (drives the air-click / right-click gesture; `AirClickDetector` reads
  `world[]`, not `points[]`)

**`left` fields:**
- `x_base`, `open` — open-palm position
- `scroll_active` — two-finger scroll mode (index + middle close together)
- `scroll_dy` — index-tip y offset per frame; **negative = up = positive wheel**
- `scroll_dx` — index-tip x offset per frame; **positive = right = positive hWheel**

Missing keys default to "feature off". Per-test `GestureStateMachine::Config`
overrides pin any timing the fixture depends on so tests are deterministic
regardless of adaptive warm-up order.

**Mirrored-camera convention:** `CursorController` negates the x delta so the
cursor follows the user's perceived hand direction (selfie-mirror). A fixture
with increasing `x_base` (hand moves right) produces `cursorDx < 0` (cursor
moves left on screen). Tests assert the sign accordingly.