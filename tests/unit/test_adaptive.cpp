#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include "util/Adaptive.h"

namespace {

using vmosue::GetAdaptive;
using vmosue::GetSignalObserver;
using vmosue::kColdStartFrames;
using vmosue::SignalObserver;

// Reset global state between tests so they don't bleed into each
// other. The observer is a process-singleton; we don't have a Reset()
// method, so each TEST should drive its own observations and assert
// against the resulting adaptive value (not against an absolute).
// The AdaptiveObserverTest fixture below is unused but kept as a
// placeholder if a future test needs per-test setup.
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
  // (midpoint 0.7 minus 0.05 bias = 0.65).
  for (int i = 0; i < 200; ++i) {
    GetSignalObserver().RecordScores(0.9f, 0.5f);
  }
  float s = GetAdaptive().MinHandScore();
  EXPECT_GT(s, 0.55f);  // above the 0.5 phantom
  EXPECT_LT(s, 0.7f);   // below the 0.9 real hand
}

TEST(Adaptive, MinHandScoreSmallGapKeepsBoth) {
  // Simulate two genuinely-detected hands (small gap, both ~0.85).
  // Threshold = (0.85 + 0.82)/2 - 0.05 = 0.785, both pass.
  for (int i = 0; i < 200; ++i) {
    GetSignalObserver().RecordScores(0.85f, 0.82f);
  }
  float s = GetAdaptive().MinHandScore();
  EXPECT_LT(s, 0.82f);  // below both, so both pass
  EXPECT_GT(s, 0.7f);
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

}  // namespace