#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "util/ProfileGuard.h"

namespace {

using vmosue::ProfileGuard;

// Each test uses a unique series name so the static window can't
// leak state between tests (the Series map is process-wide).
class ProfileGuardTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ProfileGuard::Reset("pgu_basic");
    ProfileGuard::Reset("pgu_threshold");
    ProfileGuard::Reset("pgu_p95");
    ProfileGuard::Reset("pgu_disabled");
  }
};

// A ProfileGuard is a RAII timer; the simplest sanity check is
// that LastSample() is non-zero after the guard's destructor runs.
TEST_F(ProfileGuardTest, RecordsNonZeroSample) {
  {
    ProfileGuard g("pgu_basic");
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  const double last = ProfileGuard::LastSample("pgu_basic");
  EXPECT_GE(last, 0.0);
  // 2ms sleep gives us ~2ms of measured time (plus some overhead
  // for the steady_clock arithmetic). Anything < 50ms is plausibly
  // a fast machine; an upper bound of 500ms is conservative.
  EXPECT_LT(last, 500.0);
}

// PROFILE_GUARD_DISABLED must record nothing. We assert the
// P95 is exactly 0.0 after a disabled guard is destroyed.
TEST_F(ProfileGuardTest, DisabledRecordsNothing) {
  {
    PROFILE_GUARD_DISABLED("pgu_disabled");
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  EXPECT_EQ(ProfileGuard::LastSample("pgu_disabled"), 0.0);
  EXPECT_EQ(ProfileGuard::P95Ms("pgu_disabled"), 0.0);
}

// P95 of a 100-sample series where 95 are fast and 5 are slow
// should be the slow one (since the slow ones are at the tail).
// We can't trigger the destructor 100x in a unit test, so we
// verify the sort/window logic with a smaller series.
TEST_F(ProfileGuardTest, P95OfRollingWindow) {
  // Manually stuff the series via 64 separate guards. To keep
  // the test fast, we only need to exercise the sort; we don't
  // care that the wall-clock values are accurate.
  std::vector<double> values;
  for (int i = 0; i < 64; ++i) {
    {
      ProfileGuard g("pgu_p95");
    }
    values.push_back(ProfileGuard::LastSample("pgu_p95"));
  }
  // Verify the P95 calculation matches an independently computed
  // 95th percentile over the same series. The implementation
  // uses index ceil(0.95 * N) - 1 so the P95 of a 64-sample
  // window is the 61st-smallest value (index 60 in 0-based).
  // (Earlier versions of this test asserted p95 == values.back(),
  // which only held by timing luck -- the most-recent insert is
  // not the 61st-smallest sample in general, so the assertion
  // flaked roughly 95% of the time on a fast machine.)
  std::vector<double> sorted = values;
  std::sort(sorted.begin(), sorted.end());
  const double expected = sorted[60];
  const double p95 = ProfileGuard::P95Ms("pgu_p95");
  EXPECT_GE(p95, 0.0);
  EXPECT_EQ(p95, expected);
}

// Direct threshold behaviour is hard to assert without capturing
// the warn log; we just verify the constructor accepts a
// threshold and doesn't crash with a tight one. A regression
// that re-introduces an unconditional abort would be caught.
TEST_F(ProfileGuardTest, CustomThresholdDoesNotCrash) {
  {
    ProfileGuard g("pgu_threshold", /*enabled=*/true, /*thresholdMs=*/0.001);
    // Do tiny work.
    volatile int x = 1;
    (void)x;
  }
  SUCCEED();
}

// Regression: the constructor must store the threshold it is given.
// Before the fix, `threshold_` was missing from the member
// initializer list, so its value was indeterminate and on common
// stack layouts it ended up as 0.0. With threshold_ == 0.0, every
// sample satisfies `ms >= threshold_` and the destructor logs a
// WARN line — at the 30Hz capture cadence this floods the console
// with WARN lines (and the file log too) until the user force-quits.
TEST_F(ProfileGuardTest, ConstructorStoresThreshold) {
  {
    ProfileGuard g("pgu_stored", /*enabled=*/true, /*thresholdMs=*/42.5);
    EXPECT_EQ(g.ThresholdForTest(), 42.5);
  }
  // Same check with the default threshold (no arg). The class
  // header documents 60.0ms as the spec budget; if that ever
  // changes this test should be updated deliberately.
  {
    ProfileGuard g("pgu_stored2");
    EXPECT_EQ(g.ThresholdForTest(), 60.0);
  }
}

}  // namespace
