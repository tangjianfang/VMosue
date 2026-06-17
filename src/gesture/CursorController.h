#pragma once
#include "inference/HandDetector.h"
#include "input/InputInjector.h"

namespace vmosue {

// Forward declaration: defined in GestureStateMachine.h. The .cpp file
// includes that header so the parameter type is complete at the point of
// use; here we only need the type name for the reference parameter.
struct ActionSet;

// CursorController maps the right-hand index-finger MCP (landmark 5)
// to an absolute system cursor position. The MCP is used as the pivot
// rather than the fingertip so a pinch (which sharply moves the tip)
// does not yank the cursor.
//
// Mapping: the video frame's normalized [0, 1] range maps 1:1 to the
// virtual desktop's pixel range. The X axis is flipped for the selfie-
// mirror convention: a hand on the camera's left (small landmark x)
// should drive the cursor to the user's right (large screen x) — see
// CursorController.cpp for the rationale.
//
// Output goes into actions.cursorX / actions.cursorY (absolute screen
// pixels, replacing the v0.4 relative-delta scheme that drifted and
// made the cursor feel "offset from the hand"). The consumer thread
// forwards to Win32 SetCursorPos via InputInjector::SetCursorPos.
//
// All inputs are normalized landmark coordinates in [0, 1]. Outputs
// are clamped to [0, virtualDesktopW-1] x [0, virtualDesktopH-1] so a
// hand that briefly strays outside the frame cannot push the OS
// cursor into a black hole off the desktop.
class CursorController {
 public:
  struct Config {
    // No tunables: position is a pure 1:1 mapping by design. Kept as
    // an empty struct so call sites that pass Config{} still compile.
  };

  void SetConfig(const Config&);
  // Writes the absolute target cursor position (in virtual-desktop
  // pixels) into actions.cursorX / actions.cursorY. A frame with an
  // empty/non-finite pivot leaves cursorX at its sentinel (INT_MIN).
  void OnLandmarks(const HandLandmarks& right, ActionSet& actions, double dt);
  void Reset();
};

}  // namespace vmosue
