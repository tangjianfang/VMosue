#pragma once
// v0.6: shared "action reference" table. The same list is rendered
// by the ActionListWindow (F1 help / tray menu) and step 7 of the
// TutorialWindow, so the two surfaces cannot drift apart. Adding
// a new action means adding one row here, one i18n key in
// resources/i18n/{en,zh}.json, and one entry in the
// ActionListWindow content loop.
//
// The keys are bare strings (not wide) because I18n::TW takes
// UTF-8 keys. The "descriptionKey" is rendered in a smaller font
// under the action name.
//
// Why const char* and not std::string_view: std::string is not a
// literal type in C++20 (its destructor is not constexpr), so
// `inline constexpr std::array<ActionRef, N>` fails to compile
// when ActionRef holds a std::string. const char* is a literal
// type and works for static lookup tables.
#include <array>

namespace vmosue {

struct ActionRef {
  const char* gestureKey;     // "gesture.pinch"
  const char* actionKey;      // "action.leftClick"
  const char* descriptionKey; // "help.desc.pinch"
};

// The 7 supported one-shot / continuous actions. Order matters:
// it is also the display order in ActionListWindow and the
// tutorial step. Keep it stable.
inline constexpr std::array<ActionRef, 7> kActionList = {{
  {"gesture.pinch",        "action.leftClick",     "help.desc.pinch"},
  {"gesture.pushForward",  "action.rightClick",    "help.desc.pushForward"},
  {"gesture.thumbMiddle",  "action.middleClick",   "help.desc.thumbMiddle"},
  {"gesture.pinchTwice",   "action.doubleClick",   "help.desc.pinchTwice"},
  {"gesture.pinchHold",    "action.drag",          "help.desc.pinchHold"},
  {"gesture.twoFinger",    "action.scroll",        "help.desc.twoFinger"},
  {"gesture.openHand",     "action.pause",         "help.desc.openHand"},
}};

}  // namespace vmosue
