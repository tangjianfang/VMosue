#pragma once
// DisplayInfo - thin Win32 wrapper for multi-monitor enumeration and
// virtual-screen geometry.
//
// Task 25 (Multi-Monitor Support). The InputInjector needs to know the
// active monitor's width so relative cursor deltas (which are produced
// in normalized 1920-pixel space by the gesture pipeline) are scaled
// correctly when the cursor is on a monitor of a different physical
// resolution. The broader app also needs to enumerate displays (so the
// tray / settings windows can pick a target monitor for the overlay).
//
// This header deliberately does NOT include <windows.h> from the public
// surface to keep the include footprint small. HMONITOR is a Win32
// opaque pointer; we forward-declare it via a small typedef to keep
// `Display::handle` usable in headers that already include <windows.h>
// through the .cpp. We need the real <windows.h> to get the actual
// HMONITOR typedef and RECT/POINT, so we include it here. The header
// is small and only adds <windows.h> to consumers that ask for
// DisplayInfo, which is fine for a platform-layer module.

#include <windows.h>
#include <vector>

namespace vmosue {

// A single physical display as reported by EnumDisplayMonitors.
// `rect` is in virtual-screen coordinates (matching what
// GetMonitorInfo() returns), not monitor-local. `primary` is true
// only for the OS-designated primary monitor.
struct Display {
  HMONITOR handle;
  RECT rect;       // in virtual screen coordinates
  bool primary;
};

class DisplayInfo {
 public:
  // Enumerate all attached displays. The result is sorted so the
  // primary monitor is always at index 0 (callers like the test
  // suite and the overlay placement code rely on this ordering).
  // Returns an empty vector if EnumDisplayMonitors fails.
  static std::vector<Display> Enumerate();

  // Bounding rect of the entire virtual screen (all monitors combined),
  // in virtual-screen coordinates. Falls back to a single-monitor
  // {0,0,GetSystemMetrics(SM_CXSCREEN),...} rect if the system metric
  // query fails.
  static RECT VirtualScreen();

  // Current OS cursor position. Returns {0, 0} if GetCursorPos fails;
  // callers should treat that as a soft error and not crash.
  static POINT CursorPos();
};

}  // namespace vmosue
