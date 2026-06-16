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
