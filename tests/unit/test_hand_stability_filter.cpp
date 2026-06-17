#include <gtest/gtest.h>
#include "inference/HandStabilityFilter.h"

using vmosue::HandStabilityFilter;

TEST(HandStabilityFilter, SingleFramePhantomDropped) {
  HandStabilityFilter f;
  // A phantom appears for exactly 1 frame and is gone. With
  // kStabilityFrames=3 it must not pass.
  EXPECT_FALSE(f.Accept(/*hand=*/1, /*score=*/0.6f, /*floor=*/0.5f));
  EXPECT_FALSE(f.Accept(/*hand=*/1, /*score=*/0.2f, /*floor=*/0.5f));  // gap
  EXPECT_FALSE(f.Accept(/*hand=*/1, /*score=*/0.2f, /*floor=*/0.5f));
  EXPECT_FALSE(f.Accept(/*hand=*/1, /*score=*/0.2f, /*floor=*/0.5f));
}

TEST(HandStabilityFilter, SteadyHandPassesAfter3Frames) {
  HandStabilityFilter f;
  EXPECT_FALSE(f.Accept(1, 0.7f, 0.5f));
  EXPECT_FALSE(f.Accept(1, 0.7f, 0.5f));
  EXPECT_TRUE (f.Accept(1, 0.7f, 0.5f));
  EXPECT_TRUE (f.Accept(1, 0.7f, 0.5f));
}

TEST(HandStabilityFilter, FlickerDroppedThenSteadyHandPasses) {
  HandStabilityFilter f;
  // Phantom 1: 1 frame, then 2 frames of nothing.
  f.Accept(1, 0.55f, 0.5f);
  f.Accept(1, 0.10f, 0.5f);
  f.Accept(1, 0.10f, 0.5f);
  // Real hand now visible and steady: counter must start from 0
  // (the phantom's count was 0 already after the 2 missed frames)
  // and reach 3 by the third good frame.
  EXPECT_FALSE(f.Accept(1, 0.8f, 0.5f));
  EXPECT_FALSE(f.Accept(1, 0.8f, 0.5f));
  EXPECT_TRUE (f.Accept(1, 0.8f, 0.5f));
}

TEST(HandStabilityFilter, TwoHandsTrackedIndependently) {
  HandStabilityFilter f;
  // Right hand steady from frame 1; left hand flickering.
  // kStabilityFrames=3 means the right hand only passes after 3
  // consecutive above-floor frames; the left hand never reaches
  // that because its good frames are interleaved with sub-floor
  // gaps.
  EXPECT_FALSE(f.Accept(1, 0.8f, 0.5f));   // right: 1 (not yet stable)
  EXPECT_FALSE(f.Accept(0, 0.6f, 0.5f));   // left:  1 (phantom)
  EXPECT_FALSE(f.Accept(0, 0.1f, 0.5f));   // left: 0 (gap)
  EXPECT_FALSE(f.Accept(1, 0.8f, 0.5f));   // right: 2 (not yet stable)
  EXPECT_FALSE(f.Accept(0, 0.6f, 0.5f));   // left: 1
  EXPECT_TRUE (f.Accept(1, 0.8f, 0.5f));   // right: 3 -> passes
  EXPECT_FALSE(f.Accept(0, 0.1f, 0.5f));   // left: 0
  EXPECT_TRUE (f.Accept(1, 0.8f, 0.5f));   // right: 4 (steady)
  EXPECT_FALSE(f.Accept(0, 0.6f, 0.5f));   // left: 1 (still phantom)
}

TEST(HandStabilityFilter, SingleGapDoesNotResetCounter) {
  // The user's "I'm just briefly moving my hand out of frame"
  // case. One frame below floor does NOT drop the counter to zero.
  HandStabilityFilter f;
  f.Accept(1, 0.8f, 0.5f);  // 1
  f.Accept(1, 0.8f, 0.5f);  // 2
  f.Accept(1, 0.8f, 0.5f);  // 3 -> passes
  f.Accept(1, 0.8f, 0.5f);  // 4
  f.Accept(1, 0.2f, 0.5f);  // 3 (single bad frame, decremented not reset)
  EXPECT_TRUE(f.Accept(1, 0.8f, 0.5f));   // 4 -> still passes
  EXPECT_TRUE(f.Accept(1, 0.8f, 0.5f));   // 5
}

TEST(HandStabilityFilter, ResetClearsCounters) {
  HandStabilityFilter f;
  f.Accept(1, 0.8f, 0.5f);
  f.Accept(1, 0.8f, 0.5f);
  f.Accept(1, 0.8f, 0.5f);
  EXPECT_EQ(f.CountForTest(1), 3);
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
