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
- [ ] **D2/D3 · SIMD + shared-memory IPC (pending D1 runtime data).**
  Run `vmosue.exe` for ~30 s and read `lat_ipc_rtt P95` from the log
  (`%LOCALAPPDATA%\VMosue\logs\`). If P95 > 33 ms (one frame at 30 fps),
  the IPC rewrite is justified. Candidates:
  - Replace pipe IPC with shared memory (eliminates the 3.6 MB
    synchronous write per frame).
  - Send the downscaled inference frame (640×480) rather than the full
    capture frame.
  - SSE/AVX vectorization of `NV12ToBgra` / `FrameResampler`.
  **Not started — gated on D1 measurement data.**

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

The only remaining actionable item is **D2/D3**, and it requires one
piece of runtime data:

1. Run `vmosue.exe` (model must be present — run
   `.\scripts\prepare-resources.ps1` first if needed).
2. Perform hand gestures for ~30 seconds.
3. Open `%LOCALAPPDATA%\VMosue\logs\` and find the latest log file.
4. Search for lines starting with `latency ms (P50/P95)`.
5. Report the **`ipc_rtt P95`** value.

That number determines the optimisation path:
- **P95 > 33 ms** → shared-memory IPC rewrite is the priority.
- **P95 < 33 ms but > 10 ms** → downscale the inference frame first
  (cheap, low-risk).
- **P95 < 10 ms** → IPC is not the bottleneck; profile NV12→BGRA
  and FrameResampler next.
