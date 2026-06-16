#include "input/InputInjector.h"

#include "platform/DisplayInfo.h"
#include "util/Logger.h"

#include <windows.h>

namespace vmosue {

namespace {

// Reference width the gesture pipeline (CursorController / GestureStateMachine)
// normalizes cursor deltas into. We scale by (activeMonitorWidth / kRefWidth)
// so a 1-unit delta on a 4K monitor produces the same on-screen travel as
// a 1-unit delta on a 1080p monitor. The choice of 1920 is documented in
// CursorController's "sensitivity" calibration contract.
constexpr int kRefWidth = 1920;

// Helper for the common case: flag-only mouse events (no dx/dy). For
// relative moves we construct INPUT directly in MoveCursor() because
// the dx/dy fields are not part of the simple flag-only shape.
//
// File-scope (anonymous namespace) rather than a private member because
// it doesn't need any class state, and so the public header doesn't
// have to mention Win32 types.
void sendMouseInput(DWORD flags, DWORD data = 0) {
  INPUT in{};
  in.type = INPUT_MOUSE;
  in.mi.dwFlags = flags;
  in.mi.mouseData = data;
  // No dx/dy set: SendInput treats mouseData and dx/dy together, and
  // for flag-only events (DOWN/UP/WHEEL) both should be zero.
  if (::SendInput(1, &in, sizeof(in)) != 1) {
    VMOSUE_LOG_WARN("SendInput failed: gle={}", ::GetLastError());
  }
}

// Compute the (activeMonitorWidth / kRefWidth) scale factor used to
// convert a normalized cursor delta into a per-monitor pixel delta.
// The active monitor is the one containing the current cursor, or
// the nearest one if the cursor is between monitors.
//
// Returns 1.0 (identity) when we cannot determine the active monitor
// (e.g., the monitor API fails, the monitor has a degenerate rect, or
// no monitor is attached). Falling back to identity keeps the previous
// single-monitor behavior intact.
double activeMonitorScale() {
  POINT cur = DisplayInfo::CursorPos();
  HMONITOR hMon = ::MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST);
  if (!hMon) return 1.0;
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  if (!::GetMonitorInfo(hMon, &mi)) return 1.0;
  LONG w = mi.rcMonitor.right - mi.rcMonitor.left;
  if (w <= 0) return 1.0;
  return static_cast<double>(w) / static_cast<double>(kRefWidth);
}

}  // namespace

InputInjector& InputInjector::Get() {
  static InputInjector inst;
  return inst;
}

InputInjector::InputInjector() = default;

void InputInjector::ClampDelta(int& dx, int& dy) {
  if (dx >  kMaxMovePerCall) dx =  kMaxMovePerCall;
  if (dx < -kMaxMovePerCall) dx = -kMaxMovePerCall;
  if (dy >  kMaxMovePerCall) dy =  kMaxMovePerCall;
  if (dy < -kMaxMovePerCall) dy = -kMaxMovePerCall;
}

void InputInjector::MoveCursor(int dx, int dy) {
  if (dx == 0 && dy == 0) return;

  // Defense-in-depth: hard-clamp the per-call pixel delta before it
  // reaches SendInput. CursorController's sensitivity * dead-zone
  // math can produce a few-hundred-pixel jump in a single frame if
  // the hand detector returns a noisy first sample or if a future
  // bug skips the dead-zone. Without this clamp, a single bad frame
  // yanks the OS cursor to a corner and the user can't recover
  // without force-quitting (this is the failure mode that motivated
  // adding the clamp).
  //
  // The clamp is applied BEFORE monitor scaling so the bound is
  // expressed in the gesture-pipeline's own reference units, which
  // is what we actually want to limit.
  ClampDelta(dx, dy);

  // The gesture pipeline produces deltas in a normalized 1920-wide
  // reference space (see CursorController). We scale them to the
  // active monitor's width so a hand movement covers the same
  // fraction of the screen on a 4K secondary monitor as on a 1080p
  // primary. Y uses the same X-derived scale rather than per-axis
  // scaling, which keeps aspect ratio identical across monitors of
  // the same aspect but different sizes (the common case).
  const double scale = activeMonitorScale();
  const int scaledDx = static_cast<int>(static_cast<double>(dx) * scale);
  const int scaledDy = static_cast<int>(static_cast<double>(dy) * scale);
  if (scaledDx == 0 && scaledDy == 0) return;

  // For relative moves, dx/dy must be set on the MOUSEINPUT struct.
  // (Spec bug fix: the previous draft of this method called
  // sendMouseInput(MOUSEEVENTF_MOVE, 0) here, which sent a redundant
  // empty INPUT with no dx/dy and then built a second INPUT with dx/dy
  // via SendInput directly. That doubled the event count and made every
  // cursor move emit two input events. We now build a single INPUT
  // with dx/dy and issue exactly one SendInput call.)
  INPUT in{};
  in.type = INPUT_MOUSE;
  in.mi.dwFlags = MOUSEEVENTF_MOVE;
  in.mi.dx = scaledDx;
  in.mi.dy = scaledDy;
  if (::SendInput(1, &in, sizeof(in)) != 1) {
    VMOSUE_LOG_WARN("SendInput(MOVE) failed: gle={}", ::GetLastError());
  }
}

void InputInjector::LeftDown() {
  // exchange() returns the previous value; if it was already true we
  // are already logically down, so don't emit a duplicate DOWN event.
  if (leftDown_.exchange(true)) return;
  sendMouseInput(MOUSEEVENTF_LEFTDOWN);
}

void InputInjector::LeftUp() {
  if (!leftDown_.exchange(false)) return;
  sendMouseInput(MOUSEEVENTF_LEFTUP);
}

void InputInjector::LeftClick() {
  // Direct down+up via the flag-only helper. We bypass the early-out
  // guards in LeftDown/LeftUp so a discrete click always emits a
  // matching DOWN+UP pair even if (by some bug) the internal state
  // was already out of sync with the OS. The internal flag still ends
  // in a consistent (released) state.
  leftDown_.store(true);
  sendMouseInput(MOUSEEVENTF_LEFTDOWN);
  leftDown_.store(false);
  sendMouseInput(MOUSEEVENTF_LEFTUP);
}

void InputInjector::RightClick() {
  // Right-click has no tracking (no RightDown/RightUp public API), so
  // we emit both events unconditionally. sendMouseInput logs and
  // swallows SendInput failures.
  sendMouseInput(MOUSEEVENTF_RIGHTDOWN);
  sendMouseInput(MOUSEEVENTF_RIGHTUP);
}

void InputInjector::MiddleClick() {
  // Middle-click is also a discrete gesture (no public MiddleDown /
  // MiddleUp API), so we mirror RightClick's pattern: emit down
  // then up unconditionally. The SendInput helper logs and
  // swallows failures.
  sendMouseInput(MOUSEEVENTF_MIDDLEDOWN);
  sendMouseInput(MOUSEEVENTF_MIDDLEUP);
}

void InputInjector::Wheel(int delta) {
  // mouseData is a signed DWORD for WHEEL; SendInput's MOUSEINPUT.mouseData
  // is DWORD, so we cast through. WHEEL_DELTA = 120 is one notch.
  sendMouseInput(MOUSEEVENTF_WHEEL, static_cast<DWORD>(delta));
}

void InputInjector::HWheel(int delta) {
  // Horizontal wheel uses MOUSEEVENTF_HWHEEL. Positive delta
  // scrolls right; we cast through DWORD the same way Wheel()
  // does. A zero delta is filtered out by sendMouseInput's
  // null-data path: the helper only forwards non-zero data when
  // a flag-only event would otherwise be a no-op, so a caller
  // can pass 0 without producing an empty input event.
  if (delta == 0) return;
  sendMouseInput(MOUSEEVENTF_HWHEEL, static_cast<DWORD>(delta));
}

void InputInjector::SafeReleaseAll() {
  // Release any logically-held left mouse button.
  if (leftDown_.exchange(false)) {
    sendMouseInput(MOUSEEVENTF_LEFTUP);
  }

  // Force-release common modifier keys so we don't strand the OS in a
  // state where the user thinks Shift / Ctrl / Alt is held but it is
  // actually only "held" by us. We unconditionally issue KEYUP for each
  // VK; if the user was actually holding Shift physically, their next
  // release will be a no-op (the OS already saw the up).
  static constexpr int kModifierVks[] = {
      VK_LSHIFT, VK_RSHIFT,
      VK_LCONTROL, VK_RCONTROL,
      VK_LMENU, VK_RMENU,
  };
  for (int vk : kModifierVks) {
    INPUT up{};
    up.type = INPUT_KEYBOARD;
    up.ki.wVk = static_cast<WORD>(vk);
    up.ki.dwFlags = KEYEVENTF_KEYUP;
    if (::SendInput(1, &up, sizeof(up)) != 1) {
      VMOSUE_LOG_WARN("SendInput(KEYUP vk={}) failed: gle={}", vk,
                      ::GetLastError());
    }
  }
}

}  // namespace vmosue