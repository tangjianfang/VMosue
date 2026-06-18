# Roadmap / TODO

Backlog of follow-up work, derived from the post-1.0 codebase
assessment (robustness / gesture-correctness / code-quality reviews)
and from issues surfaced while building the full app locally.

Status legend: `[ ]` todo · `[~]` in progress · `[x]` done.

---

## A — Wrap-up

- [x] **A1 · Push `main` to origin.** Done — `5d4bf0a..c2014ad`
  fast-forwarded to `github.com/tangjianfang/VMosue`.
- [x] **A2 · Document the ATL / VS-instance build gotcha** in
  `docs/build-notes.md`. CMake may select the **BuildTools** instance
  (no ATL); fix via `-DCMAKE_GENERATOR_INSTANCE` or installing the
  "C++ ATL" component. Done in commit `cb72524`.
- [ ] **A3 · (optional) Precise GitHub noreply email.** Commits use
  `tangjianfang@users.noreply.github.com`; switch to the
  `<id>+tangjianfang@users.noreply.github.com` form if exact GitHub
  attribution is wanted.

## B — Robustness P0

- [x] **B1 · CameraCapture: check HRESULTs.** `ConvertToContiguousBuffer`
  / `buffer->Lock` now checked; frame is skipped (not crashed) on
  failure. Commit `eed77e8`.
- [x] **B2 · Media Foundation startup/shutdown pairing.** `mfStarted_`
  flag added; `Stop()` only calls `MFShutdown()` when `Init()` called
  `MFStartup()` successfully. Commit `eed77e8`.
- [x] **B3 · Surface worker-thread crashes to the main thread.**
  `App::NotifyThreadError()` sets an atomic flag, posts `WM_CLOSE` to
  unblock `GetMessageW`, and flips `running_=false`. All three worker
  catch blocks call it. Commit `eed77e8`.

## C — Test hardening

- [x] **C1 · Unit tests for CameraCapture pure logic.**
  `tests/unit/test_camera_capture.cpp`: `TryGetLatestFrame` returns
  false before first frame, `Frame` default init, `PixelFormat` enum
  stability. Commit `c2014ad`. Total test count: 148/148.
- [x] **C2 · CI: coverage + ASAN.** Two new jobs in
  `.github/workflows/ci.yml`: `asan-windows` (builds with
  `/fsanitize=address`) and `coverage-windows` (OpenCppCoverage on
  Debug build, uploads `coverage.xml`). Commit `c2014ad`.
- [x] **C3 · Real-inference IPC smoke test.** Conditional step in
  `build-windows` CI: when `hand_landmarker.task` is present, starts
  the detector server, sends a 1×1 BGRA frame, and asserts a valid
  `hand_count` response. Skipped silently when model is absent.
  Commit `c2014ad`.

## D — Performance (measure first, then optimize)

- [x] **D1 · Instrument the latency chain.** `ProfileGuard` extended
  with `P50Ms` + `RecordSampleForTest`. Three `PROFILE_GUARD_DISABLED`
  spans added in `App.cpp` (`lat_capture`, `lat_ipc_rtt`,
  `lat_gesture`) with a 1 Hz P50/P95 log line. See
  `docs/build-notes.md → "Measuring per-frame latency"` for how to
  read the output. Commits `1e25510`, `b9d3fc1`.
- [~] **D2/D3 · Performance (D1 data collected; root cause identified).**
  Live measurement (2026-06-18, 320×240 inference):
  ```
  capture P50/P95 =  1.3 /  5.1 ms      (negligible)
  ipc_rtt P50/P95 = 71.5 / 89.9 ms      (dominant — 100% of pipeline cost)
  gesture P50/P95 =  0.0 /  0.0 ms      (negligible)
  ```
  **Root cause:** `ipc_rtt` is end-to-end Python+MediaPipe inference
  time, NOT pipe write cost (the 3.6 MB pipe theory was wrong; capture
  is 1ms). MediaPipe `HandLandmarker` on CPU resizes its input to a
  fixed internal size (~224×224), so reducing our input from 640×480
  to 320×240 had **no measurable effect** (the 70-90 ms band held).
  Shared-memory IPC and SIMD on `NV12ToBgra` would each save < 5 ms
  combined — not worth the complexity.
  
  Real options, none of which fit "no new dependencies":
  - **GPU delegate**: `useGpu=on` is plumbed but `mediapipe` Python on
    Windows ships only the CPU delegate. Would need a custom build or
    swap to a different runtime (ONNX Runtime + DirectML, TFLite GPU).
  - **Lighter model**: `hand_landmarker.task` ships a single FP16
    variant; an INT8-quantized or distilled variant could halve
    inference time but is not in MediaPipe's release channel.
  - **Async / pipelined inference**: decouple `stateMachineLoop` from
    `landmarkQ_` rate by extrapolating cursor position via the
    OneEuro filter between landmark frames. Reduces *perceived* lag
    (cursor moves smoothly at 30+ fps even when landmarks arrive at
    11 fps) but does not reduce true input-to-action latency.
  
  **Recommendation:** the 90 ms inference floor is a structural CPU
  ceiling, not a fixable defect. Treat it as "shipped with v1.0" and
  revisit only if/when the GPU delegate or a lighter model lands
  upstream. The async cursor extrapolation is a UX polish item, not a
  performance fix.

## G — Gesture map, per-action verification, latency

- [x] **G1 · Authoritative gesture→action map.**
  `docs/user/gesture-action-map.md`: all 9 actions with landmark
  indices, thresholds, `ActionSet` fields, injection, arbitration
  rules, and fixture cross-reference. Commit `7d3ef8e`.
- [x] **G2 · Per-action synthetic-landmark fixtures + tests.**
  `tests/fixtures/actions/` (10 JSON fixtures) +
  `tests/integration/test_action_map.cpp` (10 tests: positive +
  anti-cross-talk assertions). Builder extended with `middle_pinch` and
  `push_z`/`world-z`. Commit `3554c44`.
- [x] **G3 → merged into D1 above.**

---

## What needs to happen next

**All planned items are either complete or analytically resolved.**
D2/D3 was the only remaining performance work; runtime measurement
revealed it is structurally bounded by MediaPipe CPU inference time
(~90 ms P95) and cannot be improved by IPC, framing, or SIMD changes.
See the D2/D3 entry above for the data and recommendation.

If a future iteration wants to push further:
- Migrate inference off MediaPipe Python (ONNX Runtime + DirectML, or
  TFLite GPU delegate if it lands for Windows).
- Or treat the 90 ms floor as a UX problem and add OneEuro-based
  cursor extrapolation in `stateMachineLoop` so motion stays smooth
  between landmark frames.

Both are sizeable design changes and warrant their own spec, not a
line item on this roadmap.
