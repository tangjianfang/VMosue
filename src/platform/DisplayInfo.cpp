#include "platform/DisplayInfo.h"

#include <algorithm>

#include "util/Logger.h"

namespace vmosue {

namespace {

// EnumDisplayMonitors callback. Receives one HMONITOR per attached
// display; we stash it (plus its rect and primary flag) into the
// caller-provided std::vector<Display>. Returning TRUE keeps the
// enumeration going.
//
// `hdc` and `lprcClip` are unused here: we ask GetMonitorInfo for
// MONITORINFO so we get the work area / monitor rect in one call
// rather than depending on the (deprecated / sparse) clip rect.
struct EnumCtx {
  std::vector<Display>* out;
};
BOOL CALLBACK MonitorEnumProc(HMONITOR hMon, HDC /*hdc*/,
                              LPRECT /*lprcClip*/, LPARAM data) {
  auto* ctx = reinterpret_cast<EnumCtx*>(data);
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  if (!::GetMonitorInfo(hMon, &mi)) {
    // Skip monitors we can't query rather than poisoning the result
    // with a zero-sized rect. The OS rarely fails here in practice.
    return TRUE;
  }
  Display d{};
  d.handle = hMon;
  d.rect = mi.rcMonitor;
  d.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
  ctx->out->push_back(d);
  return TRUE;
}

}  // namespace

std::vector<Display> DisplayInfo::Enumerate() {
  std::vector<Display> out;
  EnumCtx ctx{&out};
  if (!::EnumDisplayMonitors(nullptr, nullptr, &MonitorEnumProc,
                             reinterpret_cast<LPARAM>(&ctx))) {
    VMOSUE_LOG_WARN("EnumDisplayMonitors failed: gle={}", ::GetLastError());
    return out;
  }
  // Sort so the primary monitor is always at index 0. This matches
  // the test contract (DisplayInfo, EnumeratesAtLeastPrimary asserts
  // displays[0].primary) and makes the "active monitor" lookups
  // downstream predictable when no cursor is yet on a screen.
  std::stable_sort(out.begin(), out.end(),
                   [](const Display& a, const Display& b) {
                     return a.primary && !b.primary;
                   });
  return out;
}

RECT DisplayInfo::VirtualScreen() {
  RECT r{};
  r.left = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
  r.top = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
  r.right = r.left + ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
  r.bottom = r.top + ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
  if (r.right == r.left || r.bottom == r.top) {
    // Fall back to a single primary-monitor rect so callers always
    // get a sane (non-degenerate) virtual screen.
    r.left = 0;
    r.top = 0;
    r.right = ::GetSystemMetrics(SM_CXSCREEN);
    r.bottom = ::GetSystemMetrics(SM_CYSCREEN);
  }
  return r;
}

POINT DisplayInfo::CursorPos() {
  POINT p{};
  p.x = 0;
  p.y = 0;
  if (!::GetCursorPos(&p)) {
    VMOSUE_LOG_WARN("GetCursorPos failed: gle={}", ::GetLastError());
  }
  return p;
}

}  // namespace vmosue
