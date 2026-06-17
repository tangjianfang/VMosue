#include <gtest/gtest.h>
#include "ui/ActionReference.h"

using vmosue::ActionRef;
using vmosue::kActionList;

TEST(ActionReference, TableHasSevenRows) {
  // v0.6 ship target: 7 actions (left/right/middle click, double
  // click, drag, scroll, pause). The ActionListWindow layout
  // hard-codes a fixed height; changing this count requires
  // updating kWindowH.
  EXPECT_EQ(kActionList.size(), 7u);
}

TEST(ActionReference, AllKeysAreNonEmpty) {
  for (const auto& ref : kActionList) {
    EXPECT_NE(ref.gestureKey, nullptr) << "missing gestureKey";
    EXPECT_NE(ref.actionKey, nullptr) << "missing actionKey";
    EXPECT_NE(ref.descriptionKey, nullptr) << "missing descriptionKey";
    if (ref.gestureKey) EXPECT_GT(std::char_traits<char>::length(ref.gestureKey), 0u);
    if (ref.actionKey) EXPECT_GT(std::char_traits<char>::length(ref.actionKey), 0u);
    if (ref.descriptionKey) EXPECT_GT(std::char_traits<char>::length(ref.descriptionKey), 0u);
  }
}

TEST(ActionReference, KeysAreUnique) {
  // Catch copy-paste mistakes: two rows with the same gesture
  // or action key would silently render the wrong row in the
  // ActionListWindow.
  std::set<std::string> gestures;
  std::set<std::string> actions;
  for (const auto& ref : kActionList) {
    if (ref.gestureKey) gestures.insert(ref.gestureKey);
    if (ref.actionKey) actions.insert(ref.actionKey);
  }
  EXPECT_EQ(gestures.size(), kActionList.size())
      << "duplicate gestureKey in kActionList";
  EXPECT_EQ(actions.size(), kActionList.size())
      << "duplicate actionKey in kActionList";
}

TEST(ActionReference, KeysHaveDottedPrefix) {
  // Sanity check: the i18n bundle is keyed by dotted strings
  // (e.g. "action.leftClick"), and the lookup path assumes a
  // dot. A missing dot would silently return an empty string.
  for (const auto& ref : kActionList) {
    if (ref.gestureKey)     EXPECT_NE(std::char_traits<char>::find(ref.gestureKey, 64, '.'), nullptr);
    if (ref.actionKey)      EXPECT_NE(std::char_traits<char>::find(ref.actionKey,  64, '.'), nullptr);
    if (ref.descriptionKey) EXPECT_NE(std::char_traits<char>::find(ref.descriptionKey, 64, '.'), nullptr);
  }
}
