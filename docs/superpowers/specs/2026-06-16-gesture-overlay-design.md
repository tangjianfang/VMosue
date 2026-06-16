# Gesture Overlay Design (v0.4)

**Date:** 2026-06-16
**Status:** Approved by user, pending spec review

## Problem

The current `OverlayWindow` (v0.2) is a debug-style indicator: a single
colored ring at the cursor position, sized for the primary monitor only.
It does not visualize the user's actual hand, and it doesn't span multiple
monitors.

The user wants:

1. **One transparent-background gesture on screen** (the dominant hand
   skeleton), replacing the debug ring.
2. **1:1 with screen size**: each hand landmark's normalized `[0, 1]`
   coordinate from the camera maps directly to a screen pixel.
3. **Multi-monitor support**: a single overlay covers the entire virtual
   desktop (all monitors as one coordinate space), so the gesture can
   appear on any monitor.

## Scope

In scope:

- Replace the debug ring with a hand-skeleton renderer in `OverlayWindow`.
- Resize / reposition the overlay window to cover the virtual desktop.
- Pipe the dominant hand's 21 landmarks from `App` into the overlay.
- Handle `WM_DISPLAYCHANGE` (monitor configuration changes).
- Unit tests for the coordinate-mapping function and the bone topology
  table.

Out of scope (future work):

- Mirror / mirroring configuration (always-mirror the camera image into the
  overlay background).
- Persistent settings for skeleton color, line thickness.
- Multi-hand rendering (user explicitly wants one hand).
- Animation effects (the skeleton mirrors the raw landmarks; smoothing is
  done by the state machine, not the overlay).

## Architecture

`OverlayWindow` remains the single transparent window. The render thread
keeps its 16 ms cadence. The `Feedback` struct grows a 21-element landmark
array and a `hasHand` flag; no other callers change.

### Coordinate system

The overlay window is placed at the origin of the virtual desktop and
sized to fill it:

| Property            | Win32 metric                       |
| ------------------- | ---------------------------------- |
| Window X            | `GetSystemMetrics(SM_XVIRTUALSCREEN)` |
| Window Y            | `GetSystemMetrics(SM_YVIRTUALSCREEN)` |
| Window width        | `GetSystemMetrics(SM_CXVIRTUALSCREEN)` |
| Window height       | `GetSystemMetrics(SM_CYVIRTUALSCREEN)` |

The virtual-desktop origin can be negative (the primary monitor is not
always at `(0, 0)`). The mapping is:

```
screen_x = SM_XVIRTUALSCREEN + landmark.x * SM_CXVIRTUALSCREEN
screen_y = SM_YVIRTUALSCREEN + landmark.y * SM_CYVIRTUALSCREEN
```

So a landmark at `(0, 0)` (top-left of the camera image) appears at the
top-left of the virtual desktop. A landmark at `(1, 1)` (bottom-right of
the camera image) appears at the bottom-right.

### Hand selection

The "one hand" is the right hand (`handedness == 1`). This matches the
existing cursor-driver logic in `App::stateMachineLoop` — the same hand
that drives the mouse pointer is the one we draw. If no right hand is
detected, `hasHand = false` and nothing is drawn.

The left hand, even if detected, is ignored for the overlay.

### Skeleton topology

The 21 landmarks follow the MediaPipe Hands canonical order (wrist,
thumb, index, middle, ring, pinky; 4 points per finger). The 23 bones
are:

| # | From  | To    | Description                  |
|---|-------|-------|------------------------------|
| 1 | 0     | 1     | wrist → thumb base           |
| 2 | 1     | 2     | thumb base → thumb joint     |
| 3 | 2     | 3     | thumb joint → thumb joint    |
| 4 | 3     | 4     | thumb joint → thumb tip      |
| 5 | 0     | 5     | wrist → index base           |
| 6 | 5     | 6     | index base → index joint     |
| 7 | 6     | 7     | index joint → index joint    |
| 8 | 7     | 8     | index joint → index tip      |
| 9 | 0     | 9     | wrist → middle base          |
| 10| 9     | 10    | middle base → middle joint   |
| 11| 10    | 11    | middle joint → middle joint  |
| 12| 11    | 12    | middle joint → middle tip    |
| 13| 0     | 13    | wrist → ring base            |
| 14| 13    | 14    | ring base → ring joint       |
| 15| 14    | 15    | ring joint → ring joint      |
| 16| 15    | 16    | ring joint → ring tip        |
| 17| 0     | 17    | wrist → pinky base           |
| 18| 17    | 18    | pinky base → pinky joint     |
| 19| 18    | 19    | pinky joint → pinky joint    |
| 20| 19    | 20    | pinky joint → pinky tip      |
| 21| 5     | 9     | palm: index base → middle base |
| 22| 9     | 13    | palm: middle base → ring base |
| 23| 13    | 17    | palm: ring base → pinky base  |

That's 23 bones total: each of the 5 fingers contributes 4 segments
(wrist → base, base → joint, joint → joint, joint → tip), so
5 × 4 = 20; the palm adds 3 more (index → middle → ring → pinky bases),
giving 20 + 3 = **23 line segments**. The implementation will hold the
table as a single source of truth and derive the count via `sizeof()`.

### Visual style

- **Joint dots**: `ID2D1SolidColorBrush`-drawn filled circles, radius 5 px.
- **Bones**: `ID2D1SolidColorBrush`-drawn 3-px lines.
- **Color** (single brush per frame, swapped per confidence tier):
  - `confidence > 0.8`: green `(0.2, 1.0, 0.4, 0.9)`
  - `confidence > 0.5`: yellow `(1.0, 1.0, 0.2, 0.9)`
  - otherwise: red `(1.0, 0.2, 0.2, 0.9)`
  - `paused`: gray `(0.5, 0.5, 0.5, 0.7)`
- **No skeleton when `hasHand == false`** — D2D clear to transparent black,
  no draw calls. The overlay is invisible.

## Data flow

1. `App::stateMachineLoop` pops a `vector<HandLandmarks>` from
   `landmarkQ_`.
2. It already iterates to find the right hand for the cursor. In the same
   pass, copy the right hand's 21 landmarks into a `Feedback` and set
   `hasHand = true`. If no right hand is found, set `hasHand = false`.
3. `overlay_.Update(fb)` — existing thread-safe handoff via `std::mutex`.
4. `OverlayWindow::Render` reads the Feedback snapshot:
   - `Clear` to transparent black.
   - If `hasHand`, draw the 23 bones then the 21 dots.
   - `EndDraw`.

## Code changes

### `src/ui/OverlayWindow.h`

Extend `Feedback` (no breaking changes for existing callers — the new
fields default to "no hand"):

```cpp
struct Feedback {
  float cursorX = 0.5f, cursorY = 0.5f;        // normalized (legacy)
  float confidence = 0.0f;                    // 0..1
  int leftHandCount = 0, rightHandCount = 0;  // legacy
  bool paused = false;
  bool lastClickLeft = false, lastClickRight = false;
  uint64_t lastClickTimeUs = 0;
  // v0.4: dominant hand skeleton, mapped by OverlayWindow.
  // Empty + hasHand=false when no right hand is detected.
  std::array<Point2F, 21> landmarks{};
  bool hasHand = false;
};
```

Include `inference/HandDetector.h` to get `Point2F` (already defined
there as `{x, y, z}`).

Add a private helper:

```cpp
static D2D1_POINT_2F LandmarkToScreen(
    const Point2F& lm,
    int virtX, int virtY, int virtW, int virtH);
```

Add a private member:

```cpp
int virtX_ = 0, virtY_ = 0, virtW_ = 0, virtH_ = 0;
```

…cached at Init and refreshed on `WM_DISPLAYCHANGE`.

### `src/ui/OverlayWindow.cpp`

- `Init`: switch `SM_CXSCREEN`/`SM_CYSCREEN` to
  `SM_CXVIRTUALSCREEN`/`SM_CYVIRTUALSCREEN`. Position the window at
  `(SM_XVIRTUALSCREEN, SM_YVIRTUALSCREEN)`. Cache the four values.
- Add `WndProc` `WM_DISPLAYCHANGE` handler: re-query the four metrics,
  resize the window via `SetWindowPos`, recreate the D2D render target.
- `Render`: replace the ellipse + circle code with the skeleton. Build a
  single brush per frame (color from confidence tier), then for each bone
  `DrawLine`, for each landmark `DrawEllipse` at radius 5.

Add a top-level `kHandBones[]` constant (or a function returning a static
array) holding the 23 `(from, to)` index pairs. The renderer iterates
`std::size(kHandBones)` rather than hard-coding 23.

### `src/app/App.cpp`

In `App::stateMachineLoop`, after picking `right`, populate the new
fields:

```cpp
if (right) {
  fb.cursorX = right->points[5].x;
  fb.cursorY = right->points[5].y;
  fb.confidence = right->score;
  fb.rightHandCount = 1;
  fb.landmarks = right->points;   // std::array<Point2F, 21> copy
  fb.hasHand = true;
} else {
  fb.hasHand = false;
}
```

### Tests

- New file or extension: `tests/unit/test_overlay.cpp`
  - `LandmarkToScreenReturnsVirtualDesktopOriginForZero`: given
    `(0, 0)`, result is `(virtX, virtY)`.
  - `LandmarkToScreenReturnsBottomRightForUnit`: given `(1, 1)`, result
    is `(virtX + virtW, virtY + virtH)`.
  - `LandmarkToScreenHandlesNegativeVirtualOrigin`: given a
    `(virtX = -1920, virtY = 0, virtW = 3840, virtH = 1080)` and a
    landmark at `(0.5, 0.5)`, result is `(0, 540)`.
  - `BoneTopologyHasExpectedIndices`: the bone table has 23 entries and
    all indices are in `[0, 21)`.
  - `BoneTopologyContainsWristConnections`: every finger base
    (1, 5, 9, 13, 17) is connected to wrist (0).
  - `BoneTopologyContainsPalmConnectors`: (5,9), (9,13), (13,17) are
    present.

## Edge cases

| Case                          | Behavior                                                                  |
|-------------------------------|---------------------------------------------------------------------------|
| No hand detected              | `hasHand = false`; nothing drawn; overlay is fully transparent.            |
| Multiple hands detected       | Right hand drawn; left ignored (even if more confident).                  |
| Virtual-desktop size change   | `WM_DISPLAYCHANGE` resizes the window and recreates the D2D render target. |
| Negative virtual-desktop origin | Mapping handles `SM_XVIRTUALSCREEN < 0` correctly.                       |
| Overlay window destroyed early | Render thread checks `renderTarget_` and `hwnd_` before drawing (already does). |
| Hand occluded (low confidence) | Skeleton still draws, but red instead of green (per existing convention). |

## Risks

- **D2D render target recreation on display change** can race the render
  thread. Mitigation: gate the recreate behind the same `std::mutex` used
  for `Feedback`.
- **Mouse cursor over the overlay** — the overlay already has
  `WS_EX_TRANSPARENT` and `HTTRANSPARENT` in the WndProc, so click-through
  is preserved.
- **Existing `cursorX/Y` and `leftHandCount` fields are now unused for
  rendering** but kept on `Feedback` to avoid breaking callers. They will
  be removed in a future cleanup once nothing references them.

## Success criteria

1. The overlay window covers the full virtual desktop (verified by
   `GetWindowRect`).
2. With a right hand detected and stable, the 21 landmarks and 23 bones
   render at the expected screen-pixel positions (verified visually and
   via a debug dump).
3. With no hand detected, the overlay is fully transparent (verified by
   reading the rendered D2D target alpha).
4. With no right hand but a left hand, no skeleton is drawn.
5. Plugging in a second monitor (or unplugging one) resizes the overlay
   within 200 ms without crashing.
6. Unit tests pass; existing 89 tests still pass.

## Out-of-scope notes

- Mirror mode: not asked for; will not be implemented.
- Multi-hand skeleton: user explicitly requested "one gesture".
- Smoothed-landmark display: the skeleton mirrors the raw landmarks
  the detector returned (post-smoothing, since the state machine
  smooths before publishing). Skeleton shake is therefore the same
  amount of shake the user already sees as cursor shake.
