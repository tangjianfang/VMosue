#include "gesture/GestureStateMachine.h"
#include "gesture/HandOpen.h"
#include "input/InputInjector.h"
#include "util/Logger.h"

#include <cassert>
#include <climits>

namespace vmosue {

Result<void> GestureStateMachine::Init(const Config& c) {
  cfg_ = c;
  cursor_.SetConfig(c.cursor);
  click_.SetConfig(c.click);
  airClick_.SetConfig(c.airClick);
  scroll_.SetConfig(c.scroll);
  pause_.SetConfig(c.pause);
  // v0.6: dwell-time calibration. Defaults to 1500ms (the
  // user-facing setting); 0 disables the gate (legacy tests use
  // this). Cooldown is short — just over the OS double-click
  // window — so a held gesture doesn't spam clicks.
  DwellGate::Config dc;
  dc.dwellMs = c.dwellMs;
  dc.cooldownMs = c.dwellCooldownMs;
  dwell_.SetConfig(dc);
  return Result<void>::Ok({});
}

void GestureStateMachine::Pause() {
  state_.store(GlobalState::Paused);
  vmosue::InputInjector::Get().SafeReleaseAll();
  // Task 13 fix (spec bug): the spec's Pause did not set pending_.safeRelease,
  // so a consumer that polls for the flag would never see one. Set it for
  // symmetry with EmergencyStop. The InputInjector side is already released;
  // the flag is a hint to any downstream consumer (e.g. overlay) that a
  // release event just happened.
  std::lock_guard<std::mutex> lk(actionsMu_);
  pending_.safeRelease = true;
}

void GestureStateMachine::Resume() {
  state_.store(GlobalState::Active);
}

void GestureStateMachine::EmergencyStop() {
  state_.store(GlobalState::EmergencyStopped);
  vmosue::InputInjector::Get().SafeReleaseAll();
  // Task 13 fix (spec bug): the spec's EmergencyStop did not set
  // pending_.safeRelease, so the test (and any consumer that relies on the
  // flag) would never observe the event. Set it under the actions mutex.
  std::lock_guard<std::mutex> lk(actionsMu_);
  pending_.safeRelease = true;
}

void GestureStateMachine::Reset() {
  cursor_.Reset();
  click_.Reset();
  airClick_.Reset();
  scroll_.Reset();
  pause_.Reset();
  // v0.6: also clear the dwell counter so a half-completed
  // calibration cannot survive a Pause/Resume or any other
  // explicit Reset.
  dwell_.Reset();
  twoHandOpenActive_ = false;
  twoHandOpenStartMs_ = 0;
  std::lock_guard<std::mutex> lk(actionsMu_);
  pending_ = {};
  // Default-init leaves cursorX at INT_MIN (sentinel = "no target"),
  // which is the correct post-reset state — the consumer must not
  // touch the OS cursor until a real target arrives.
}

void GestureStateMachine::OnLandmarks(const std::vector<HandLandmarks>& hands, int64_t ts, double dt) {
  // dt is forwarded to the cursor controller for future per-frame timing
  // (e.g. rate-limited cursor smoothing in the state machine itself). The
  // click and air-click detectors are time-window based and use the
  // millisecond timestamp `ts` directly.

  // Find the "other" hand (opposite of configured handedness) up front.
  // Task 16 lays the groundwork; Task 19 wires the ScrollDetector that
  // consumes `other`, and Task 20 wires the PauseDetector. The pointers
  // are computed per-call rather than stored, so they cannot dangle
  // across frames.
  const HandLandmarks* other = nullptr;
  {
    int otherHandedness = cfg_.handednessRight ? 0 : 1;
    for (const auto& h : hands) {
      if (h.handedness == otherHandedness) {
        other = &h;
        break;
      }
    }
  }

  // Task 21: detect "primary" (right-by-default) hand for two-hand-open
  // emergency stop. The pointer is local to this call so it cannot dangle
  // across frames. We only need it for the open-hand heuristic; the
  // cursor / click / scroll detectors below use the same lookup pattern.
  const HandLandmarks* primary = nullptr;
  {
    int want = cfg_.handednessRight ? 1 : 0;
    for (const auto& h : hands) {
      if (h.handedness == want) {
        primary = &h;
        break;
      }
    }
  }

  // Task 21: two-hand-open emergency stop. Both hands visibly open for
  // >= cfg.twoHandOpenHoldMs triggers EmergencyStop(). We do this BEFORE
  // the Paused / Active short-circuit so a two-hand-open gesture still
  // works while the system is Paused (it's a separate physical signal).
  // We do NOT fire while already EmergencyStopped (latched terminal
  // state, hotkey / restart only clears it).
  if (primary && other && state_.load() != GlobalState::EmergencyStopped) {
    bool bothOpen = IsHandOpen(*primary) && IsHandOpen(*other);
    if (bothOpen) {
      if (!twoHandOpenActive_) {
        // First frame of a new gesture: stamp the start. We use a bool
        // rather than `startMs_ == 0` to detect "not yet started"
        // because 0 is a valid timestamp.
        twoHandOpenActive_ = true;
        twoHandOpenStartMs_ = ts;
      } else if ((ts - twoHandOpenStartMs_) >= cfg_.twoHandOpenHoldMs) {
        VMOSUE_LOG_WARN("Two-hand-open gesture triggered EmergencyStop");
        // Drain any in-flight actions first so a consumer that polls
        // after the safeRelease sees a clean slate (the pause-toggle
        // branch above does the same). Then call EmergencyStop() OUTSIDE
        // the lock: EmergencyStop() also acquires actionsMu_, and this
        // mutex is non-recursive, so calling it under the lock would
        // deadlock on Windows.
        {
          std::lock_guard<std::mutex> lk(actionsMu_);
          pending_ = {};
        }
        EmergencyStop();
        // The state is now latched -- no further gesture processing.
        return;
      }
    } else {
      // Either hand closed (or we lost a hand) -- reset the timer so the
      // next open gesture has to wait the full hold again. This matches
      // the PauseDetector semantics.
      twoHandOpenActive_ = false;
      twoHandOpenStartMs_ = 0;
    }
  } else {
    // One or both hands missing -- treat as broken gesture.
    twoHandOpenActive_ = false;
    twoHandOpenStartMs_ = 0;
  }

  // Task 20: PauseDetector runs even when Paused, so the user can toggle
  // back to Active. While Paused, all other detectors are short-circuited
  // below. EmergencyStop cannot be exited via gestures — only the hotkey
  // or restart of the app should clear it (see EmergencyStop()).
  if (other && state_.load() != GlobalState::EmergencyStopped) {
    auto pauseEv = pause_.OnLandmarks(*other, ts);
    if (pauseEv == PauseDetector::Event::PauseToggle) {
      GlobalState cur = state_.load();
      // EmergencyStopped is already excluded by the outer guard; the
      // ternary below only needs to distinguish Active vs Paused.
      assert(cur == GlobalState::Active || cur == GlobalState::Paused);
      GlobalState next = (cur == GlobalState::Paused)
                             ? GlobalState::Active
                             : GlobalState::Paused;
      state_.store(next);
      // Drain pending actions so anything in flight on the consumer side
      // does not replay after resume, and reset every detector so an
      // in-progress gesture (mid-click hold, mid-scroll phase) does not
      // resume mid-action. Pause is a soft state, not EmergencyStop, so
      // we deliberately do NOT call SafeReleaseAll: the user just wants
      // to stop emitting events, not cancel a current OS-level button
      // hold. We also do NOT set pending_.safeRelease (that flag is for
      // EmergencyStop; see EmergencyStop() below).
      std::lock_guard<std::mutex> lk(actionsMu_);
      pending_ = {};
      cursor_.Reset();
      click_.Reset();
      airClick_.Reset();
      scroll_.Reset();
    }
  }

  // After pause processing, short-circuit if not Active. Note: the pause
  // detector ran above so a Paused state can be flipped back to Active
  // within this same frame.
  if (state_.load() != GlobalState::Active) return;

  // Task 19: ScrollDetector consumes the "other" hand's index/middle
  // Y motion. Returns a {dy, dx} wheel delta pair (positive = up /
  // right). Run this BEFORE the primary-hand check so scroll still
  // emits wheel events when only the "other" hand is visible (e.g.
  // the user has occluded their primary hand for a moment -- we
  // shouldn't drop scroll input on the floor just because the
  // cursor can't move this frame).
  ScrollDelta scrollDelta = other ? scroll_.OnLandmarks(*other, ts) : ScrollDelta{};

  // Find right hand (or left, when the user is left-handed).
  const HandLandmarks* right = nullptr;
  for (const auto& h : hands) {
    int want = cfg_.handednessRight ? 1 : 0;
    if (h.handedness == want) {
      right = &h;
      break;
    }
  }

  if (!right) {
    // No primary hand: only the scroll branch produced anything.
    // Flush the wheel deltas so a downstream consumer (e.g. App)
    // still sees the scroll input -- it would otherwise be lost
    // until the next frame happens to have both hands.
    if (scrollDelta.dy != 0 || scrollDelta.dx != 0) {
      std::lock_guard<std::mutex> lk(actionsMu_);
      pending_.wheel  += scrollDelta.dy;
      pending_.hWheel += scrollDelta.dx;
    }
    return;
  }

  ActionSet local;

  // Task 13 fix (spec bug): CursorController::OnLandmarks previously
  // returned void, dropping the pixel delta on the floor. We changed its
  // signature to accept an ActionSet& out-param so the delta is propagated
  // through the state machine to the consumer thread.
  cursor_.OnLandmarks(*right, local, dt);

  auto clickEv = click_.OnLandmarks(*right, ts);
  switch (clickEv) {
    case ClickEvent::LeftClick:       local.leftClick = true; break;
    case ClickEvent::LeftDoubleClick: local.leftDoubleClick = true; break;
    case ClickEvent::LeftDown:        local.leftDown = true; break;        // legacy fallback (shouldn't fire)
    case ClickEvent::LeftUp:          local.leftUp = true; break;          // legacy fallback (shouldn't fire)
    case ClickEvent::LeftDragStart:   local.leftDown = true; break;        // DragStart maps to LMB down
    case ClickEvent::LeftDragEnd:     local.leftUp = true; break;          // DragEnd maps to LMB up
    case ClickEvent::MiddleClick:     local.middleClick = true; break;    // thumb-middle pinch
    default: break;
  }
  // Right-click (push-toward-camera) arbitration. The air-click and
  // pinch detectors run on the same hand independently, so a frame
  // that both ends a pinch-drag (leftUp) and completes a forward
  // push could inject `leftUp + rightClick` together — the OS would
  // see a left-release racing a right-click, which apps interpret
  // unpredictably. We run the detector unconditionally so its phase
  // state stays current, but drop the resulting click when any
  // left/middle button event already fired this frame. This extends
  // the existing "left wins" priority (used for the middle click) to
  // the right click.
  bool leftBusy = local.leftClick || local.leftDoubleClick ||
                  local.leftDown || local.leftUp || local.middleClick;
  if (airClick_.OnLandmarks(*right, ts) == AirClickEvent::RightClick &&
      !leftBusy) {
    local.rightClick = true;
  }
  if (local.leftClick || local.leftDown || local.leftUp ||
      local.leftDoubleClick || local.rightClick || local.middleClick ||
      local.safeRelease ||
      local.cursorX != INT_MIN) {
    VMOSUE_LOG_DEBUG("Actions: cx={} cy={} click={} dbl={} down={} up={} "
                     "right={} middle={}",
        local.cursorX, local.cursorY,
        local.leftClick, local.leftDoubleClick, local.leftDown, local.leftUp,
        local.rightClick, local.middleClick);
  }
  // v0.6: dwell-time calibration. Run `local` through the
  // DwellGate; only continuously-asserted one-shot actions come
  // out the other side. Cursor, wheel, and sustained LMB
  // (leftDown/leftUp for drag) pass through unchanged.
  ActionSet gated = dwell_.Process(local, ts);
  std::lock_guard<std::mutex> lk(actionsMu_);
  // Cursor target is an absolute screen position, not a delta. The
  // latest frame always wins: a slow consumer polling N frames of
  // pending cursorX should jump to the freshest target rather than
  // accumulate a meaningless sum. Skipping the assignment on frames
  // without a hand (cursorX == INT_MIN sentinel) keeps the previous
  // target visible until a new one arrives, so the OS cursor doesn't
  // visibly freeze between detections.
  if (gated.cursorX != INT_MIN) {
    pending_.cursorX = gated.cursorX;
    pending_.cursorY = gated.cursorY;
  }
  pending_.wheel  += scrollDelta.dy;
  pending_.hWheel += scrollDelta.dx;
  if (gated.leftClick)       pending_.leftClick = true;
  if (gated.leftDoubleClick) pending_.leftDoubleClick = true;
  if (gated.leftDown)        pending_.leftDown = true;
  if (gated.leftUp)          pending_.leftUp = true;
  if (gated.rightClick)      pending_.rightClick = true;
  if (gated.middleClick)     pending_.middleClick = true;
  if (gated.safeRelease)     pending_.safeRelease = true;
}

ActionSet GestureStateMachine::ConsumeActions() {
  std::lock_guard<std::mutex> lk(actionsMu_);
  ActionSet out = pending_;
  pending_ = {};
  return out;
}

}  // namespace vmosue
