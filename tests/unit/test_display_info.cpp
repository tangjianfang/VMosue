#include <gtest/gtest.h>
#include "platform/DisplayInfo.h"

using vmosue::DisplayInfo;
using vmosue::Display;

// The spec's contract test: Enumerate() must return at least one
// display (the OS primary), and that display must be at index 0 so
// downstream code that picks "the first" monitor always means the
// primary. This is enforced by the stable_sort in Enumerate().
TEST(DisplayInfo, EnumeratesAtLeastPrimary) {
  auto displays = DisplayInfo::Enumerate();
  ASSERT_GE(displays.size(), 1u);
  EXPECT_TRUE(displays[0].primary);
}

// Exactly one of the enumerated displays must be the OS primary.
// Without this invariant the sort in Enumerate() could put multiple
// "primary=true" entries first and the ordering contract above would
// be ambiguous.
TEST(DisplayInfo, PrimaryIsUnique) {
  auto displays = DisplayInfo::Enumerate();
  int primaryCount = 0;
  for (const auto& d : displays) {
    if (d.primary) ++primaryCount;
  }
  EXPECT_EQ(primaryCount, 1);
}

// VirtualScreen() must return a non-degenerate rect that contains
// the origin (0, 0). The origin is on the primary monitor for any
// default Windows multi-monitor layout (the primary is always
// positioned with (0, 0) on its top-left). We allow a zero width as
// long as the rect is well-formed (left <= right, top <= bottom).
TEST(DisplayInfo, VirtualScreenContainsOrigin) {
  RECT v = DisplayInfo::VirtualScreen();
  EXPECT_LE(v.left, v.right);
  EXPECT_LE(v.top, v.bottom);
  // The primary monitor always covers (0, 0), so the virtual screen
  // bounding rect must include it.
  EXPECT_LE(v.left, 0);
  EXPECT_LE(v.top, 0);
  EXPECT_GT(v.right, 0);
  EXPECT_GT(v.bottom, 0);
}

// The double-click interval used to drive the pinch double-click
// window must always be inside the clamped [200, 900] ms band,
// whatever the machine's Windows mouse setting (or a corrupt registry
// value) reports. Below 200 ms the gesture is unperformable; above
// 900 ms unrelated pinches would merge.
TEST(DisplayInfo, DoubleClickTimeIsClamped) {
  int ms = DisplayInfo::SystemDoubleClickTimeMs();
  EXPECT_GE(ms, 200);
  EXPECT_LE(ms, 900);
}
