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

 private:
  enum class Phase { Idle, Approach, Retreat };
  Config cfg_;
  Phase phase_ = Phase::Idle;
  int64_t phaseStartMs_ = 0;
  // std::nullopt means "never clicked"; using 0 as a sentinel would falsely
  // early-return when ts is small (cooldownMs check would skip the very
  // first frame at t < cooldownMs).
  std::optional<int64_t> lastClickMs_;
  float baseZ_ = 0.0f;
  bool palmStableAtStart_ = false;
};

}  // namespace vmosue