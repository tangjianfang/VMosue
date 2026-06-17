#include <gtest/gtest.h>
#include <string>
#include "inference/HandDetector.h"

using vmosue::HandDetector;

TEST(HandDetector, AntiInterferenceOffUsesLowConfidence) {
  // "off" = trust the model; the lowest source-side floor.
  // The C++ side's HandStabilityFilter is still the stronger
  // anti-phantom signal so this is permissive, not lenient.
  EXPECT_FLOAT_EQ(
      HandDetector::MinHandConfidenceForAntiInterference("off"),
      0.4f);
}

TEST(HandDetector, AntiInterferenceLowUsesMidLowConfidence) {
  EXPECT_FLOAT_EQ(
      HandDetector::MinHandConfidenceForAntiInterference("low"),
      0.5f);
}

TEST(HandDetector, AntiInterferenceMediumUsesDefaultConfidence) {
  // "medium" is the v0.6 default; matches the Python script's
  // hardcoded --min-hand-confidence default.
  EXPECT_FLOAT_EQ(
      HandDetector::MinHandConfidenceForAntiInterference("medium"),
      0.6f);
}

TEST(HandDetector, AntiInterferenceHighUsesStrictConfidence) {
  // "high" = strictest; only high-confidence detections pass
  // the source-side filter. Even with the C++ stability filter
  // on top, a phantom with score 0.65 still gets dropped here.
  EXPECT_FLOAT_EQ(
      HandDetector::MinHandConfidenceForAntiInterference("high"),
      0.7f);
}

TEST(HandDetector, AntiInterferenceUnknownFallsBackToMedium) {
  // Future-proofing: a config written by a newer VMosue
  // version with a value we don't recognize yet should still
  // produce a sane (default) confidence rather than crashing
  // or returning 0. Medium is the safe default.
  EXPECT_FLOAT_EQ(
      HandDetector::MinHandConfidenceForAntiInterference(""),
      0.6f);
  EXPECT_FLOAT_EQ(
      HandDetector::MinHandConfidenceForAntiInterference("extreme"),
      0.6f);
  EXPECT_FLOAT_EQ(
      HandDetector::MinHandConfidenceForAntiInterference("MEDIUM"),
      0.6f);  // case-sensitive: "MEDIUM" is unknown -> medium
}

TEST(HandDetector, ConfidenceInValidRange) {
  // Defensive: the result must always be in [0, 1] so a
  // hand-edited or future-added enum value cannot produce
  // a meaningless floor (e.g. -0.5 or 2.0).
  for (const std::string& v :
       {"off", "low", "medium", "high", "", "unknown", "OFF"}) {
    float c = HandDetector::MinHandConfidenceForAntiInterference(v);
    EXPECT_GE(c, 0.0f) << "value: " << v;
    EXPECT_LE(c, 1.0f) << "value: " << v;
  }
}
