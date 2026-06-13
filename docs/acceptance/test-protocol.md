# VMosue v1.0.0 — Manual Acceptance Test Protocol

This document describes how to execute each manual acceptance test listed in
the project specification (§3.4). It is the runbook that produces the data
recorded in `v1.0-results.md`.

Each test has four parts:

1. **What is being tested** — the user-facing behaviour.
2. **How to perform it** — concrete steps.
3. **What counts as success** — pass/fail criterion from the spec.
4. **How to record the result** — which column of the results table to fill.

---

## 0. Pre-flight checklist

Before starting the acceptance run, confirm the following. The values are
recorded in `v1.0-results.md` → "Test Environment".

- VMosue v1.0.0 installer has been installed (or the release `vmosue.exe`
  has been placed in a known folder).
- The application has been launched and the first-run calibration tutorial
  completed successfully.
- The camera is connected, in focus, and shows a clear preview in the
  Tutorial / Debug window.
- Ambient lighting is stable; if the room uses automatic dimming, disable
  it for the duration of the run.
- No other mouse / trackpad is being actively used by the operator; this
  prevents contaminating success-rate counts.
- A stopwatch or clock with seconds is available for timed tests (scroll,
  30-minute soak).
- A spreadsheet, paper log, or plain text scratchpad is ready for raw
  per-trial counts.

The seven tests can be performed in any order. Tests 1–5 are short
counted-action tests; tests 6 and 7 are duration / robustness tests.

---

## 1. Single-click — 20 random buttons (target ≥ 99 %)

**What is being tested**
Single click accuracy: thumb-index pinch on the right hand drives a left
mouse-button down + up at the on-screen cursor location, on a non-moving
target.

**How to perform it**

1. Open a window with at least 20 distinct clickable elements at varying
   sizes and on-screen positions. Suggested: a folder containing 20+ files
   in detail view, or a web page with 20 distinct buttons.
2. With VMosue active, move the on-screen cursor to a target, perform one
   thumb-index pinch, and confirm the click registered (e.g. the file is
   selected, the button responds).
3. Repeat for 20 distinct targets. Pick targets of different sizes
   (small icons, large buttons) and at different positions (edges,
   center).
4. Record each trial: success or miss. A miss is when no click
   registered, the wrong button registered, or a click registered at
   the wrong location.

**What counts as success**
≥ 99 % (≥ 19/20 successful).

**How to record**
- Attempts: 20
- Successes: number of successful clicks
- Rate: successes / attempts, in percent

---

## 2. Double-click — 10 folders (target ≥ 95 %)

**What is being tested**
Double-click recognition: two consecutive pinches within the
application-layer double-click window (250–500 ms) trigger a Windows
double-click on the underlying icon.

**How to perform it**

1. Open a folder containing 10 sub-folders in detail / icon view.
2. For each folder, hover the cursor over the icon, perform two pinches
   within ~400 ms of each other, and confirm the folder opens.
3. Record each trial: success or miss. A miss is when only a single
   click registered (folder selected but not opened) or the timing was
   rejected entirely.

**What counts as success**
≥ 95 % (≥ 9/10 successful). Note: timing-sensitive; aim for ~350 ms
between pinches.

**How to record**
- Attempts: 10
- Successes: number of folders that opened
- Rate: successes / attempts, in percent

---

## 3. Drag — 10 icons (target ≥ 95 %)

**What is being tested**
Click-and-hold drag: thumb-index pinch held for > 100 ms initiates a
left-button drag; releasing the pinch drops the icon at the new
location.

**How to perform it**

1. Open a folder with 10 icons. Have an empty destination folder / area
   on the same drive.
2. For each icon:
   a. Hover over the icon.
   b. Pinch and hold for ~500 ms (cursor should show a "grabbed" state
      in the Overlay).
   c. Move the hand to translate the cursor to the drop target.
   d. Release the pinch.
   e. Confirm the icon moved.
3. Record each trial: success or miss.

**What counts as success**
≥ 95 % (≥ 9/10 successful). A miss is when the drag started but was
released prematurely, when the icon did not follow the cursor, or when
the drop registered in the wrong location.

**How to record**
- Attempts: 10
- Successes: number of successful drags
- Rate: successes / attempts, in percent

---

## 4. Scroll — 5-minute webpage (target 0 misfires)

**What is being tested**
Scroll gesture stability: left hand two-finger vertical motion produces
smooth vertical scrolling, without spurious clicks, jitter, or
accidental triggers.

**How to perform it**

1. Open a long web page (e.g. an article, a search results list, or a
   social media feed) in a browser window that fills most of the
   screen.
2. With VMosue active, scroll the page using the configured left-hand
   gesture (typically two-finger up/down — see user docs / gestures.md).
3. Scroll continuously and naturally for 5 minutes — alternate
   directions, change speed, occasionally stop and start.
4. During the 5 minutes, count any unintended event: a click that was
   not requested, a sudden page-jump, a stuck scroll that keeps going
   after the hand stops, or an emergency-stop false positive.

**What counts as success**
0 misfires over the 5-minute window.

**How to record**
- Attempts: 5 minutes (record start and end timestamps)
- Successes: minutes with 0 misfires (or "5" if 0 total)
- Rate: N/A — record misfire count in the Notes column

---

## 5. Right-click — 20 trials (target ≥ 97 %)

**What is being tested**
Air click: forward-then-return Z-axis motion of the right index finger
triggers a right mouse-button down + up at the current cursor location.

**How to perform it**

1. Open a desktop or folder with at least 20 distinct items.
2. For each target:
   a. Hover the cursor over the item.
   b. Perform the air click — push the right index finger toward the
      camera, then retract (see gestures.md for the exact motion).
   c. Confirm a context menu opened.
3. Record each trial: success or miss. A miss is when no menu opened,
   a left-click registered instead, or the menu opened at the wrong
   location.

**What counts as success**
≥ 97 % (≥ 19/20 successful).

**How to record**
- Attempts: 20
- Successes: number of context menus that opened
- Rate: successes / attempts, in percent

---

## 6. 30-minute continuous use — no crashes, no stuck keys

**What is being tested**
Long-session stability: the application remains responsive and the
input injector never leaves a mouse button or modifier in a "down"
state. Includes watchdog behaviour: any background thread that hangs
should trigger SafeRelease + Overlay warning rather than a hard freeze.

**How to perform it**

1. With VMosue active, perform a realistic mix of activities for 30
   uninterrupted minutes:
   - Browse the web (mix of scroll, click, right-click)
   - Open / close windows
   - Drag a few files
   - Pause and resume via the configured gesture
   - Occasionally trigger and dismiss the emergency stop
2. During the run, watch for:
   - Application crash or hang
   - Stuck left button (drag selection persists after release)
   - Stuck right button (persistent context menu)
   - Frozen cursor that does not respond to hand motion
   - Any error popups or Overlay red warnings
3. At the end of 30 minutes, confirm the application is still
   responsive and the cursor still tracks the hand.

**What counts as success**
30 minutes elapsed with no crash, no stuck button, and no frozen
cursor.

**How to record**
- Attempts: 1 (the 30-minute run)
- Successes: 1 if all pass criteria met, 0 otherwise
- Rate: N/A — record any incidents in the Notes column

---

## 7. Hand loss / occlusion — no mouse freeze

**What is being tested**
Recovery robustness: when the hand briefly leaves the frame or is
occluded (rolled chair, passing object, intentional cover), VMosue must
not leave the cursor or any button in a stuck state. When the hand
returns, tracking must resume within ~1 second.

**How to perform it**

1. With VMosue active and the hand visible, perform a normal click (so
   the cursor is at a known position).
2. Briefly remove the hand from the camera frame for ~1–2 seconds.
3. Optionally, before returning the hand, occlude it (cover with a
   book, the other hand, etc.) for ~1 second.
4. Bring the hand back into frame.
5. Repeat steps 1–4 ten times.
6. After each cycle, confirm:
   - The cursor did not freeze at its last position (i.e. it remained
     responsive on return).
   - No mouse button was left pressed.
   - Tracking resumed within ~1 second of the hand re-entering the
     frame.

**What counts as success**
10/10 cycles where the cursor is responsive on return and no button is
stuck.

**How to record**
- Attempts: 10
- Successes: number of clean recoveries
- Rate: successes / attempts, in percent

---

## 8. Recording results

After running each test, copy the raw counts into the corresponding row
of `v1.0-results.md`. For each test:

- Fill in Attempts, Successes, Rate.
- Use the Notes column for:
  - Date / time the test was run.
  - Pass / Fail decision against the spec threshold.
  - Any anomalies, error messages, or qualitative observations.
  - Cross-references to fix commits if a failure was addressed
    (Step 37.2).

If any test fails the spec threshold, do not modify the thresholds.
Instead:

1. File the failure in the "Known Issues" section.
2. Address the bug (Task 37.2) and commit the fix as a separate commit.
3. Re-run only the failing test and update the corresponding row.
4. Record the fix commit hash in the Notes column.

A failure does not block v1.0.0 release — but every failed test must
have a tracking issue and either a fix or an explicit deferral
recorded in Known Issues.