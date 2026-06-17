#include <gtest/gtest.h>
#include "inference/HandStabilityFilter.h"

using vmosue::HandStabilityFilter;

namespace {
// kStabilityFrames is bumped between releases as the anti-phantom
// target tightens. Tests below should reference the current value
// rather than hard-coding 3, so a future bump doesn't silently
// invalidate them. C++20 inline constexpr means the test binary
// always sees the latest header value.
constexpr int kK = HandStabilityFilter::kStabilityFrames;
}  // namespace

TEST(HandStabilityFilter, SingleFramePhantomDropped) {
  HandStabilityFilter f;
  // A phantom appears for exactly 1 frame and is gone. With
  // kStabilityFrames=N it must not pass.
  EXPECT_FALSE(f.Accept(/*hand=*/1, /*score=*/0.6f, /*floor=*/0.5f));
  EXPECT_FALSE(f.Accept(/*hand=*/1, /*score=*/0.2f, /*floor=*/0.5f));  // gap
  EXPECT_FALSE(f.Accept(/*hand=*/1, /*score=*/0.2f, /*floor=*/0.5f));
  EXPECT_FALSE(f.Accept(/*hand=*/1, /*score=*/0.2f, /*floor=*/0.5f));
  EXPECT_FALSE(f.Accept(/*hand=*/1, /*score=*/0.2f, /*floor=*/0.5f));
}

TEST(HandStabilityFilter, SteadyHandPassesAfterKFrames) {
  HandStabilityFilter f;
  // First k-1 frames: counter increments but does not reach the
  // threshold. The k-th frame crosses it.
  for (int i = 1; i < kK; ++i) {
    EXPECT_FALSE(f.Accept(1, 0.7f, 0.5f)) << "frame " << i;
  }
  EXPECT_TRUE(f.Accept(1, 0.7f, 0.5f));
  EXPECT_TRUE(f.Accept(1, 0.7f, 0.5f));
}

TEST(HandStabilityFilter, FlickerDroppedThenSteadyHandPasses) {
  HandStabilityFilter f;
  // Phantom 1: 1 frame, then k-1 frames of nothing (more than
  // enough to zero the counter).
  f.Accept(1, 0.55f, 0.5f);
  for (int i = 0; i < kK; ++i) f.Accept(1, 0.10f, 0.5f);
  // Real hand now visible and steady: counter must start from 0
  // (the phantom's count was 0 already after the missed frames)
  // and reach kK by the kK-th good frame.
  for (int i = 1; i < kK; ++i) {
    EXPECT_FALSE(f.Accept(1, 0.8f, 0.5f)) << "frame " << i;
  }
  EXPECT_TRUE(f.Accept(1, 0.8f, 0.5f));
}

TEST(HandStabilityFilter, TwoHandsTrackedIndependently) {
  HandStabilityFilter f;
  // Right hand steady from frame 1; left hand flickering.
  // The right hand only passes after kK consecutive above-floor
  // frames; the left hand never reaches that because its good
  // frames are interleaved with sub-floor gaps.
  EXPECT_FALSE(f.Accept(1, 0.8f, 0.5f));   // right: 1
  EXPECT_FALSE(f.Accept(0, 0.6f, 0.5f));   // left:  1 (phantom)
  EXPECT_FALSE(f.Accept(0, 0.1f, 0.5f));   // left:  0 (gap)
  EXPECT_FALSE(f.Accept(1, 0.8f, 0.5f));   // right: 2
  EXPECT_FALSE(f.Accept(0, 0.6f, 0.5f));   // left:  1
  for (int i = 3; i < kK; ++i) {
    EXPECT_FALSE(f.Accept(1, 0.8f, 0.5f)) << "right frame " << i;
  }
  EXPECT_TRUE(f.Accept(1, 0.8f, 0.5f));   // right: kK -> passes
  EXPECT_FALSE(f.Accept(0, 0.1f, 0.5f));   // left:  0
  EXPECT_TRUE(f.Accept(1, 0.8f, 0.5f));   // right: steady
  EXPECT_FALSE(f.Accept(0, 0.6f, 0.5f));   // left:  still phantom
}

TEST(HandStabilityFilter, SingleGapDoesNotResetCounter) {
  // The user's "I'm just briefly moving my hand out of frame"
  // case. One frame below floor does NOT drop the counter to zero.
  HandStabilityFilter f;
  for (int i = 0; i < kK; ++i) f.Accept(1, 0.8f, 0.5f);  // reach kK
  EXPECT_TRUE(f.Accept(1, 0.8f, 0.5f));                  // kK+1
  f.Accept(1, 0.2f, 0.5f);  // single bad frame, decremented not reset
  EXPECT_TRUE(f.Accept(1, 0.8f, 0.5f));
  EXPECT_TRUE(f.Accept(1, 0.8f, 0.5f));
}

TEST(HandStabilityFilter, ResetClearsCounters) {
  HandStabilityFilter f;
  for (int i = 0; i < kK; ++i) f.Accept(1, 0.8f, 0.5f);
  EXPECT_EQ(f.CountForTest(1), kK);
  f.Reset();
  EXPECT_EQ(f.CountForTest(1), 0);
  EXPECT_FALSE(f.Accept(1, 0.8f, 0.5f));  // back to single-frame state
}

TEST(HandStabilityFilter, OutOfRangeHandednessIsFalse) {
  HandStabilityFilter f;
  EXPECT_FALSE(f.Accept(-1, 0.9f, 0.1f));
  EXPECT_FALSE(f.Accept(2,  0.9f, 0.1f));
  EXPECT_FALSE(f.Accept(99, 0.9f, 0.1f));
}

// v0.6.2: new tests for the >90% phantom-rejection target.
//
// Strategy: simulate 30 cycles of mixed "real hand" + "phantom
// hand" input. A real hand is at score 0.85 for 30 consecutive
// frames (a typical user gesture), then disappears for 30
// frames. A phantom hand is at score 0.55 for 3 frames
// (typical flicker), then disappears.
//
// Pass criterion: <10% of phantom frames produce a passing
// Accept() result. We measure this as
//   phantom_passes / phantom_frames <= 0.10
// where phantom_frames is the count of frames where the phantom
// was present, and phantom_passes is how many of those frames
// returned Accept=true.
//
// Note: Accept() returns true on the kK-th and subsequent frames
// of a *continuous* run. A 3-frame phantom run never reaches
// kK (currently 5), so phantom_passes should be 0 in practice.
// The 10% budget is generous headroom for future tweaks.

TEST(HandStabilityFilter, PhantomRejectionRateExceeds90Percent) {
  HandStabilityFilter f;
  constexpr float kFloor = 0.5f;
  int phantomFrames = 0;
  int phantomPasses = 0;
  // 30 cycles of: 30-frame real run, 30-frame gap, 3-frame
  // phantom run, 30-frame gap. The phantom bursts are the only
  // thing the assertion counts.
  for (int cycle = 0; cycle < 30; ++cycle) {
    for (int i = 0; i < 30; ++i) f.Accept(1, 0.85f, kFloor);
    for (int i = 0; i < 30; ++i) f.Accept(1, 0.05f, kFloor);
    for (int i = 0; i < 3; ++i) {
      ++phantomFrames;
      if (f.Accept(1, 0.55f, kFloor)) ++phantomPasses;
    }
    for (int i = 0; i < 30; ++i) f.Accept(1, 0.05f, kFloor);
  }
  ASSERT_GT(phantomFrames, 0);
  double rate = static_cast<double>(phantomPasses) / phantomFrames;
  EXPECT_LE(rate, 0.10) << "phantom acceptance rate "
                         << (rate * 100) << "% exceeds 10% budget";
}

TEST(HandStabilityFilter, RealHandAcceptanceRateIsHigh) {
  // The companion metric to PhantomRejectionRate: when the user
  // is genuinely making a gesture, we MUST accept every frame
  // (after the initial kK-frame ramp). A regression that makes
  // the filter reject real hands is worse than one that lets
  // through a phantom, because the user would feel the app is
  // completely dead.
  HandStabilityFilter f;
  constexpr float kFloor = 0.5f;
  int realFrames = 0;
  int realPasses = 0;
  // 100 consecutive frames of a real hand.
  for (int i = 0; i < 100; ++i) {
    ++realFrames;
    if (f.Accept(1, 0.85f, kFloor)) ++realPasses;
  }
  double rate = static_cast<double>(realPasses) / realFrames;
  // We expect 100 - kK + 1 passes (the kK-th is the first true,
  // then 100 - kK more frames all pass).
  int expected = 100 - kK + 1;
  EXPECT_EQ(realPasses, expected)
      << "real-hand acceptance rate " << (rate * 100)
      << "% differs from expected " << expected << " passes";
  EXPECT_GE(rate, 0.95)
      << "real-hand acceptance dropped below 95% — the app would "
         "feel broken to the user";
}
