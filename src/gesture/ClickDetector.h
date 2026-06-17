#pragma once
#include <optional>
#include "inference/HandDetector.h"

namespace vmosue {

enum class ClickEvent {
  None,
  LeftClick,
  LeftDoubleClick,
  LeftDown,
  LeftUp,
  LeftDragStart,
  LeftDragEnd,
  // Thumb-middle pinch (right hand, landmarks 4 and 12). The
  // left-click and middle-click state machines run in parallel;
  // when both fire on the same frame, left wins (priority order
  // matches how muscle memory typically resolves the two — the
  // index finger is "first" in a pinch sequence).
  MiddleClick,
};

class ClickDetector {
 public:
  struct Config {
    float pinchThresholdNorm = 0.04f;   // 4% of frame
    float releaseThresholdNorm = 0.07f;
    int holdForDragMs = 200;
    int doubleClickWindowMs = 400;
    // When true, a single click is held (not emitted) until the
    // doubleClickWindowMs elapses. If a second click arrives within
    // the window, the held click is discarded and a LeftDoubleClick
    // is emitted instead.
    bool suppressSingleClickInDoubleWindow = false;
  };

  void SetConfig(const Config&);
  ClickEvent OnLandmarks(const HandLandmarks& right, int64_t timestampMs);
  void Reset();

  // v0.6.2: read-only view of the current pinch state. DwellGate uses
  // this to know whether the user is still sustaining the pinch, so
  // it can accumulate dwell across consecutive frames and only
  // commit the click on the release frame. Returns true while in
  // Pinching or Held phase (i.e., fingers are close enough to count
  // as a sustained pinch).
  bool IsLeftPinching() const { return phase_ == Phase::Pinching ||
                                       phase_ == Phase::Held; }
  bool IsMiddlePinching() const { return middlePhase_ == MiddlePhase::Pinching; }

 private:
  enum class Phase { Idle, Pinching, Held };
  Config cfg_;
  Phase phase_ = Phase::Idle;
  int64_t pinchStartMs_ = 0;
  // std::nullopt means "never clicked"; using 0 as a sentinel would falsely
  // pair a very first click (timestamp near 0) with a double-click.
  std::optional<int64_t> lastClickMs_;
  // When suppressSingleClickInDoubleWindow is true, a single click is
  // held here waiting for the double-click window to expire. If a
  // second click arrives in time, the pending click is discarded and
  // a LeftDoubleClick is emitted instead.
  bool pendingSingleClick_ = false;
  int64_t pendingSingleClickStartMs_ = 0;

  // Middle-click state machine (thumb-middle pinch, landmarks 4 and
  // 12). Middle-click is rare in normal use, so we keep this
  // deliberately simple: no double-click, no drag — just Idle ->
  // Pinching on threshold cross, emit MiddleClick on release. Reuses
  // the same pinch / release thresholds the left-click state
  // machine uses (the physical "fingers touch" semantic is
  // identical).
  enum class MiddlePhase { Idle, Pinching };
  MiddlePhase middlePhase_ = MiddlePhase::Idle;
  int64_t middlePinchStartMs_ = 0;
};

}  // namespace vmosue
