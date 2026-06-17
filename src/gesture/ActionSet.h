#pragma once
// v0.6: extracted ActionSet to its own header so the dwell-time
// gate (DwellGate.h) can include it without pulling in the full
// GestureStateMachine (which itself includes DwellGate.h — a
// circular dependency if both were in the same header).
#include <climits>

namespace vmosue {

struct ActionSet {
  // Absolute target cursor position in virtual-screen pixels.
  // Set every frame by CursorController from the primary hand's
  // MCP pivot (landmark 5); consumed by InputInjector::SetCursorPos,
  // which forwards to Win32 SetCursorPos. The video frame's [0,1]
  // range maps 1:1 to the virtual desktop — see CursorController for
  // the selfie-mirror flip on X.
  //
  // Sentinel: cursorX == INT_MIN means "no movement this frame" (hand
  // not detected, paused, or skipped). cursorY is undefined in that
  // case and must not be consumed. Intentionally NOT additive across
  // frames — the latest absolute position always wins (a slow consumer
  // pulling N frames worth of pending cursorX should jump to the
  // freshest target, not accumulate deltas which don't exist here).
  int cursorX = INT_MIN;
  int cursorY = INT_MIN;
  bool leftClick = false, leftDoubleClick = false;
  bool leftDown = false, leftUp = false;
  bool rightClick = false;
  bool middleClick = false;
  int wheel = 0;       // vertical wheel delta (positive = up)
  int hWheel = 0;      // horizontal wheel delta (positive = right)
  bool safeRelease = false;
};

}  // namespace vmosue
