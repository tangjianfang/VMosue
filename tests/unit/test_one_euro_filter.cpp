#include <gtest/gtest.h>
#include <cmath>
#include "util/OneEuroFilter.h"

using vmosue::OneEuroFilter;

TEST(OneEuroFilter, SmoothsHighFrequencyNoise) {
  OneEuroFilter f(30.0, 1.0, 0.0, 1.0);  // freq=30, mincutoff=1, beta=0, dcutoff=1
  double last = 0.0;
  for (int i = 0; i < 100; ++i) {
    double raw = 10.0 + ((i % 2) ? 0.5 : -0.5);  // noisy around 10
    last = f.Filter(raw, 1.0 / 30.0);
  }
  // After 100 steps, the filtered value should be near 10.0
  EXPECT_NEAR(last, 10.0, 0.3);
}

TEST(OneEuroFilter, TracksSlowSignal) {
  // beta=1 lets the cutoff scale with |edx|, which is what gives a
  // 1€ filter its "tracks speed but rejects noise" property. With
  // beta=0 the cutoff is pinned at mincutoff=1 Hz and the filter
  // has a steady-state lag of ~4-5 units on a 29-units/sec ramp --
  // which is correct behavior for the smoothing config, just not
  // what we want to assert here.
  OneEuroFilter f(30.0, 1.0, 1.0, 1.0);
  for (int i = 0; i < 30; ++i) {
    f.Filter(static_cast<double>(i), 1.0 / 30.0);
  }
  // After ramping from 0 to 29, filter should track close
  EXPECT_NEAR(f.Filter(29.0, 1.0 / 30.0), 28.0, 2.0);
}

TEST(OneEuroFilter, ResetsOnNewInstance) {
  OneEuroFilter a(30.0, 1.0, 0.0, 1.0);
  a.Filter(100.0, 1.0 / 30.0);
  OneEuroFilter b(30.0, 1.0, 0.0, 1.0);
  EXPECT_NEAR(b.Filter(0.0, 1.0 / 30.0), 0.0, 0.01);
}

TEST(OneEuroFilter, AdaptParamsDoesNotSnap) {
  // After AdaptParams the filter should continue from its current
  // state, not snap to the new input. We prime the filter to ~5.0,
  // retune to a much more aggressive (low-mincutoff) config, and
  // feed the same input again. The output should be close to 5.0,
  // not jump to 10.0.
  OneEuroFilter f(30.0, 1.0, 0.0, 1.0);
  for (int i = 0; i < 30; ++i) {
    f.Filter(5.0, 1.0 / 30.0);
  }
  f.AdaptParams(30.0, 0.1, 0.0);  // very heavy smoothing
  double v = f.Filter(10.0, 1.0 / 30.0);
  // With mincutoff=0.1 the cutoff barely moves from the previous
  // smooth value, so the output should still be near 5 (well below
  // 10). We give a generous tolerance for the derivative term.
  EXPECT_LT(v, 7.0);
  EXPECT_GT(v, 3.0);
}

// v0.5: the smoother in App::inferenceLoop is now driven by actual
// wall-clock dt (the real elapsed time between frames, which varies
// 20-80ms under Python IPC) instead of a constant `1/inferenceFps`.
// This test pins the contract that the dt parameter is HONORED: the
// same input trajectory, filtered at two different dts, must produce
// different outputs. Concretely, a longer dt (slower frame rate)
// gives a lower alpha in the low-pass, so the filter tracks a step
// input more slowly. If OneEuroFilter ever stops reading dt (e.g. a
// future refactor hardcodes the cadence), this test breaks.
TEST(OneEuroFilter, DtParameterChangesTracking) {
  // Step from 0 -> 100 at one frame, then stay at 100 for 30 more
  // frames. With dt=0.033 (30 Hz nominal) the filter should reach
  // ~90% of the way in those 30 frames; with dt=0.100 (10 Hz idle
  // downshift) the same 30 frames are stretched to wall-clock 3
  // seconds of filter time, and the filter should track the step
  // essentially perfectly by then.
  const double kStep = 100.0;
  const int    kSteps = 30;

  OneEuroFilter fast(30.0, 1.0, 0.0, 1.0);
  // First call: seed
  fast.Filter(0.0, 1.0 / 30.0);
  fast.Filter(kStep, 1.0 / 30.0);
  for (int i = 0; i < kSteps; ++i) {
    fast.Filter(kStep, 1.0 / 30.0);
  }
  const double fastFinal = fast.Filter(kStep, 1.0 / 30.0);

  OneEuroFilter slow(30.0, 1.0, 0.0, 1.0);
  slow.Filter(0.0, 1.0 / 30.0);  // same seed dt for fairness
  slow.Filter(kStep, 1.0 / 30.0);
  // Same 30 updates but each one spans 100ms of wall time instead
  // of 33ms — so the filter has had ~3s of effective time to settle,
  // which is much more than the 1s of the fast path. The slow path
  // output should be closer to kStep than the fast path output.
  for (int i = 0; i < kSteps; ++i) {
    slow.Filter(kStep, 1.0 / 10.0);
  }
  const double slowFinal = slow.Filter(kStep, 1.0 / 10.0);

  // Sanity: both filters actually track the step (no NaN, no snap
  // to zero).
  EXPECT_GT(fastFinal, kStep * 0.5);
  EXPECT_GT(slowFinal, kStep * 0.5);
  // The core regression: slowFinal is strictly closer to kStep
  // than fastFinal. This is the contract the wall-clock dt fix
  // relies on — if dt is ignored, both would converge to the same
  // value within numerical noise.
  EXPECT_LT(kStep - slowFinal, kStep - fastFinal)
      << "dt parameter is not honored: "
      << "fastFinal=" << fastFinal << " slowFinal=" << slowFinal;
}

// Companion test: dt <= 0 (including the dt=0 default) must fall
// back to the constructor's freq_ (preserving the original Filter
// signature's behavior for callers that don't pass an explicit dt).
TEST(OneEuroFilter, ZeroDtFallsBackToFreq) {
  OneEuroFilter explicit_(30.0, 1.0, 0.0, 1.0);
  explicit_.Filter(0.0, 1.0 / 30.0);
  explicit_.Filter(10.0, 1.0 / 30.0);
  const double a = explicit_.Filter(10.0, 1.0 / 30.0);

  OneEuroFilter defaulted(30.0, 1.0, 0.0, 1.0);
  defaulted.Filter(0.0, 1.0 / 30.0);
  defaulted.Filter(10.0, 1.0 / 30.0);
  // dt=0 -> falls back to 1/freq_ = 1/30, same as explicit case.
  const double b = defaulted.Filter(10.0, 0.0);

  EXPECT_DOUBLE_EQ(a, b);
}
