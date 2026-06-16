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

  int FramesObserved() const {
    std::lock_guard<std::mutex> lk(mu_);
    return framesObserved_;
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
  // Cold-start fallback: 0.6 (v0.4 default).
  float MinHandScore() const {
    auto s = GetSignalObserver().GetScoreStats();
    if (!s.hasData) return 0.6f;
    constexpr float kBias = 0.05f;
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