# Gesture Overlay Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the v0.2 debug cursor ring in `OverlayWindow` with a transparent hand-skeleton renderer for the dominant hand. Landmarks map 1:1 to virtual-desktop pixels so the gesture spans all monitors.

**Architecture:** Single `OverlayWindow` covering the entire virtual desktop (`SM_CXVIRTUALSCREEN` × `SM_CYVIRTUALSCREEN` at `SM_XVIRTUALSCREEN, SM_YVIRTUALSCREEN`). `Feedback` grows a 21-element landmark array + `hasHand` flag. The render thread swaps its one-circle-per-frame for a 23-bone / 21-dot skeleton. `WM_DISPLAYCHANGE` resizes the window and recreates the D2D render target via an atomic flag the render thread checks each frame.

**Tech Stack:** Win32, Direct2D (`ID2D1HwndRenderTarget`), C++20, gtest. Build via `cmd //c .\build-ninja.bat`. Tests via `ctest`.

---

## File map

| File | Change | Why |
|------|--------|-----|
| `src/ui/HandSkeleton.h` *(new)* | Add `kHandBones[]` table (23 entries). | Single source of truth for bone topology; testable in isolation. |
| `src/ui/OverlayGeometry.h` *(new)* | Add inline `LandmarkToScreen()` + `ScreenPoint`. | Pure function; no D2D dependency in tests. |
| `src/ui/OverlayWindow.h` | Extend `Feedback` with `landmarks` + `hasHand`; include `HandDetector.h` and `HandSkeleton.h`. | New render inputs. |
| `src/ui/OverlayWindow.cpp` | Switch `Init` to virtual-desktop metrics; add `WM_DISPLAYCHANGE` handler; replace `Render` with skeleton; add `ResizeRenderTarget` and `atomic<bool> needsResize_`. | Make overlay cover all monitors + draw skeleton. |
| `src/app/App.cpp` | Populate `fb.landmarks` and `fb.hasHand` in `stateMachineLoop`. | Pipe dominant hand to overlay. |
| `tests/unit/test_hand_skeleton.cpp` *(new)* | Tests for bone table size, index range, wrist connections, palm connectors. | Lock down topology contract. |
| `tests/unit/test_overlay_geometry.cpp` *(new)* | Tests for `LandmarkToScreen` mapping (zero, unit, mid, negative origin). | Lock down coordinate mapping. |
| `tests/CMakeLists.txt` | Register the two new test files. | Make the tests build + run. |

---

## Task 1: Define `kHandBones` constant (TDD)

**Files:**
- Create: `src/ui/HandSkeleton.h`
- Test: `tests/unit/test_hand_skeleton.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Add the new test source to `tests/CMakeLists.txt`**

Read `tests/CMakeLists.txt` first. Find the list of test source files (likely under a `set(TEST_SOURCES ...)` or per-target `target_sources` call). Add `tests/unit/test_hand_skeleton.cpp` and `tests/unit/test_overlay_geometry.cpp` to that list. The list is currently:
```cmake
set(TEST_SOURCES
  unit/test_app_config.cpp
  unit/test_frame_convert.cpp
  ...
)
```
Add the two new files in alphabetical order.

- [ ] **Step 2: Write the failing test file**

Create `tests/unit/test_hand_skeleton.cpp`:

```cpp
#include <gtest/gtest.h>
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
```

- [ ] **Step 3: Run the test and verify it fails to compile**

Run from repo root:
```bash
cmd //c ".\build-ninja.bat"
```

Expected: build failure on `tests/unit/test_hand_skeleton.cpp` because `src/ui/HandSkeleton.h` does not exist.

- [ ] **Step 4: Create `src/ui/HandSkeleton.h`**

```cpp
#pragma once
#include <array>
#include <utility>

namespace vmosue {

// MediaPipe Hands canonical 21-landmark skeleton topology.
//
// Landmark index reference (per the model):
//   0  = wrist
//   1..4  = thumb   (CMC, MCP, IP, TIP)
//   5..8  = index   (MCP, PIP, DIP, TIP)
//   9..12 = middle  (MCP, PIP, DIP, TIP)
//   13..16= ring    (MCP, PIP, DIP, TIP)
//   17..20= pinky   (MCP, PIP, DIP, TIP)
//
// 23 bones total: 5 fingers x 4 segments (= 20, includes the
// wrist-to-finger-base) plus 3 palm connectors (index -> middle ->
// ring -> pinky bases). The renderer iterates this table by
// std::size(); do not hard-code 23 elsewhere.
inline constexpr std::array<std::pair<int, int>, 23> kHandBones = {{
    // Thumb
    {0, 1}, {1, 2}, {2, 3}, {3, 4},
    // Index
    {0, 5}, {5, 6}, {6, 7}, {7, 8},
    // Middle
    {0, 9}, {9, 10}, {10, 11}, {11, 12},
    // Ring
    {0, 13}, {13, 14}, {14, 15}, {15, 16},
    // Pinky
    {0, 17}, {17, 18}, {18, 19}, {19, 20},
    // Palm
    {5, 9}, {9, 13}, {13, 17},
}};

}  // namespace vmosue
```

- [ ] **Step 5: Build and run the new test**

```bash
cmd //c ".\build-ninja.bat"
```

If the build succeeds, run the new test directly:
```bash
cd build && ctest -R HandSkeleton --output-on-failure
```

Expected: 5 tests pass. If `ctest -R` does not match, fall back to running the gtest binary directly:
```bash
./bin/vmosue_tests --gtest_filter='HandSkeleton.*'
```
(Use the actual binary name from `tests/CMakeLists.txt`; common names are `vmosue_tests` or `unit_tests`.)

- [ ] **Step 6: Commit**

```bash
git add src/ui/HandSkeleton.h tests/unit/test_hand_skeleton.cpp tests/CMakeLists.txt
git commit -m "feat(ui): define kHandBones skeleton topology (23 bones)"
```

---

## Task 2: Add `LandmarkToScreen` helper (TDD)

**Files:**
- Create: `src/ui/OverlayGeometry.h`
- Test: `tests/unit/test_overlay_geometry.cpp`

- [ ] **Step 1: Write the failing test file**

Create `tests/unit/test_overlay_geometry.cpp`:

```cpp
#include <gtest/gtest.h>
#include "inference/HandDetector.h"
#include "ui/OverlayGeometry.h"

namespace {

using vmosue::LandmarkToScreen;
using vmosue::Point2F;

// A landmark at (0, 0) in normalized camera coords should land at
// the top-left of the virtual desktop.
TEST(OverlayGeometry, ZeroMapsToVirtualDesktopOrigin) {
  Point2F p{0.0f, 0.0f};
  auto sp = LandmarkToScreen(p, /*virtX=*/-1920, /*virtY=*/0,
                             /*virtW=*/3840,  /*virtH=*/1080);
  EXPECT_FLOAT_EQ(sp.x, -1920.0f);
  EXPECT_FLOAT_EQ(sp.y, 0.0f);
}

// A landmark at (1, 1) should land at the bottom-right corner of
// the virtual desktop.
TEST(OverlayGeometry, UnitMapsToBottomRight) {
  Point2F p{1.0f, 1.0f};
  auto sp = LandmarkToScreen(p, -1920, 0, 3840, 1080);
  EXPECT_FLOAT_EQ(sp.x, 1920.0f);
  EXPECT_FLOAT_EQ(sp.y, 1080.0f);
}

// A landmark at the middle of the camera should land at the center
// of the virtual desktop, even if the origin is negative (the primary
// monitor is on the right of the secondary in this layout).
TEST(OverlayGeometry, HalfIsCenterOfVirtualDesktop) {
  Point2F p{0.5f, 0.5f};
  auto sp = LandmarkToScreen(p, -1920, 0, 3840, 1080);
  EXPECT_FLOAT_EQ(sp.x, 0.0f);
  EXPECT_FLOAT_EQ(sp.y, 540.0f);
}

// A landmark at the left third of the camera image on a
// side-by-side dual-monitor rig should land in the secondary monitor.
TEST(OverlayGeometry, QuarterLandsInSecondaryMonitor) {
  // Secondary monitor is x in [-1920, 0]. A 25%-from-left landmark
  // should land at -1920 + 0.25*3840 = -960.
  Point2F p{0.25f, 0.5f};
  auto sp = LandmarkToScreen(p, -1920, 0, 3840, 1080);
  EXPECT_FLOAT_EQ(sp.x, -960.0f);
  EXPECT_FLOAT_EQ(sp.y, 540.0f);
}

}  // namespace
```

- [ ] **Step 2: Build and verify compile failure**

```bash
cmd //c ".\build-ninja.bat"
```

Expected: build failure on `tests/unit/test_overlay_geometry.cpp` because `src/ui/OverlayGeometry.h` does not exist.

- [ ] **Step 3: Create `src/ui/OverlayGeometry.h`**

```cpp
#pragma once
#include "inference/HandDetector.h"  // for Point2F

namespace vmosue {

// A pixel position in virtual-desktop coordinates. Returned by
// LandmarkToScreen so the renderer can convert to D2D1_POINT_2F as
// needed. We avoid D2D1_POINT_2F here to keep this header free of
// the d2d1.h include chain (the unit tests should not need Direct2D).
struct ScreenPoint {
  float x;
  float y;
};

// Map a normalized hand landmark to a pixel position on the virtual
// desktop. The four metrics come from GetSystemMetrics:
//   virtX = SM_XVIRTUALSCREEN  (can be negative)
//   virtY = SM_YVIRTUALSCREEN  (can be negative)
//   virtW = SM_CXVIRTUALSCREEN
//   virtH = SM_CYVIRTUALSCREEN
//
// The mapping is the obvious 1:1 affine: a landmark at (0, 0) in
// camera coords lands at (virtX, virtY); (1, 1) at (virtX+virtW,
// virtY+virtH). Aspect ratio is NOT preserved here; the spec says
// landmarks map 1:1 to pixels.
inline ScreenPoint LandmarkToScreen(const Point2F& lm,
                                    int virtX, int virtY,
                                    int virtW, int virtH) {
  return ScreenPoint{
      static_cast<float>(virtX) + lm.x * static_cast<float>(virtW),
      static_cast<float>(virtY) + lm.y * static_cast<float>(virtH),
  };
}

}  // namespace vmosue
```

- [ ] **Step 4: Build and run the new test**

```bash
cmd //c ".\build-ninja.bat"
```
Then run the new test:
```bash
cd build && ctest -R OverlayGeometry --output-on-failure
```
Or directly:
```bash
./bin/vmosue_tests --gtest_filter='OverlayGeometry.*'
```

Expected: 4 tests pass.

- [ ] **Step 5: Commit**

```bash
git add src/ui/OverlayGeometry.h tests/unit/test_overlay_geometry.cpp
git commit -m "feat(ui): add LandmarkToScreen mapping (virtual-desktop pixels)"
```

---

## Task 3: Extend `Feedback` struct

**Files:**
- Modify: `src/ui/OverlayWindow.h`

- [ ] **Step 1: Read the current `OverlayWindow.h`**

Verify the current state. The struct today:

```cpp
struct Feedback {
  float cursorX = 0.5f, cursorY = 0.5f;
  float confidence = 0.0f;
  int leftHandCount = 0, rightHandCount = 0;
  bool paused = false;
  bool lastClickLeft = false, lastClickRight = false;
  uint64_t lastClickTimeUs = 0;
};
```

- [ ] **Step 2: Add the new fields and includes**

Replace the includes block (currently `#include <windows.h>` + `#include <d2d1.h>` + `<atomic>` + `<mutex>` + `<thread>`) with:

```cpp
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d2d1.h>
#include <array>
#include <atomic>
#include <mutex>
#include <thread>

#include "inference/HandDetector.h"  // Point2F
#include "ui/HandSkeleton.h"          // (transitively: kHandBones)
```

Add the two new fields to `Feedback` (placed at the end so existing
callers that only use the first 7 fields still get zero-initialized
defaults):

```cpp
struct Feedback {
  float cursorX = 0.5f, cursorY = 0.5f;        // normalized (legacy, unused by v0.4 render)
  float confidence = 0.0f;                    // 0..1
  int leftHandCount = 0, rightHandCount = 0;  // legacy
  bool paused = false;
  bool lastClickLeft = false, lastClickRight = false;
  uint64_t lastClickTimeUs = 0;
  // v0.4: dominant-hand skeleton, rendered by OverlayWindow::Render.
  // hasHand=false means "no right hand detected" — the overlay is
  // blank. When true, landmarks are 21 normalized [0,1] points
  // (post-smoothing) and the render maps them 1:1 to virtual-desktop
  // pixels.
  std::array<Point2F, 21> landmarks{};
  bool hasHand = false;
};
```

Add the cached virtual-desktop metrics to the private section:

```cpp
 private:
  // Cached SM_*VIRTUALSCREEN values, refreshed on Init and on
  // WM_DISPLAYCHANGE. Used by Render to map landmarks to pixels.
  int virtX_ = 0, virtY_ = 0, virtW_ = 0, virtH_ = 0;

  // Set by WM_DISPLAYCHANGE; the render thread sees the flag at
  // the top of Render and calls ResizeRenderTarget before drawing.
  std::atomic<bool> needsResize_{false};

  // Release the existing D2D render target (if any) and re-create
  // it sized to the current window client area. Called from the
  // render thread after a needsResize_ flip.
  void ResizeRenderTarget();
```

- [ ] **Step 3: Build to confirm the header change is well-formed**

```bash
cmd //c ".\build-ninja.bat"
```

Expected: succeeds. (Existing tests should still pass — the new
fields are default-initialized so the default-constructed `Feedback`
remains "no hand".)

Run the full suite to be sure:
```bash
cd build && ctest --output-on-failure
```

Expected: all 93 tests pass (89 existing + 4 from Task 2 + 5 from Task 1 = 98; some pre-existing tests may have been added since the 89 count, so the assertion is "all pass, no regressions"). If the count is off, just verify zero failures.

- [ ] **Step 4: Commit**

```bash
git add src/ui/OverlayWindow.h
git commit -m "feat(ui): extend Feedback with 21 landmarks + hasHand flag"
```

---

## Task 4: Resize `OverlayWindow::Init` to virtual desktop

**Files:**
- Modify: `src/ui/OverlayWindow.cpp`

- [ ] **Step 1: Replace the screen-metric calls in `Init`**

In `src/ui/OverlayWindow.cpp`, find the `Init` method. Replace the
two-line metric block at the top:

```cpp
int sw = GetSystemMetrics(SM_CXSCREEN);
int sh = GetSystemMetrics(SM_CYSCREEN);
```

with:

```cpp
virtX_ = GetSystemMetrics(SM_XVIRTUALSCREEN);
virtY_ = GetSystemMetrics(SM_YVIRTUALSCREEN);
virtW_ = GetSystemMetrics(SM_CXVIRTUALSCREEN);
virtH_ = GetSystemMetrics(SM_CYVIRTUALSCREEN);
```

Then replace the `CreateWindowEx` position/size args (`0, 0, sw, sh`)
with `virtX_, virtY_, virtW_, virtH_`. The new lines should read:

```cpp
hwnd_ = CreateWindowEx(
    WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
    kClassName, L"", WS_POPUP,
    virtX_, virtY_, virtW_, virtH_,
    hwndParent, nullptr,
    GetModuleHandle(nullptr), nullptr);
```

- [ ] **Step 2: Add the `ResizeRenderTarget` method**

At the end of `OverlayWindow.cpp` (just before the closing
`}  // namespace vmosue`), add the implementation:

```cpp
void OverlayWindow::ResizeRenderTarget() {
  if (renderTarget_) {
    renderTarget_->Release();
    renderTarget_ = nullptr;
  }
  if (!d2dFactory_ || !hwnd_) return;
  RECT rc;
  GetClientRect(hwnd_, &rc);
  HRESULT hr = d2dFactory_->CreateHwndRenderTarget(
      D2D1::RenderTargetProperties(),
      D2D1::HwndRenderTargetProperties(
          hwnd_, D2D1::SizeU(rc.right, rc.bottom)),
      &renderTarget_);
  if (FAILED(hr)) {
    // Log and leave renderTarget_ null; the render thread will
    // skip this frame and retry on the next needsResize_ flip.
    VMOSUE_LOG_WARN("OverlayWindow: render target recreate failed hr=0x{:x}", hr);
  }
}
```

- [ ] **Step 3: Build**

```bash
cmd //c ".\build-ninja.bat"
```

Expected: build succeeds. `ResizeRenderTarget` is declared but not
yet called, so the linker should be satisfied.

- [ ] **Step 4: Commit**

```bash
git add src/ui/OverlayWindow.cpp
git commit -m "feat(ui): overlay window sized to virtual desktop"
```

---

## Task 5: Add `WM_DISPLAYCHANGE` handler

**Files:**
- Modify: `src/ui/OverlayWindow.cpp` (WndProc and Render)

- [ ] **Step 1: Update the WndProc to handle `WM_DISPLAYCHANGE`**

In `OverlayWindow::WndProc`, add a `case WM_DISPLAYCHANGE:` arm. The
`WndProc` currently only handles `WM_NCHITTEST` and falls through to
`DefWindowProc` for everything else. Insert a new case in the
switch:

```cpp
case WM_DISPLAYCHANGE: {
  // Monitor configuration changed (plug, unplug, or resolution
  // change). Re-query the four virtual-desktop metrics, resize the
  // window to the new virtual desktop, and ask the render thread
  // to recreate the D2D render target at the next iteration.
  OverlayWindow* self = reinterpret_cast<OverlayWindow*>(
      GetWindowLongPtrW(h, GWLP_USERDATA));
  if (self) {
    self->virtX_ = GetSystemMetrics(SM_XVIRTUALSCREEN);
    self->virtY_ = GetSystemMetrics(SM_YVIRTUALSCREEN);
    self->virtW_ = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    self->virtH_ = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    SetWindowPos(h, HWND_TOP,
                 self->virtX_, self->virtY_,
                 self->virtW_, self->virtH_,
                 SWP_NOACTIVATE | SWP_NOZORDER);
    self->needsResize_.store(true);
  }
  return 0;
}
```

- [ ] **Step 2: Hook `needsResize_` into `Render`**

In `OverlayWindow::Render`, insert the resize check at the very top
(before the `if (!renderTarget_) return;` line):

```cpp
void OverlayWindow::Render() {
  if (needsResize_.exchange(false)) {
    ResizeRenderTarget();
  }
  if (!renderTarget_) return;
  // ... existing code unchanged below this line ...
}
```

The `exchange(false)` returns the previous value and atomically
clears the flag. The render thread does the actual resize (it owns
`renderTarget_`); the WndProc only sets the flag.

- [ ] **Step 3: Build**

```bash
cmd //c ".\build-ninja.bat"
```

Expected: succeeds.

- [ ] **Step 4: Manual smoke test — run with a second monitor event**

This is a manual step: there is no automated test for the
display-change handler. Run the app, leave it idle for ~3 seconds
(the debug window should auto-show), and use Windows Settings to
toggle "Extend these displays" / "Show only on 1" a few times. The
overlay should follow the new virtual desktop dimensions without
crashing. (You can verify the window dimensions by attaching
Spy++/x64dbg, or by simply checking the overlay is still on top
of the right region.)

```bash
cmd //c ".\build-ninja.bat" && taskkill //F //IM vmosue.exe 2>nul & build\bin\vmosue.exe
```

Then toggle display config in Windows Settings. Press Ctrl+C in
the console to exit.

- [ ] **Step 5: Commit**

```bash
git add src/ui/OverlayWindow.cpp
git commit -m "feat(ui): handle WM_DISPLAYCHANGE for monitor config changes"
```

---

## Task 6: Replace the cursor circle with the skeleton render

**Files:**
- Modify: `src/ui/OverlayWindow.cpp`

- [ ] **Step 1: Replace the body of `Render`**

In `src/ui/OverlayWindow.cpp`, find the existing `Render` method
(currently ends with a `DrawEllipse` of radius 14 at the cursor
position, then `brush->Release(); EndDraw();`). Replace the entire
method body (from `Feedback f;` to the `EndDraw()` call,
inclusive) with:

```cpp
void OverlayWindow::Render() {
  if (needsResize_.exchange(false)) {
    ResizeRenderTarget();
  }
  if (!renderTarget_) return;

  Feedback f;
  { std::lock_guard<std::mutex> lk(mu_); f = feedback_; }

  renderTarget_->BeginDraw();
  renderTarget_->Clear(D2D1::ColorF(0, 0, 0, 0));  // transparent

  if (f.hasHand) {
    // Pick a color based on the v0.2 confidence tier. The
    // palette is preserved from the v0.2 debug ring so users
    // familiar with that color scheme get the same feedback
    // signals: green=confident, yellow=marginal, red=poor,
    // gray=paused.
    D2D1_COLOR_F col;
    if (f.paused) {
      col = D2D1::ColorF(0.5f, 0.5f, 0.5f, 0.7f);
    } else if (f.confidence > 0.8f) {
      col = D2D1::ColorF(0.2f, 1.0f, 0.4f, 0.9f);
    } else if (f.confidence > 0.5f) {
      col = D2D1::ColorF(1.0f, 1.0f, 0.2f, 0.9f);
    } else {
      col = D2D1::ColorF(1.0f, 0.2f, 0.2f, 0.9f);
    }

    ID2D1SolidColorBrush* brush = nullptr;
    renderTarget_->CreateSolidColorBrush(col, &brush);
    if (brush) {
      // Map each landmark to a virtual-desktop pixel.
      D2D1_POINT_2F pts[21];
      for (int i = 0; i < 21; ++i) {
        auto sp = LandmarkToScreen(f.landmarks[i],
                                   virtX_, virtY_, virtW_, virtH_);
        pts[i] = D2D1::Point2F(sp.x, sp.y);
      }

      // Bones first (under the joints), then the joint dots on
      // top so the dots are visible at every joint.
      for (const auto& bone : kHandBones) {
        renderTarget_->DrawLine(pts[bone.first], pts[bone.second],
                                brush, 3.0f);
      }
      const float dotRadius = 5.0f;
      for (int i = 0; i < 21; ++i) {
        renderTarget_->DrawEllipse(
            D2D1::Ellipse(pts[i], dotRadius, dotRadius),
            brush, 2.0f);
      }
      brush->Release();
    }
  }
  renderTarget_->EndDraw();
}
```

Make sure to add `#include "ui/OverlayGeometry.h"` and `#include "ui/HandSkeleton.h"` to the top of `OverlayWindow.cpp` (alongside the existing `#include "ui/OverlayWindow.h"`).

- [ ] **Step 2: Build**

```bash
cmd //c ".\build-ninja.bat"
```

Expected: succeeds.

- [ ] **Step 3: Commit**

```bash
git add src/ui/OverlayWindow.cpp
git commit -m "feat(ui): render dominant-hand skeleton (23 bones, 21 dots)"
```

---

## Task 7: Wire `App::stateMachineLoop` to populate the new fields

**Files:**
- Modify: `src/app/App.cpp`

- [ ] **Step 1: Update the Feedback population**

In `src/app/App.cpp`, find the `stateMachineLoop` method. The current
code is:

```cpp
const HandLandmarks* right = nullptr;
for (const auto& h : hands) {
  if (h.handedness == 1) { right = &h; break; }
}
Feedback fb{};
if (right) {
  fb.cursorX = right->points[5].x;
  fb.cursorY = right->points[5].y;
  fb.confidence = right->score;
  fb.rightHandCount = 1;
}
for (const auto& h : hands) {
  if (h.handedness == 0) { fb.leftHandCount = 1; break; }
}
fb.paused = (sm_.State() == GlobalState::Paused);
overlay_.Update(fb);
```

Replace the `if (right) { ... }` block (just the body) with:

```cpp
if (right) {
  fb.cursorX = right->points[5].x;
  fb.cursorY = right->points[5].y;
  fb.confidence = right->score;
  fb.rightHandCount = 1;
  // v0.4: pipe the dominant hand's 21 landmarks to the overlay
  // so it can render the skeleton. points is std::array<Point2F, 21>
  // which matches Feedback::landmarks — assignment does a deep copy.
  fb.landmarks = right->points;
  fb.hasHand = true;
} else {
  // No right hand this tick — clear the flag so the overlay draws
  // nothing (instead of drawing a stale skeleton).
  fb.hasHand = false;
}
```

- [ ] **Step 2: Build**

```bash
cmd //c ".\build-ninja.bat"
```

Expected: succeeds.

- [ ] **Step 3: Commit**

```bash
git add src/app/App.cpp
git commit -m "feat(app): feed dominant hand's 21 landmarks to overlay"
```

---

## Task 8: Final verification

- [ ] **Step 1: Build and run the full test suite**

```bash
cmd //c ".\build-ninja.bat" && cd build && ctest --output-on-failure
```

Expected: all tests pass, including the 9 new ones (5 in `HandSkeleton.*`, 4 in `OverlayGeometry.*`).

- [ ] **Step 2: Manual smoke test — skeleton renders on a real hand**

```bash
taskkill //F //IM vmosue.exe 2>nul
build\bin\vmosue.exe
```

Show your right hand to the camera. Within ~1 second, the 21 joint dots and 23 connecting lines should appear on the overlay, mapped to virtual-desktop pixels. Move your hand to the left third of the camera view; the skeleton should follow into the left monitor (or wherever the secondary monitor sits). Cover the camera or pull your hand out of frame; the overlay should fade to fully transparent (the next `Update` sets `hasHand = false`).

Confirm:
- One hand at a time (only the right hand's skeleton is drawn).
- No cursor ring at the index MCP — only the skeleton.
- When paused (open left hand held still for 1 second), the skeleton turns gray.

- [ ] **Step 3: Commit any final fixes**

If the smoke test revealed visual problems (e.g., colors wrong, lines too thick, dots too small), fix them in a final commit:

```bash
git add -A
git commit -m "fix(ui): tune skeleton visuals after manual smoke test"
```

(Use a more specific message that describes the actual tweak.)

---

## Self-Review Checklist

Before considering the plan complete, verify:

- [x] Each spec section maps to at least one task:
  - "Replace debug ring with skeleton" → Task 6
  - "Landmarks map 1:1 to pixels" → Task 2 (helper) + Task 4 (uses cached virt coords) + Task 6 (calls helper in render)
  - "Single virtual-desktop overlay" → Task 4 (sizing in Init) + Task 5 (resize on display change)
  - "One hand at a time" → Task 7 (picks `right`, ignores left)
  - "Color reflects confidence" → Task 6 (color tiering in Render)
  - "No skeleton when no hand" → Task 6 (`if (f.hasHand)` guard) + Task 7 (`hasHand=false` path)
  - "Unit tests" → Tasks 1, 2
- [x] No placeholders ("TBD", "TODO", "similar to Task N", etc.).
- [x] Type consistency: `LandmarkToScreen` returns `ScreenPoint` everywhere; `kHandBones` is `std::array<std::pair<int,int>, 23>` everywhere; `Feedback::landmarks` is `std::array<Point2F, 21>`; `Feedback::hasHand` is `bool`.
- [x] Each task has explicit commit commands.
- [x] Build command is the documented one (`cmd //c .\build-ninja.bat`).
