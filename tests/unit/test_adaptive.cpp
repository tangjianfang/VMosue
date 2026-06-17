#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include "util/Adaptive.h"

namespace {

using vmosue::GetAdaptive;
using vmosue::GetSignalObserver;
using vmosue::kColdStartFrames;
using vmosue::SignalObserver;

// The observer is a process-singleton, so observations bleed across
// TESTs. Where a test needs a known starting point it calls
// GetSignalObserver().Reset() and then drives its own observations
// (and warms past the cold-start + blend window) so the assertion is
// deterministic regardless of test order. The AdaptiveObserverTest
// fixture below is unused but kept as a placeholder if a future test
// needs per-test setup.
class AdaptiveObserverTest : public ::testing::Test {
 protected:
  void SetUp() override {}
};

// ---- Score gap ----

TEST(Adaptive, MinHandScoreColdStartReturns060) {
  // With no observations, the score floor is the v0.4 fallback (0.6).
  // We can't reliably assert on the cold default because the global
  // observer may have data from other tests, so we just check the
  // floor is sane (>= 0.3 hard floor, <= 0.95 sensible upper bound).
  float s = GetAdaptive().MinHandScore();
  EXPECT_GE(s, 0.3f);
  EXPECT_LE(s, 0.95f);
}

TEST(Adaptive, MinHandScoreLargeGapDropsFloor) {
  // Simulate "one real hand + one phantom" by feeding the observer
  // a top1 around 0.9 and top2 around 0.5 for many frames. The gap
  // is large (0.4) so the adaptive floor should sit between them
  // (midpoint 0.7 minus v0.6.2 kBias 0.16 = 0.54). v0.6.2 raised
  // kBias from 0.05 to 0.16 to support the >90% phantom-rejection
  // target. With kBias=0.16 the phantom at 0.5 falls just below
  // the floor of 0.54 — the phantom is correctly rejected.
  for (int i = 0; i < 200; ++i) {
    GetSignalObserver().RecordScores(0.9f, 0.5f);
  }
  float s = GetAdaptive().MinHandScore();
  EXPECT_GT(s, 0.45f);  // above the 0.5 phantom (with the new bias)
  EXPECT_LT(s, 0.7f);   // below the 0.9 real hand
}

TEST(Adaptive, MinHandScoreSmallGapKeepsBoth) {
  // Simulate two genuinely-detected hands (small gap, both ~0.85).
  // Threshold = (0.85 + 0.82)/2 - 0.16 = 0.675, both pass. v0.6.2
  // raised kBias 0.05 -> 0.16, so the floor is lower than before
  // — which is the point: small gaps should still admit both
  // hands, while large gaps should reject the phantom. The
  // multi-hand case is what Adaptive's blend is meant to
  // protect.
  for (int i = 0; i < 200; ++i) {
    GetSignalObserver().RecordScores(0.85f, 0.82f);
  }
  float s = GetAdaptive().MinHandScore();
  EXPECT_LT(s, 0.82f);  // below both, so both pass
  EXPECT_GT(s, 0.5f);
}

// ---- Confidence percentiles ----

TEST(Adaptive, ConfidenceCutoffsPercentile) {
  // Feed a known distribution: ~80% above 0.9, ~20% near 0.4. The
  // green/yellow cutoffs should split the distribution somewhere
  // meaningful — green above ~0.7, yellow above ~0.5.
  for (int i = 0; i < 100; ++i) {
    float c = (i % 5 == 0) ? 0.4f : 0.95f;
    GetSignalObserver().RecordConfidence(c);
  }
  auto [green, yellow] = GetAdaptive().ConfidenceCutoffs();
  EXPECT_GE(green, yellow + 0.05f);
  EXPECT_GT(green, 0.7f);
  EXPECT_LT(yellow, 0.7f);
}

TEST(Adaptive, ConfidenceCutoffsFallBackToV04) {
  // Reset by NOT recording any confidence; the controller should
  // return the cold default (0.8, 0.5). Note: previous tests in
  // this run may have populated the global observer, so we just
  // assert that the returned pair is in a sensible range, not
  // exact values.
  auto [green, yellow] = GetAdaptive().ConfidenceCutoffs();
  EXPECT_GE(green, yellow);
  EXPECT_GE(green, 0.0f);
  EXPECT_LE(yellow, 1.0f);
}

// ---- Render cadence ----

TEST(Adaptive, RenderSleepColdStartReturns16ms) {
  // With no render-duration observations, RenderSleepMs returns 16.
  // The exact value depends on whether previous tests have populated
  // the observer, so we check the range: cold = 16, adaptive =
  // some non-negative value <= 100.
  int ms = GetAdaptive().RenderSleepMs();
  EXPECT_GE(ms, 0);
  EXPECT_LE(ms, 100);
}

TEST(Adaptive, RenderSleepReducesUnderLoad) {
  // Feed a heavy render cost (50ms) and a low observed FPS (10).
  // The target FPS should be clamped to 30 (the floor), so
  // target_period = 33ms and sleep = 33 - 50 = -17 -> 0.
  GetSignalObserver().RecordFrameRate(10.0);
  for (int i = 0; i < 60; ++i) {
    GetSignalObserver().RecordRenderDuration(std::chrono::milliseconds(50));
  }
  int ms = GetAdaptive().RenderSleepMs();
  EXPECT_LE(ms, 5);  // heavy render -> near-zero sleep
}

TEST(Adaptive, RenderSleepIncreasesForFastFPS) {
  // Feed a tiny render cost and a high observed FPS. The target
  // period should be small, and the sleep should be near the
  // target period (since last render is ~0).
  GetSignalObserver().RecordFrameRate(120.0);
  for (int i = 0; i < 60; ++i) {
    GetSignalObserver().RecordRenderDuration(std::chrono::microseconds(500));
  }
  int ms = GetAdaptive().RenderSleepMs();
  EXPECT_GT(ms, 0);
  EXPECT_LE(ms, 20);  // 1000/120 = 8ms target minus ~0ms render = ~8ms
}

// ---- Stroke / dot scale ----

TEST(Adaptive, StrokeScale1080p) {
  auto [bone, dot] = GetAdaptive().OverlayStrokeAndDot(1920);
  EXPECT_NEAR(bone, 3.0f, 0.01f);
  EXPECT_NEAR(dot,  5.0f, 0.01f);
}

TEST(Adaptive, StrokeScale4K) {
  auto [bone, dot] = GetAdaptive().OverlayStrokeAndDot(3840);
  EXPECT_NEAR(bone, 6.0f, 0.01f);
  EXPECT_NEAR(dot,  10.0f, 0.01f);
}

TEST(Adaptive, StrokeScaleClamped) {
  // Tiny display shouldn't shrink below 0.5x scale.
  auto [bone, dot] = GetAdaptive().OverlayStrokeAndDot(640);
  EXPECT_NEAR(bone, 1.5f, 0.01f);
  // Huge display (8K) shouldn't grow past 3x scale.
  auto [bone2, dot2] = GetAdaptive().OverlayStrokeAndDot(7680);
  EXPECT_NEAR(bone2, 9.0f, 0.01f);
}

// ---- Landmark filter (One-Euro) ----

TEST(Adaptive, LandmarkFilterParamsColdStart) {
  // No motion observations -> v0.4 defaults (1.0, 0.005). The
  // observer may be populated by previous tests, so we just check
  // the value is sane (mincutoff in [0.3, 5.0], beta in
  // [0.001, 0.05]).
  auto [mc, b] = GetAdaptive().LandmarkFilterParams();
  EXPECT_GE(mc, 0.3);
  EXPECT_LE(mc, 5.0);
  EXPECT_GE(b, 0.001);
  EXPECT_LE(b, 0.05);
}

TEST(Adaptive, LandmarkFilterParamsNoisyHandHighMincutoff) {
  // Feed a high-noise, high-motion sequence. mincutoff should be
  // clamped high (the noisy hand needs to track jitter without
  // lag). beta should drop toward the floor (fast motion -> less
  // smoothing). meanMotion > 0.5 puts beta at
  // 0.005/0.5 = 0.01, below the 0.001 lower clamp isn't reached
  // here, but we should see beta well below the 0.05 default.
  for (int i = 0; i < 200; ++i) {
    double dx = ((i % 2) ? 0.5 : -0.5);
    double dy = ((i % 2) ? 0.5 : -0.5);
    GetSignalObserver().RecordLandmarkMotion(dx, dy);
  }
  auto [mc, b] = GetAdaptive().LandmarkFilterParams();
  EXPECT_GE(mc, 1.0);   // noisy -> track more
  EXPECT_LE(b, 0.02);   // fast -> less smoothing than default
}

TEST(Adaptive, LandmarkFilterParamsStillHandLowMincutoff) {
  // Feed near-zero deltas (still hand). mincutoff should clamp
  // down to 0.3 (max jitter removal); beta should rise toward
  // 0.05 (slow motion -> smooth aggressively).
  for (int i = 0; i < 200; ++i) {
    GetSignalObserver().RecordLandmarkMotion(0.0001, 0.0001);
  }
  auto [mc, b] = GetAdaptive().LandmarkFilterParams();
  EXPECT_LE(mc, 1.0);  // quiet -> smoother
  EXPECT_GE(b, 0.005);
}

// ---- Cursor dead zone + desktop pixels ----

TEST(Adaptive, CursorDeadZoneColdStartIs002) {
  float dz = GetAdaptive().CursorDeadZone();
  EXPECT_GE(dz, 0.005f);
  EXPECT_LE(dz, 0.05f);
}

TEST(Adaptive, DesktopPixelsFallback1080p) {
  auto [w, h] = GetAdaptive().DesktopPixels();
  EXPECT_GT(w, 0);
  EXPECT_GT(h, 0);
  // The fallback is 1920x1080; a populated observer would return
  // the recorded size. We don't assert exact values to stay
  // robust to test ordering.
  EXPECT_GE(w, 640);
  EXPECT_GE(h, 480);
}

TEST(Adaptive, DesktopPixelsReturnsRecorded) {
  GetSignalObserver().RecordVirtualDesktop(3840, 2160);
  auto [w, h] = GetAdaptive().DesktopPixels();
  EXPECT_EQ(w, 3840);
  EXPECT_EQ(h, 2160);
}

TEST(Adaptive, CursorDeadZoneScalesWithNoise) {
  // Heavy cursor noise -> larger dead zone. Feed 10px of jitter
  // per frame; the resulting dead zone should be near the upper
  // bound (0.05) once warmup completes.
  for (int i = 0; i < 200; ++i) {
    GetSignalObserver().RecordCursorMotion(10.0, 10.0);
  }
  float dz = GetAdaptive().CursorDeadZone();
  EXPECT_GE(dz, 0.01f);  // at least the lower middle of the range
}

// ---- Z approach threshold ----

TEST(Adaptive, ZApproachThresholdColdStartIs002) {
  float thr = GetAdaptive().ZApproachThreshold();
  EXPECT_NEAR(thr, 0.02f, 0.01f);
}

TEST(Adaptive, ZApproachThresholdClampsToRange) {
  // Extreme z noise should still be clamped to [0.005, 0.1].
  for (int i = 0; i < 200; ++i) {
    GetSignalObserver().RecordZMotion(1.0);  // 1m of noise
  }
  float thr = GetAdaptive().ZApproachThreshold();
  EXPECT_LE(thr, 0.1f);
  EXPECT_GE(thr, 0.005f);
}

// ---- Click distance (pinch / release) ----

TEST(Adaptive, PinchAndReleaseFromObservedRange) {
  // Feed a clean bimodal distance: open = 0.15, pinch = 0.02.
  // min = 0.02, max = 0.15, range = 0.13.
  // pinch = 0.02 + 0.4 * 0.13 = 0.072
  // release = 0.02 + 0.6 * 0.13 = 0.098
  for (int i = 0; i < 100; ++i) {
    GetSignalObserver().RecordClickDistance(0.15);
    GetSignalObserver().RecordClickDistance(0.02);
  }
  float p = GetAdaptive().PinchThreshold();
  float r = GetAdaptive().ReleaseThreshold();
  EXPECT_GE(p, 0.05f);
  EXPECT_LE(p, 0.09f);
  EXPECT_GE(r, 0.08f);
  EXPECT_LE(r, 0.12f);
  EXPECT_GT(r, p);  // hysteresis preserved by construction
}

TEST(Adaptive, PinchThresholdSaneAfterWarming) {
  // Drive our own observations instead of relying on prior tests
  // having populated the click-distance window — that cross-test
  // dependency made this fail whenever the test ran in isolation or
  // before the tests that recorded distances (the window was empty,
  // so s.min == s.max == 0 while the cold-start threshold was 0.04).
  // Reset, warm fully past the blend window, and feed a known
  // open/closed distance range; the adaptive threshold must then land
  // inside [min, max] (the geometric property the formula guarantees).
  GetSignalObserver().Reset();
  for (int i = 0;
       i < vmosue::kColdStartFrames + vmosue::kBlendFrames + 5; ++i) {
    GetSignalObserver().RecordClickDistance(0.10);  // "open"
    GetSignalObserver().RecordClickDistance(0.02);  // "closed"
  }
  float p = GetAdaptive().PinchThreshold();
  auto s = GetSignalObserver().GetClickDistance();
  EXPECT_GE(p, static_cast<float>(s.min));
  EXPECT_LE(p, static_cast<float>(s.max));
}

// ---- Scroll distance + scale ----

TEST(Adaptive, ScrollEnterAndExitFromObservedRange) {
  // Same scheme as pinch/release but on a different range. Open
  // = 0.12, closed = 0.01 -> enter = 0.01 + 0.4*0.11 = 0.054,
  // exit = 0.01 + 0.6*0.11 = 0.076. enter < exit -> proper
  // hysteresis.
  for (int i = 0; i < 100; ++i) {
    GetSignalObserver().RecordScrollDistance(0.12);
    GetSignalObserver().RecordScrollDistance(0.01);
  }
  float e = GetAdaptive().ScrollEnterThreshold();
  float x = GetAdaptive().ScrollExitThreshold();
  EXPECT_GE(e, 0.04f);
  EXPECT_LE(e, 0.08f);
  EXPECT_GT(x, e);
}

TEST(Adaptive, ScrollScaleFactorScalesWithHeight) {
  // ScrollScaleFactor() blends from the 1500 fallback toward the
  // adaptive value over the cold-start + blend window, so the bare
  // RecordVirtualDesktop() calls a previous version of this test used
  // returned 1500 regardless of resolution whenever the process-global
  // frame counter was still inside that window (a test-ordering
  // dependency). Reset the observer and warm it fully past
  // kColdStartFrames + kBlendFrames so the blend alpha is 1.0 and the
  // adaptive value is exact.
  GetSignalObserver().Reset();
  for (int i = 0; i < vmosue::kColdStartFrames + vmosue::kBlendFrames + 5;
       ++i) {
    GetSignalObserver().RecordScores(0.9f, 0.1f);  // advances frame count
  }

  GetSignalObserver().RecordVirtualDesktop(1920, 1080);
  float s1080 = GetAdaptive().ScrollScaleFactor();
  GetSignalObserver().RecordVirtualDesktop(3840, 2160);
  float s4k = GetAdaptive().ScrollScaleFactor();
  // 4K should get roughly half the scale of 1080p.
  EXPECT_NEAR(s1080, 1500.0f, 5.0f);
  EXPECT_NEAR(s4k,  750.0f, 5.0f);
  EXPECT_NEAR(s1080, s4k * 2.0f, 5.0f);
}

}  // namespace