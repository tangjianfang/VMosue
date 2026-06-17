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

// v0.6.1: the 3 "must-work" actions. Every other action
// (right-click, middle-click, scroll, double-click) has been
// removed from the user-facing list because they require
// fine-grained gesture discrimination the user reported as
// unreliable ("我一个的无法正常操作"). The remaining 3 are
// the gesture-mouse equivalent of "left click, hold-and-drag,
// emergency stop" — everything else is a v1.x follow-up.
//
//   1. Pinch (thumb-index)        -> Left click
//   2. Pinch and hold + move      -> Drag
//   3. Open hand held 1s          -> Pause toggle
//
// The first two are the absolute minimum for a usable gesture
// mouse; the third is the safety net. Users that need
// right-click, scroll, etc. can hit F1 to see this list and
// know they're not yet supported.
//
// Order matters: it is also the display order in
// ActionListWindow. Keep it stable.
inline constexpr std::array<ActionRef, 3> kActionList = {{
  {"gesture.pinch",        "action.leftClick",     "help.desc.pinch"},
  {"gesture.pinchHold",    "action.drag",          "help.desc.pinchHold"},
  {"gesture.openHand",     "action.pause",         "help.desc.openHand"},
}};

}  // namespace vmosue
