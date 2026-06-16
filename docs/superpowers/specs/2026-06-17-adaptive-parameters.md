# Adaptive Parameters (v0.5)

**Date:** 2026-06-17
**Status:** Approved principle; implementation in waves
**Replaces:** every hard-coded magic number in the codebase that derives from environment / signal statistics.

## Principle

Every tunable in vmosue is a **deterministic function of observable signals**. No
parameter is a user preference. Where v0.4 had `minHandConfidence = 0.6f`,
`color threshold = 0.8f`, `Sleep(16)`, `deadZoneNorm = 0.02f`, etc., v0.5 has
`f(rolling_observations)` with bounded min/max guards.

The three driving signals are:

1. **Model output statistics** — per-hand scores, landmark jitter, classification
   confidence over a rolling window.
2. **System state** — observed camera FPS, render time per frame, detection
   latency, virtual-desktop dimensions.
3. **User intent kinematics** — measured motion magnitudes of the hand during
   "rest", "intent-to-click", "intent-to-scroll" phases, computed online.

When the rolling window is cold (first N observations), each adaptive parameter
falls back to a **conservative default** so the system is functional immediately,
not after a warm-up.

## Per-parameter adaptation table

### Hand detection

| Parameter (old)                          | New: principle                                                                |
|------------------------------------------|-------------------------------------------------------------------------------|
| `HandDetector::maxHands = 2`             | Keep at 2 — both hands are needed (right = cursor, left = pause gesture).     |
| `HandDetector::minHandConfidence = 0.6f` | **score-gap adaptive**: `threshold = max(top1 - k_gap·σ, floor)` where `k_gap ≈ 1.5`, `floor = 0.3`. When top1 ≫ top2 (typical single-hand case), the gap is large and the threshold drops, rejecting phantoms. When top1 ≈ top2 (genuine two hands), both survive. |

### Rendering

| Parameter (old)                                       | New: principle                                                                          |
|-------------------------------------------------------|-----------------------------------------------------------------------------------------|
| `OverlayWindow::Sleep(16)` (60 fps)                   | **render-cadence adaptive**: `sleep = max(0, target - dt_render)` where `target = clamp(observed_camera_fps, 30, 120)`. Never render faster than frames arrive; never below 30 fps so motion stays fluid. |
| `OverlayWindow::confidence > 0.8 → green`             | **percentile adaptive**: green = `c > μ + σ`, yellow = `μ-σ < c ≤ μ+σ`, red = `c < μ-σ`, where (μ, σ) are the rolling mean / std of recent confidences over a 30-frame window. Cold-start falls back to 0.8 / 0.5. |
| `OverlayWindow::confidence > 0.5 → yellow` (else red) | Same — absorbed into the percentile scheme.                                             |
| `OverlayWindow::boneWidth = 3.0f`                     | **screen-size adaptive**: `boneWidth = 3.0 * (virtW / 1920)`. Scales with virtual-desktop width so the skeleton is visible at 4K but doesn't overwhelm at 1080p. |
| `OverlayWindow::dotRadius = 5.0f`                     | Same — `dotRadius = 5.0 * (virtW / 1920)`.                                              |

### Smoothing

| Parameter (old)                                    | New: principle                                                                                                       |
|----------------------------------------------------|----------------------------------------------------------------------------------------------------------------------|
| `LandmarkSmoother::freq = 30.0`                    | Already implicit from observed `dt`.                                                                                 |
| `LandmarkSmoother::mincutoff = 1.0`                | **noise-floor adaptive**: track per-axis std of landmark position over the last 500 ms when the hand is plausibly still (no major gesture in flight). `mincutoff = clamp(σ_noise · k_nf, 0.3, 5.0)`. |
| `LandmarkSmoother::beta = 0.005`                   | **motion-magnitude adaptive**: track median `|dx|` over the last 1 s. `beta = clamp(k_beta / median(|dx| + ε), 0.001, 0.05)`. Faster users get smaller beta (less smoothing), twitchy users get larger beta. |
| `CursorController::deadZoneNorm = 0.02`            | **cursor-stillness adaptive**: σ_still of cursor position over 500 ms of "rest" frames (no gesture in flight) → `deadZone = clamp(3 · σ_still, 0.005, 0.05)`. |
| `CursorController` hardcoded `1920.0f, 1080.0f`    | **virtual-desktop adaptive**: replace with `SM_CXVIRTUALSCREEN, SM_CYVIRTUALSCREEN` (already cached on `OverlayWindow`; expose via a shared accessor). |

### Gesture detection

| Parameter (old)                                  | New: principle                                                                                       |
|--------------------------------------------------|------------------------------------------------------------------------------------------------------|
| `ClickDetector::pinchThresholdNorm = 0.04`       | **distance-bimodal adaptive**: maintain a histogram of index-thumb distance over recent frames. Threshold = the low-percentile cutoff that separates "open" from "closed". Equivalent to `min_distance + 0.4 · (max − min)`. |
| `ClickDetector::releaseThresholdNorm = 0.07`     | Same histogram; release = `min_distance + 0.6 · (max − min)`. Hysteresis preserved (release > pinch). |
| `AirClickDetector::zApproachThreshold = 0.02`    | **z-noise adaptive**: σ_z_still over 500 ms of rest frames. `zApproach = clamp(3 · σ_z_still, 0.005, 0.05)`. |
| `ScrollDetector::enterThresholdNorm = 0.05`      | Same distance-bimodal scheme as ClickDetector with its own per-detector histogram.                   |
| `ScrollDetector::exitThresholdNorm = 0.03`       | Same.                                                                                                |
| `ScrollDetector::scaleFactor = 1500.0`           | **motion-range adaptive**: `scaleFactor = base · (180 / virtH)` so 1080p gets 1500 and 4K gets ~750 (less scrolling per pixel of hand motion). |

## Architecture

```
                    ┌───────────────────────┐
   raw observations │                       │
   ────────────────►│  SignalObserver        │
   (scores, frames, │  (rolling windows,     │
    landmarks, dt)  │   histograms, EMA)     │
                    │                       │
                    └──────────┬────────────┘
                               │ read-only views
                               ▼
                    ┌───────────────────────┐
                    │  AdaptiveController   │  ◄── single source of truth for tunables
                    │  (no setters from UI) │
                    └──────────┬────────────┘
                               │ getXxx()
                               ▼
              ┌────────┬────────┬────────┬────────┐
              ▼        ▼        ▼        ▼        ▼
            Detect  Smoother  Overlay  Cursor  Click/Scroll
```

- `SignalObserver` collects observations; no consumers care about how.
- `AdaptiveController` exposes `minHandScore()`, `colorGreenCutoff()`,
  `colorYellowCutoff()`, `deadZoneNorm()`, `pinchThresholdNorm()`, etc.
- No UI setter. The SettingsWindow slider is removed in v0.5.

## Wave plan

1. **Wave 1 — core signal-driven** (this iteration):
   - `SignalObserver` (rolling stats)
   - `AdaptiveController` with score-gap phantom filter, percentile color
     tiers, render cadence
   - Wire into `HandDetector`, `OverlayWindow`, `DebugWindow`

2. **Wave 2 — noise-observation adaptive**: OneEuro params, deadZoneNorm,
   cursor pixel scale, AirClick z-threshold

3. **Wave 3 — gesture kinematics adaptive**: ClickDetector / ScrollDetector
   pinch + scroll thresholds

4. **Wave 4 — settings UI**: drop the sensitivity slider; show "Adaptive
   (auto)" with a live readout of the currently-derived value.

## Cold-start behavior

For the first 30 frames (~1 s at 30 fps), adaptive parameters fall back to
v0.4's hard-coded values. After the window fills, the adaptive values blend in
via `value = α · adaptive + (1 − α) · fallback` with α ramping from 0 to 1
over the next 30 frames. This avoids abrupt visual / behavioral discontinuities
when the system warms up.

## Out of scope

- Profile-specific tuning (the v0.3 `Config::activeProfile` field is removed
  in v0.5; the system observes the user, not their declared preference).
- Per-camera calibration steps — the rolling windows achieve the same effect
  online.
- ML-based adaptivity (e.g. learned thresholds from user corrections). All
  adaptation in v0.5 is closed-form on observed statistics.