#include "gesture/GestureStateMachine.h"
#include "input/InputInjector.h"
#include "util/Logger.h"

#include <cassert>

namespace vmosue {

Result<void> GestureStateMachine::Init(const Config& c) {
  cfg_ = c;
  cursor_.SetConfig(c.cursor);
  click_.SetConfig(c.click);
  airClick_.SetConfig(c.airClick);
  scroll_.SetConfig(c.scroll);
  pause_.SetConfig(c.pause);
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
  std::lock_guard<std::mutex> lk(actionsMu_);
  pending_ = {};
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

  // Find right hand (or left, when the user is left-handed).
  const HandLandmarks* right = nullptr;
  for (const auto& h : hands) {
    int want = cfg_.handednessRight ? 1 : 0;
    if (h.handedness == want) {
      right = &h;
      break;
    }
  }
  if (!right) return;

  // Task 19: ScrollDetector consumes the "other" hand's index/middle Y
  // motion. Returns a wheel delta (positive = scroll up).
  int scrollDelta = other ? scroll_.OnLandmarks(*other, ts) : 0;

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
    default: break;
  }
  if (airClick_.OnLandmarks(*right, ts) == AirClickEvent::RightClick) {
    local.rightClick = true;
  }
  if (local.leftClick || local.leftDown || local.leftUp ||
      local.leftDoubleClick || local.rightClick || local.safeRelease ||
      local.cursorDx != 0 || local.cursorDy != 0) {
    VMOSUE_LOG_DEBUG("Actions: dx={} dy={} click={} dbl={} down={} up={} right={}",
        local.cursorDx, local.cursorDy,
        local.leftClick, local.leftDoubleClick, local.leftDown, local.leftUp,
        local.rightClick);
  }
  std::lock_guard<std::mutex> lk(actionsMu_);
  // Cursor deltas are merged additively across frames; the consumer drains
  // them via ConsumeActions. This avoids losing intermediate motion when
  // the consumer polls slower than the camera frame rate.
  pending_.cursorDx += local.cursorDx;
  pending_.cursorDy += local.cursorDy;
  pending_.wheel += scrollDelta;
  if (local.leftClick)       pending_.leftClick = true;
  if (local.leftDoubleClick) pending_.leftDoubleClick = true;
  if (local.leftDown)        pending_.leftDown = true;
  if (local.leftUp)          pending_.leftUp = true;
  if (local.rightClick)      pending_.rightClick = true;
  if (local.safeRelease)     pending_.safeRelease = true;
}

ActionSet GestureStateMachine::ConsumeActions() {
  std::lock_guard<std::mutex> lk(actionsMu_);
  ActionSet out = pending_;
  pending_ = {};
  return out;
}

}  // namespace vmosue
