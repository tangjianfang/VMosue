#pragma once
// v0.5 Adaptive: rolling-window statistics + derived tunables.
//
// No parameter in vmosue is a user preference; every tunable is a
// deterministic function of observable signals (model output stats,
// system state, user-intent kinematics). SignalObserver collects the
// raw observations; AdaptiveController derives the tunables that the
// rest of the codebase reads.
//
// Cold-start: the first kColdStartFrames observations fall back to a
// conservative default (the v0.4 hard-coded value). After that,
// adaptive values blend in via alpha = min(1, n / kBlendFrames) so
// the user sees a smooth transition instead of an abrupt switch.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <utility>
#include <vector>

namespace vmosue {

// Number of frames the adaptive controller stays in cold-start mode.
// ~1 s at 30 fps; chosen so that rolling windows have enough samples
// to give a meaningful percentile / gap statistic.
inline constexpr int kColdStartFrames = 30;

// Number of frames over which the cold-start blend ramps to fully
// adaptive. alpha = min(1, frames_since_warmup / kBlendFrames).
inline constexpr int kBlendFrames = 30;

// Maximum number of samples kept in any rolling window. The window
// is a simple circular buffer of last N observations; older samples
// are overwritten. 120 ≈ 4 s at 30 fps, which is enough to span
// short gesture sequences (click, scroll flick, pause tap).
inline constexpr int kRollingWindowSize = 120;

class SignalObserver {
 public:
  // Record one frame's worth of hand-detection scores. We track the
  // top-2 scores per frame (real hand + possible phantom). The
  // score-gap statistic (top1 - top2) drives the phantom filter. If
  // the model returned only one hand, top2 is recorded as 0 so the
  // gap is maximal.
  void RecordScores(float top1, float top2) {
    std::lock_guard<std::mutex> lk(mu_);
    ++framesObserved_;
    top1Idx_ = (top1Idx_ + 1) % kRollingWindowSize;
    top2Idx_ = (top2Idx_ + 1) % kRollingWindowSize;
    top1Scores_[top1Idx_] = top1;
    top2Scores_[top2Idx_] = top2;
  }

  // Record one hand-detection confidence. Used by OverlayWindow for
  // percentile-based color tiers.
  void RecordConfidence(float c) {
    std::lock_guard<std::mutex> lk(mu_);
    confIdx_ = (confIdx_ + 1) % kRollingWindowSize;
    confBuf_[confIdx_] = c;
    if (c < confMin_) confMin_ = c;
    if (c > confMax_) confMax_ = c;
  }

  // Record one render-frame duration. The OverlayWindow feeds this
  // so the render cadence can adapt to actual cost.
  void RecordRenderDuration(std::chrono::steady_clock::duration d) {
    using ms = std::chrono::milliseconds;
    auto v = std::chrono::duration_cast<ms>(d).count();
    std::lock_guard<std::mutex> lk(mu_);
    ++framesObserved_;
    renderMsIdx_ = (renderMsIdx_ + 1) % kRollingWindowSize;
    renderMsBuf_[renderMsIdx_] = static_cast<double>(v);
  }

  // Observed camera / detection frame rate (frames per second). The
  // overlay uses this to cap its render cadence so we don't paint
  // faster than frames arrive.
  void RecordFrameRate(double fps) {
    std::lock_guard<std::mutex> lk(mu_);
    lastFps_ = fps;
  }

  // Record the average per-axis landmark delta (dx, dy) for one
  // frame. The AdaptiveController consumes this to derive the
  // One-Euro filter's mincutoff (from motion noise floor) and beta
  // (from typical motion magnitude).
  void RecordLandmarkMotion(double dx, double dy) {
    std::lock_guard<std::mutex> lk(mu_);
    ++framesObserved_;
    lmdxIdx_ = (lmdxIdx_ + 1) % kRollingWindowSize;
    lmdyIdx_ = (lmdyIdx_ + 1) % kRollingWindowSize;
    lmdxBuf_[lmdxIdx_] = dx;
    lmdyBuf_[lmdyIdx_] = dy;
  }

  // Record the cursor pixel delta. AdaptiveController uses this to
  // derive the cursor dead-zone (from observed stillness noise).
  void RecordCursorMotion(double dx, double dy) {
    std::lock_guard<std::mutex> lk(mu_);
    curdxIdx_ = (curdxIdx_ + 1) % kRollingWindowSize;
    curdyIdx_ = (curdyIdx_ + 1) % kRollingWindowSize;
    curdxBuf_[curdxIdx_] = dx;
    curdyBuf_[curdyIdx_] = dx;
    curdyBuf_[curdyIdx_] = dy;  // intentionally overwrite the stray above
  }

  // Cache the virtual-desktop dimensions. Called once on
  // OverlayWindow::Init; consumed by CursorController for pixel
  // conversion (replacing the v0.4 hard-coded 1920x1080).
  // (x, y) is the virtual-desktop origin in pixels (negative on
  // multi-monitor rigs where the primary is not at (0, 0)). The
  // default (0, 0, 1920, 1080) matches the documented cold-start
  // fallback and is what every test sees unless Init ran on a real
  // multi-monitor host.
  void RecordVirtualDesktop(int x, int y, int w, int h) {
    std::lock_guard<std::mutex> lk(mu_);
    virtX_ = x;
    virtY_ = y;
    virtW_ = w;
    virtH_ = h;
  }
  // Backwards-compatible overload for callers that don't track
  // the origin (the existing tests). Records (0, 0) as the origin.
  void RecordVirtualDesktop(int w, int h) {
    RecordVirtualDesktop(0, 0, w, h);
  }

  // Record the per-frame z (depth) delta. World-space z is in
  // meters and is noisier than the x/y image-space coords, so it
  // gets its own rolling window. Consumed by
  // AdaptiveController::ZApproachThreshold() to set the
  // air-click approach distance relative to the observed noise
  // floor (3-sigma rule). Tracked independently of the x/y
  // landmark motion window because the units and noise
  // characteristics are different.
  void RecordZMotion(double dz) {
    std::lock_guard<std::mutex> lk(mu_);
    ++framesObserved_;
    zIdx_ = (zIdx_ + 1) % kRollingWindowSize;
    zBuf_[zIdx_] = dz;
  }

  // Record the index-thumb 2D distance (ClickDetector's pinch
  // signal). We keep a rolling min/max so the controller can place
  // pinch / release thresholds at min + k * (max - min) — the
  // "distance-bimodal" scheme: any user's open/closed pinch sits
  // at distinct min/max values, and the threshold band sits in
  // the gap between them.
  void RecordClickDistance(double d) {
    std::lock_guard<std::mutex> lk(mu_);
    ++framesObserved_;
    clickDistIdx_ = (clickDistIdx_ + 1) % kRollingWindowSize;
    clickDistBuf_[clickDistIdx_] = d;
  }

  // Record the index-middle vertical distance (ScrollDetector's
  // "two fingers together" signal). Same min/max scheme as
  // click distance but a separate buffer because the units and
  // semantics differ.
  void RecordScrollDistance(double d) {
    std::lock_guard<std::mutex> lk(mu_);
    ++framesObserved_;
    scrollDistIdx_ = (scrollDistIdx_ + 1) % kRollingWindowSize;
    scrollDistBuf_[scrollDistIdx_] = d;
  }

  // ---- Accessors (called under lock by AdaptiveController) ----

  struct ScoreStats {
    bool hasData;
    double top1Mean, top1Std;
    double top2Mean;        // typically ~phantom score
    double gapMean;         // top1 - top2
  };
  ScoreStats GetScoreStats() const {
    std::lock_guard<std::mutex> lk(mu_);
    ScoreStats s{};
    s.hasData = framesObserved_ >= kColdStartFrames;
    if (!s.hasData) return s;
    double sum1 = 0, sumSq1 = 0, sum2 = 0, sumGap = 0;
    int n = std::min<int>(framesObserved_, kRollingWindowSize);
    for (int i = 0; i < n; ++i) {
      double t1 = top1Scores_[i];
      double t2 = top2Scores_[i];
      sum1 += t1;
      sumSq1 += t1 * t1;
      sum2 += t2;
      sumGap += (t1 - t2);
    }
    s.top1Mean = sum1 / n;
    s.top1Std = std::sqrt(std::max(0.0, sumSq1 / n - s.top1Mean * s.top1Mean));
    s.top2Mean = sum2 / n;
    s.gapMean = sumGap / n;
    return s;
  }

  struct ConfidenceStats {
    bool hasData;
    double mean, std;
    double minObs, maxObs;
  };
  ConfidenceStats GetConfidenceStats() const {
    std::lock_guard<std::mutex> lk(mu_);
    ConfidenceStats s{};
    s.hasData = confIdx_ >= 0 && framesObserved_ >= kColdStartFrames / 2;
    if (!s.hasData) return s;
    int n = std::min<int>(confIdx_ + 1, kRollingWindowSize);
    double sum = 0, sumSq = 0;
    for (int i = 0; i < n; ++i) {
      sum += confBuf_[i];
      sumSq += confBuf_[i] * confBuf_[i];
    }
    s.mean = sum / n;
    s.std = std::sqrt(std::max(0.0, sumSq / n - s.mean * s.mean));
    s.minObs = confMin_;
    s.maxObs = confMax_;
    return s;
  }

  double LastRenderMs() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (renderMsIdx_ < 0) return 0.0;
    return renderMsBuf_[renderMsIdx_];
  }

  double LastObservedFps() const {
    std::lock_guard<std::mutex> lk(mu_);
    return lastFps_;
  }

  struct MotionStats {
    bool hasData;
    double meanAbsDx, meanAbsDy;     // typical motion magnitude
    double stdDx, stdDy;              // motion noise floor
  };
  MotionStats GetLandmarkMotion() const {
    std::lock_guard<std::mutex> lk(mu_);
    MotionStats s{};
    s.hasData = lmdxIdx_ >= 0 && framesObserved_ >= kColdStartFrames;
    if (!s.hasData) return s;
    int n = std::min<int>(lmdxIdx_ + 1, kRollingWindowSize);
    // We track signed sums (for std) and abs sums (for meanAbs)
    // separately. The std formula is var = E[X^2] - (E[X])^2 where
    // E[X] is the arithmetic mean, NOT the mean of absolute values.
    // An earlier version reused the abs sum for both and got
    // std=0 for any signal symmetric around 0 (e.g. alternating
    // +/-0.05 jitter) because the abs mean cancels the variance.
    double sumDxSigned = 0, sumDySigned = 0;
    double sumDxAbs = 0, sumDyAbs = 0;
    double sumDxSq = 0, sumDySq = 0;
    for (int i = 0; i < n; ++i) {
      sumDxSigned += lmdxBuf_[i];
      sumDySigned += lmdyBuf_[i];
      sumDxAbs   += std::fabs(lmdxBuf_[i]);
      sumDyAbs   += std::fabs(lmdyBuf_[i]);
      sumDxSq    += lmdxBuf_[i] * lmdxBuf_[i];
      sumDySq    += lmdyBuf_[i] * lmdyBuf_[i];
    }
    const double meanDx = sumDxSigned / n;
    const double meanDy = sumDySigned / n;
    s.meanAbsDx = sumDxAbs / n;
    s.meanAbsDy = sumDyAbs / n;
    s.stdDx = std::sqrt(std::max(0.0, sumDxSq / n - meanDx * meanDx));
    s.stdDy = std::sqrt(std::max(0.0, sumDySq / n - meanDy * meanDy));
    return s;
  }

  struct CursorMotionStats {
    bool hasData;
    double stdDx, stdDy;  // observed stillness noise in pixels
  };
  CursorMotionStats GetCursorMotion() const {
    std::lock_guard<std::mutex> lk(mu_);
    CursorMotionStats s{};
    s.hasData = curdxIdx_ >= 0 && framesObserved_ >= kColdStartFrames;
    if (!s.hasData) return s;
    int n = std::min<int>(curdxIdx_ + 1, kRollingWindowSize);
    double sumDxSq = 0, sumDySq = 0;
    for (int i = 0; i < n; ++i) {
      sumDxSq += curdxBuf_[i] * curdxBuf_[i];
      sumDySq += curdyBuf_[i] * curdyBuf_[i];
    }
    s.stdDx = std::sqrt(sumDxSq / n);
    s.stdDy = std::sqrt(sumDySq / n);
    return s;
  }

  std::pair<int, int> VirtualDesktop() const {
    std::lock_guard<std::mutex> lk(mu_);
    return {virtW_, virtH_};
  }

  // Virtual desktop origin in pixels (top-left of the spanning
  // rectangle, may be negative on multi-monitor rigs). Used by
  // CursorController to translate the normalized landmark + selfie
  // flip into an absolute SetCursorPos target.
  std::pair<int, int> VirtualDesktopOrigin() const {
    std::lock_guard<std::mutex> lk(mu_);
    return {virtX_, virtY_};
  }

  struct ZMotionStats {
    bool hasData;
    double stdDz;  // stddev of per-frame depth deltas (meters)
  };
  ZMotionStats GetZMotion() const {
    std::lock_guard<std::mutex> lk(mu_);
    ZMotionStats s{};
    s.hasData = zIdx_ >= 0 && framesObserved_ >= kColdStartFrames;
    if (!s.hasData) return s;
    int n = std::min<int>(zIdx_ + 1, kRollingWindowSize);
    double sumSq = 0;
    for (int i = 0; i < n; ++i) {
      sumSq += zBuf_[i] * zBuf_[i];
    }
    s.stdDz = std::sqrt(sumSq / n);
    return s;
  }

  struct DistanceStats {
    bool hasData;
    double min, max;  // observed range over the rolling window
  };
  DistanceStats GetClickDistance() const {
    std::lock_guard<std::mutex> lk(mu_);
    DistanceStats s{};
    s.hasData = clickDistIdx_ >= 0 && framesObserved_ >= kColdStartFrames;
    if (!s.hasData) return s;
    int n = std::min<int>(clickDistIdx_ + 1, kRollingWindowSize);
    s.min = clickDistBuf_[0];
    s.max = clickDistBuf_[0];
    for (int i = 1; i < n; ++i) {
      if (clickDistBuf_[i] < s.min) s.min = clickDistBuf_[i];
      if (clickDistBuf_[i] > s.max) s.max = clickDistBuf_[i];
    }
    return s;
  }
  DistanceStats GetScrollDistance() const {
    std::lock_guard<std::mutex> lk(mu_);
    DistanceStats s{};
    s.hasData = scrollDistIdx_ >= 0 && framesObserved_ >= kColdStartFrames;
    if (!s.hasData) return s;
    int n = std::min<int>(scrollDistIdx_ + 1, kRollingWindowSize);
    s.min = scrollDistBuf_[0];
    s.max = scrollDistBuf_[0];
    for (int i = 1; i < n; ++i) {
      if (scrollDistBuf_[i] < s.min) s.min = scrollDistBuf_[i];
      if (scrollDistBuf_[i] > s.max) s.max = scrollDistBuf_[i];
    }
    return s;
  }

  int FramesObserved() const {
    std::lock_guard<std::mutex> lk(mu_);
    return framesObserved_;
  }

  // Reset all rolling buffers and counters to a cold state. Used
  // by tests that need to assert against the cold-start default
  // (or against a clean adaptive state) without being affected by
  // observations recorded in earlier tests. Calling this on a
  // production observer would be a real foot-gun (every adaptive
  // value reverts to fallback for the next 1+ seconds), so it
  // lives behind the same mutex as the accessors and is intended
  // for the test layer only.
  void Reset() {
    std::lock_guard<std::mutex> lk(mu_);
    framesObserved_ = 0;
    top1Idx_ = top2Idx_ = -1;
    confIdx_ = -1;
    confMin_ = 1.0; confMax_ = 0.0;
    renderMsIdx_ = -1;
    lastFps_ = 30.0;
    lmdxIdx_ = lmdyIdx_ = -1;
    curdxIdx_ = curdyIdx_ = -1;
    zIdx_ = -1;
    clickDistIdx_ = scrollDistIdx_ = -1;
    // Keep virtW_/virtH_ as-is: those are read from the OS once
    // and don't drift, so resetting them would just cause a
    // one-frame "no virtual desktop recorded" state.
  }

 private:
  mutable std::mutex mu_;
  int framesObserved_ = 0;

  // top-1 and top-2 detection scores per frame, kept in circular
  // buffers of kRollingWindowSize. Confusing naming: the underlying
  // storage is `std::array<float, kRollingWindowSize>` but we name
  // the accessors top1Scores_/top2Scores_ because they refer to the
  // score of the highest- and second-highest-confidence detection.
  std::array<float, kRollingWindowSize> top1Scores_{};
  std::array<float, kRollingWindowSize> top2Scores_{};
  int top1Idx_ = -1;
  int top2Idx_ = -1;

  std::array<double, kRollingWindowSize> confBuf_{};
  int confIdx_ = -1;
  double confMin_ = 1.0;
  double confMax_ = 0.0;

  std::array<double, kRollingWindowSize> renderMsBuf_{};
  int renderMsIdx_ = -1;
  double lastFps_ = 30.0;  // safe default until observed

  std::array<double, kRollingWindowSize> lmdxBuf_{};
  std::array<double, kRollingWindowSize> lmdyBuf_{};
  int lmdxIdx_ = -1;
  int lmdyIdx_ = -1;

  std::array<double, kRollingWindowSize> curdxBuf_{};
  std::array<double, kRollingWindowSize> curdyBuf_{};
  int curdxIdx_ = -1;
  int curdyIdx_ = -1;

  std::array<double, kRollingWindowSize> zBuf_{};
  int zIdx_ = -1;

  std::array<double, kRollingWindowSize> clickDistBuf_{};
  int clickDistIdx_ = -1;
  std::array<double, kRollingWindowSize> scrollDistBuf_{};
  int scrollDistIdx_ = -1;

  int virtW_ = 1920;
  int virtH_ = 1080;
  int virtX_ = 0;
  int virtY_ = 0;
};

// Global instance; consumers read directly. Singleton is OK here
// because the data flow is one-directional (producers -> observer ->
// derived tunables) and there are no write paths from consumers.
inline SignalObserver& GetSignalObserver() {
  static SignalObserver obs;
  return obs;
}

// v0.5 adaptive tunables. Each method returns either an
// observation-driven value or a conservative fallback when the
// rolling window is cold.
class AdaptiveController {
 public:
  // Phantom-hand score floor.
  //
  // Principle (gap midpoint): the threshold sits just below the
  // midpoint of the observed top-1 and top-2 scores, with a small
  // bias toward rejecting. When only one hand is visible the gap
  // is large, so the midpoint is biased down toward the phantom
  // score (rejecting it). When two genuine hands are visible the
  // gap is small, so the midpoint sits near both scores (accepting
  // both). Concretely: threshold = max((top1 + top2) / 2 - 0.05,
  // 0.3).
  //
  // Examples:
  //   (0.9, 0.5) -> threshold 0.65 -> rejects 0.5 phantom, accepts 0.9
  //   (0.85, 0.82) -> threshold 0.785 -> accepts both
  //   (0.95, 0.94) -> threshold 0.895 -> accepts both
  //
  // Cold-start fallback: 0.6 (v0.4 default). v0.6.2: kBias raised
  // 0.05 -> 0.16 to support the >90% phantom-rejection target the
  // user asked for. At a single-real-hand scene with (top1=0.92,
  // top2=0.10) this gives a floor of (0.92+0.10)/2 - 0.16 = 0.40,
  // which lets the 0.92 real hand through while blocking any
  // 0.10-0.30 phantom. In a two-real-hand scene (0.90, 0.85) the
  // floor becomes 0.715, which is still below both real hands —
  // so the bias does NOT over-reject in the multi-hand case that
  // Adaptive's blend is meant to protect.
  float MinHandScore() const {
    auto s = GetSignalObserver().GetScoreStats();
    if (!s.hasData) return 0.6f;
    constexpr float kBias = 0.16f;
    constexpr float kFloor = 0.3f;
    float adaptive = static_cast<float>(
        (s.top1Mean + s.top2Mean) * 0.5 - kBias);
    if (adaptive < kFloor) adaptive = kFloor;
    return BlendWithFallback(adaptive, 0.6f);
  }

  // Confidence thresholds for the skeleton color tier.
  //
  // Principle (percentile): green if c > mu + sigma; yellow if
  // mu-sigma < c <= mu+sigma; red if c < mu-sigma. This makes the
  // color reflect *relative* confidence — a "low" hand in a noisy
  // scene still gets green if it's the best the user can manage.
  //
  // Returns (greenCutoff, yellowCutoff). greenCutoff >= yellowCutoff.
  // Cold-start fallback: (0.8, 0.5) (v0.4 default).
  std::pair<float, float> ConfidenceCutoffs() const {
    auto s = GetSignalObserver().GetConfidenceStats();
    if (!s.hasData) return {0.8f, 0.5f};
    float green  = static_cast<float>(s.mean + s.std);
    float yellow = static_cast<float>(s.mean - s.std);
    if (green > 1.0f) green = 1.0f;
    if (green < yellow + 0.05f) green = yellow + 0.05f;
    return {BlendWithFallback(green, 0.8f),
            BlendWithFallback(yellow, 0.5f)};
  }

  // Render thread sleep duration in milliseconds.
  //
  // Principle: sleep just long enough to hit target_fps, given the
  // observed render cost. target_fps = clamp(observed_camera_fps,
  // 30, 120) so we never waste CPU painting faster than frames
  // arrive, and we never drop below 30 fps (visible motion
  // discontinuity below ~25 fps).
  //
  // Cold-start fallback: 16 ms (60 fps).
  int RenderSleepMs() const {
    double fps = GetSignalObserver().LastObservedFps();
    if (fps <= 0) fps = 60;
    double targetFps = std::min(120.0, std::max(30.0, fps));
    double targetPeriod = 1000.0 / targetFps;
    double lastRender = GetSignalObserver().LastRenderMs();
    double sleep = targetPeriod - lastRender;
    if (sleep < 0) sleep = 0;
    if (sleep > 100) sleep = 100;
    int framesObserved = GetSignalObserver().FramesObserved();
    if (framesObserved < kColdStartFrames) return 16;
    return static_cast<int>(sleep);
  }

  // Stroke width and dot radius in D2D pixels.
  //
  // Principle (screen-size adaptive): scale by (virtW / 1920) so the
  // skeleton is visible on 4K but doesn't overwhelm a 1080p display.
  // Returns (boneWidth, dotRadius).
  std::pair<float, float> OverlayStrokeAndDot(int virtW) const {
    float scale = static_cast<float>(virtW) / 1920.0f;
    if (scale < 0.5f) scale = 0.5f;
    if (scale > 3.0f) scale = 3.0f;
    return {3.0f * scale, 5.0f * scale};
  }

  // One-Euro filter parameters (mincutoff, beta).
  //
  // Principle:
  //   - mincutoff = clamp(k_nf * sigma_noise, 0.3, 5.0). The noise
  //     floor sigma_noise is the observed std of landmark position
  //     deltas when the hand is still (no large gestures in flight).
  //     A noisy hand needs a higher mincutoff to track the noise
  //     without lag; a stable hand gets a low mincutoff for
  //     maximum jitter removal.
  //   - beta = clamp(k_beta / (median_dx + epsilon), 0.001, 0.05).
  //     The slower the typical motion, the more aggressively we
  //     smooth (high beta); the faster the motion, the less
  //     smoothing (low beta) to keep cursor responsive.
  //
  // Cold-start: (2.5, 0.02) — less aggressive than the v0.4
  // (1.0, 0.005) defaults so the very first second after startup
  // feels responsive rather than rubbery. The adaptive controller
  // overrides these from observation within ~1 s of warm-up.
  std::pair<double, double> LandmarkFilterParams() const {
    auto s = GetSignalObserver().GetLandmarkMotion();
    if (!s.hasData) return {2.5, 0.02};
    double sigma = std::max(s.stdDx, s.stdDy);
    double meanMotion = std::max(s.meanAbsDx, s.meanAbsDy);
    constexpr double kNf = 50.0;       // mincutoff = 50 * sigma
    constexpr double kBeta = 0.005;    // beta = 0.005 / mean_motion
    constexpr double epsilon = 1e-4;
    double mincutoff = std::clamp(kNf * sigma, 0.3, 5.0);
    double beta = std::clamp(kBeta / (meanMotion + epsilon),
                              0.001, 0.05);
    return {
      BlendWithFallback(static_cast<float>(mincutoff), 2.5f),
      BlendWithFallback(static_cast<float>(beta), 0.02f),
    };
  }

  // Cursor dead zone in normalized [0, 1] coords.
  //
  // Principle:  deadZone = clamp(3 * sigma_still / frameSize, 0.005, 0.05).
  // sigma_still is the std of cursor pixel deltas during "still" frames
  // (low motion magnitude). Dividing by frameSize converts pixel noise
  // back to normalized coords. The 3-sigma factor catches ~99.7% of
  // jitter without swallowing real intentional motion.
  //
  // Cold-start: 0.02 (v0.4 default).
  float CursorDeadZone() const {
    auto s = GetSignalObserver().GetCursorMotion();
    if (!s.hasData) return 0.02f;
    auto [virtW, virtH] = GetSignalObserver().VirtualDesktop();
    double frameSize = static_cast<double>(virtW + virtH) * 0.5;
    double sigma = std::max(s.stdDx, s.stdDy);
    double adaptive = 3.0 * sigma / frameSize;
    if (adaptive < 0.005) adaptive = 0.005;
    if (adaptive > 0.05)  adaptive = 0.05;
    return BlendWithFallback(static_cast<float>(adaptive), 0.02f);
  }

  // Virtual desktop pixel dimensions.
  //
  // Principle: read from the observer (set once on OverlayWindow::Init).
  // Returns the cached (width, height). Cold-start fallback: (1920, 1080).
  std::pair<int, int> DesktopPixels() const {
    auto p = GetSignalObserver().VirtualDesktop();
    if (p.first <= 0 || p.second <= 0) return {1920, 1080};
    return p;
  }

  // Air-click approach threshold in world-space meters.
  //
  // Principle:  threshold = clamp(3 * sigma_z, 0.005, 0.1).
  // sigma_z is the stddev of wrist (or any tracked landmark) depth
  // deltas — a depth-accurate proxy for "how much does the depth
  // estimate jiggle on a still hand." The 3-sigma factor catches
  // ~99.7% of noise without swallowing real intentional forward
  // push. Floor (0.005 m) prevents false clicks in low-noise
  // conditions; ceiling (0.1 m) prevents miss in very noisy
  // conditions (e.g. low-cost ToF cameras).
  //
  // Cold-start: 0.02 m (v0.4 default).
  float ZApproachThreshold() const {
    auto s = GetSignalObserver().GetZMotion();
    if (!s.hasData) return 0.02f;
    double adaptive = 3.0 * s.stdDz;
    if (adaptive < 0.005) adaptive = 0.005;
    if (adaptive > 0.1)   adaptive = 0.1;
    return BlendWithFallback(static_cast<float>(adaptive), 0.02f);
  }

  // Click pinch threshold (normalized [0, 1] frame units).
  //
  // Principle (distance-bimodal adaptive): track the rolling
  // min/max of the index-thumb distance. The user's "open" hand
  // and "closed" pinch sit at distinct values; the threshold sits
  // 40% of the way from min to max — the point where the
  // detector reliably crosses when the user pinches.
  // pinch = min + 0.4 * (max - min).
  //
  // Cold-start: 0.04 (v0.4 default).
  float PinchThreshold() const {
    auto s = GetSignalObserver().GetClickDistance();
    if (!s.hasData) return 0.04f;
    constexpr double kPinch = 0.4;
    double adaptive = s.min + kPinch * (s.max - s.min);
    // Hard floor at 1% of frame so an extreme pinch (the fingers
    // already overlap in the image) doesn't yield a negative or
    // near-zero threshold.
    if (adaptive < 0.01) adaptive = 0.01;
    return BlendWithFallback(static_cast<float>(adaptive), 0.04f);
  }

  // Click release threshold (normalized [0, 1] frame units).
  //
  // Same distance-bimodal scheme as pinch, but at the 60% point
  // so release > pinch by construction. The 20% hysteresis band
  // prevents flapping at the boundary.
  //
  // Cold-start: 0.07 (v0.4 default).
  float ReleaseThreshold() const {
    auto s = GetSignalObserver().GetClickDistance();
    if (!s.hasData) return 0.07f;
    constexpr double kRelease = 0.6;
    double adaptive = s.min + kRelease * (s.max - s.min);
    if (adaptive < 0.02) adaptive = 0.02;
    return BlendWithFallback(static_cast<float>(adaptive), 0.07f);
  }

  // Scroll enter threshold (normalized [0, 1] frame units).
  //
  // Same distance-bimodal scheme as pinch, but for the
  // index-middle vertical distance (the "two fingers together"
  // signal). The same 40%-of-range placement yields a threshold
  // that adapts to the user's finger anatomy.
  //
  // Cold-start: 0.05 (v0.4 default).
  float ScrollEnterThreshold() const {
    auto s = GetSignalObserver().GetScrollDistance();
    if (!s.hasData) return 0.05f;
    constexpr double kEnter = 0.4;
    double adaptive = s.min + kEnter * (s.max - s.min);
    if (adaptive < 0.01) adaptive = 0.01;
    return BlendWithFallback(static_cast<float>(adaptive), 0.05f);
  }

  // Scroll exit threshold (normalized [0, 1] frame units).
  //
  // Same scheme as release, at the 60% point. With
  // enter < exit the detector is in "active scroll" state when
  // the fingers are close together and falls out of it when they
  // spread — the natural reading of a two-finger-pinch-scroll.
  //
  // Cold-start: 0.03 (v0.4 default).
  float ScrollExitThreshold() const {
    auto s = GetSignalObserver().GetScrollDistance();
    if (!s.hasData) return 0.03f;
    constexpr double kExit = 0.6;
    double adaptive = s.min + kExit * (s.max - s.min);
    if (adaptive < 0.02) adaptive = 0.02;
    return BlendWithFallback(static_cast<float>(adaptive), 0.03f);
  }

  // Scroll scale factor (pixels-per-frame per unit normalized
  // finger displacement).
  //
  // Principle (motion-range adaptive): scale = base * (1080 /
  // virtH) so a 1080p screen gets the full 1500 (matching v0.4)
  // and a 4K screen gets half (less scrolling per pixel of
  // finger motion — preserves the same physical scroll
  // distance-per-cm-of-finger). Clamped to a sensible range.
  //
  // Cold-start: 1500.
  float ScrollScaleFactor() const {
    auto [virtW, virtH] = GetSignalObserver().VirtualDesktop();
    if (virtH <= 0) virtH = 1080;
    constexpr double kBase = 1500.0;
    constexpr double kRefH = 1080.0;
    double adaptive = kBase * (kRefH / static_cast<double>(virtH));
    if (adaptive < 500.0)  adaptive = 500.0;
    if (adaptive > 4000.0) adaptive = 4000.0;
    return BlendWithFallback(static_cast<float>(adaptive), 1500.0f);
  }

 private:
  // Linear blend: alpha ramps from 0 to 1 over kBlendFrames after
  // cold-start. alpha = min(1, (frames - kColdStartFrames) /
  // kBlendFrames). Returns alpha * adaptive + (1 - alpha) * fallback.
  static float BlendWithFallback(float adaptive, float fallback) {
    int n = GetSignalObserver().FramesObserved();
    if (n < kColdStartFrames) return fallback;
    int warm = n - kColdStartFrames;
    if (warm >= kBlendFrames) return adaptive;
    float alpha = static_cast<float>(warm) / static_cast<float>(kBlendFrames);
    return alpha * adaptive + (1.0f - alpha) * fallback;
  }
};

inline AdaptiveController& GetAdaptive() {
  static AdaptiveController a;
  return a;
}

}  // namespace vmosue