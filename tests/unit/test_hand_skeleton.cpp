#include <gtest/gtest.h>
#include <algorithm>
#include <set>
#include <utility>
#include "ui/HandSkeleton.h"

namespace {

using vmosue::kHandBones;

TEST(HandSkeleton, TopologyHas23Bones) {
  EXPECT_EQ(kHandBones.size(), 23u);
}

TEST(HandSkeleton, AllIndicesInRange) {
  for (const auto& bone : kHandBones) {
    EXPECT_GE(bone.first,  0);  EXPECT_LT(bone.first,  21);
    EXPECT_GE(bone.second, 0);  EXPECT_LT(bone.second, 21);
    EXPECT_NE(bone.first, bone.second);
  }
}

TEST(HandSkeleton, ContainsWristConnections) {
  // Every finger base (1, 5, 9, 13, 17) is connected to the wrist (0).
  const std::set<int> fingerBases = {1, 5, 9, 13, 17};
  for (int base : fingerBases) {
    bool found = false;
    for (const auto& bone : kHandBones) {
      if ((bone.first == 0 && bone.second == base) ||
          (bone.first == base && bone.second == 0)) {
        found = true; break;
      }
    }
    EXPECT_TRUE(found) << "missing wrist connection to landmark " << base;
  }
}

TEST(HandSkeleton, ContainsPalmConnectors) {
  // The palm of the hand: index -> middle -> ring -> pinky bases.
  const std::set<std::pair<int,int>> palm = {
      {5, 9}, {9, 13}, {13, 17}
  };
  for (const auto& p : palm) {
    bool found = false;
    for (const auto& bone : kHandBones) {
      if ((bone.first == p.first && bone.second == p.second) ||
          (bone.first == p.second && bone.second == p.first)) {
        found = true; break;
      }
    }
    EXPECT_TRUE(found) << "missing palm connection "
                       << p.first << " -> " << p.second;
  }
}

TEST(HandSkeleton, NoDuplicateBones) {
  std::set<std::pair<int,int>> seen;
  for (const auto& bone : kHandBones) {
    auto key = std::minmax(bone.first, bone.second);
    auto [_, inserted] = seen.insert(key);
    EXPECT_TRUE(inserted) << "duplicate bone "
                          << bone.first << " <-> " << bone.second;
  }
}

}  // namespace