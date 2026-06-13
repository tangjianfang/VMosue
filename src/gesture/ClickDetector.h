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
};

}  // namespace vmosue
