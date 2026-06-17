#pragma once
#include <optional>
#include "inference/HandDetector.h"

namespace vmosue {

enum class AirClickEvent { None, RightClick };

class AirClickDetector {
 public:
  struct Config {
    float zApproachThreshold = 0.02f;   // world-units
    int windowMs = 200;                // total approach+retreat
    int minWindowMs = 80;
    int cooldownMs = 300;
  };

  void SetConfig(const Config&);
  AirClickEvent OnLandmarks(const HandLandmarks& right, int64_t ts);
  void Reset();

  // v0.6.2: true while the index fingertip is in the "Approach"
  // phase (pushed toward camera). DwellGate uses this as the
  // right-click-held signal so the air-click dwell accumulates
  // across consecutive frames and only commits on the retreat.
  bool IsApproaching() const { return phase_ == Phase::Approach; }

 private:
  // Two-phase model: Idle waits for the index fingertip to push toward
  // the camera (Approach); Approach waits for it to retract within the
  // timing window (-> RightClick) or time out (-> Idle). The earlier
  // `Retreat` enumerator was never entered and has been removed.
  enum class Phase { Idle, Approach };
  Config cfg_;
  Phase phase_ = Phase::Idle;
  int64_t phaseStartMs_ = 0;
  // std::nullopt means "never clicked"; using 0 as a sentinel would falsely
  // early-return when ts is small (cooldownMs check would skip the very
  // first frame at t < cooldownMs).
  std::optional<int64_t> lastClickMs_;
};

}  // namespace vmosue