#include "gesture/GestureStateMachine.h"
#include "input/InputInjector.h"
#include "util/Logger.h"

namespace vmosue {

Result<void> GestureStateMachine::Init(const Config& c) {
  cfg_ = c;
  cursor_.SetConfig(c.cursor);
  click_.SetConfig(c.click);
  airClick_.SetConfig(c.airClick);
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
  std::lock_guard<std::mutex> lk(actionsMu_);
  pending_ = {};
}

void GestureStateMachine::OnLandmarks(const std::vector<HandLandmarks>& hands, int64_t ts, double dt) {
  // dt is forwarded to the cursor controller for future per-frame timing
  // (e.g. rate-limited cursor smoothing in the state machine itself). The
  // click and air-click detectors are time-window based and use the
  // millisecond timestamp `ts` directly.
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
    case ClickEvent::LeftDown:        local.leftDown = true; break;
    case ClickEvent::LeftUp:          local.leftUp = true; break;
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
