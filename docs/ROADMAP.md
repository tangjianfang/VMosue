# Roadmap / TODO

Backlog of follow-up work, derived from the post-1.0 codebase
assessment (robustness / gesture-correctness / code-quality reviews)
and from issues surfaced while building the full app locally.

Status legend: `[ ]` todo · `[~]` in progress · `[x]` done.

The first batch — subprocess crash recovery, real right-click (world
landmarks), hot-path perf (Frame reuse, hand-built IPC metadata,
nth_element P95), system-aligned double-click, and the doc fixes — is
already merged to `main` (commits `3de39a2`, `c3bc84f`) and is **not**
listed here.

---

## A — Wrap-up (low cost, low risk)

- [ ] **A1 · Push `main` to origin.** Two commits ahead of
  `origin/main`, fast-forward (no force needed). Outward-facing — do
  only with explicit user authorization.
- [~] **A2 · Document the ATL / VS-instance build gotcha** in
  `docs/build-notes.md`. CMake may select the **BuildTools** instance,
  which lacks the C++ ATL component, so `CameraCapture.h` → `atlbase.h`
  fails with `C1083`. Fix: configure with
  `-DCMAKE_GENERATOR_INSTANCE="…/2022/Community"`, or install the
  "C++ ATL" component into BuildTools.
- [ ] **A3 · (optional) Precise GitHub noreply email.** The two merged
  commits use `tangjianfang@users.noreply.github.com`; switch to the
  `<id>+tangjianfang@users.noreply.github.com` form if exact GitHub
  attribution is wanted.

## B — Robustness P0 (found in review, not yet fixed)

- [ ] **B1 · CameraCapture: check HRESULTs.**
  `ConvertToContiguousBuffer` / `buffer->Lock` return values are
  unchecked (~`CameraCapture.cpp:244`); on failure `data` is a null
  pointer still passed to `NV12ToBgra`. Check the HRESULT, skip the
  frame and log on failure.
- [ ] **B2 · Media Foundation startup/shutdown pairing.**
  `EnumerateDevices` and `Init` each call `MFStartup`; `Stop` calls
  `MFShutdown` once — the reference counting can be unbalanced. Make
  each `MFStartup` balanced by exactly one `MFShutdown`.
- [ ] **B3 · Surface worker-thread crashes to the main thread.**
  Worker loops (`App.cpp:515`) only log and exit on exception; the main
  thread never learns. The Watchdog detects stalls but also only logs.
  Add an atomic failure flag / callback so a crashed capture / inference
  / state-machine thread produces a visible stop + notice instead of a
  silently frozen cursor.

## C — Test hardening

- [ ] **C1 · Unit tests for currently-uncovered modules.**
  `CameraCapture` (testable pure logic: format / pitch math), `App`
  startup-and-shutdown sequencing, platform layer (`AutoStart` /
  `Hotkey` testable parts). UI / COM paths are hard to test — focus on
  pure logic.
- [ ] **C2 · CI: coverage + ASAN.** Add a code-coverage report and an
  AddressSanitizer run (MSVC `/fsanitize=address`) to
  `.github/workflows/ci.yml`. `scripts/run-coverage.sh` already exists —
  wire it in.
- [ ] **C3 · Real-inference end-to-end smoke test.** The current E2E
  uses a recorded fixture and bypasses real MediaPipe. Add an optional
  smoke test (gated on a mediapipe environment) that exercises the
  Python IPC round-trip + world-landmark flow end to end — the class of
  bug the right-click fix addressed only appears on the real path.

## D — Performance (high value, measure first)

- [ ] **D1 · Microbenchmarks / profiling to rank bottlenecks.** Use the
  existing `ProfileGuard` plus targeted microbenchmarks for NV12→BGRA,
  FrameResampler, and the IPC round-trip; report P50/P95 to confirm the
  review's claim that the pipe IPC is the dominant latency source.
  **Gates D2/D3** — no optimization before data.
- [ ] **D2/D3 · SIMD + shared-memory IPC (pending D1).** Based on D1:
  SSE/AVX vectorization of NV12→BGRA / FrameResampler; replace the pipe
  IPC with shared memory to eliminate the per-frame 3.6 MB synchronous
  pipe write. High risk — needs thorough build + test verification.

---

## Suggested order

`A → B → C1 + C2 → C3 → D1 → (decide) D2/D3`. The A→C line is all
low-risk and locally compile-verifiable; the D performance items are
isolated and start with measurement (D1) before any rewrite.
