#pragma once
#include <array>
#include <optional>
#include "inference/HandDetector.h"
#include "util/OneEuroFilter.h"

namespace vmosue {

// Lightweight One-Euro filter applied independently to each (x, y, z)
// component of a hand's 21 landmarks. With default tuning it suppresses
// high-frequency jitter from the hand landmarker without introducing
// visible lag on deliberate cursor movement.
//
// Implementation note: OneEuroFilter is not default-constructible, so we
// keep the per-point filter instances inside std::optional and lazily
// construct them on the first call to Smooth().
class LandmarkSmoother {
 public:
  LandmarkSmoother(double freq = 30.0, double mincutoff = 2.5,
                   double beta = 0.02)
      : freq_(freq), mincutoff_(mincutoff), beta_(beta), initialized_(false) {
    for (auto& f : xFilters_) f.emplace(freq, mincutoff, beta);
    for (auto& f : yFilters_) f.emplace(freq, mincutoff, beta);
    for (auto& f : zFilters_) f.emplace(freq, mincutoff, beta);
  }

  void Smooth(HandLandmarks& lm, double dt) {
    if (!initialized_) {
      // Seed filters with the first observation so the initial output
      // does not snap from 0.0 to the raw value.
      for (int i = 0; i < HandLandmarks::kNumPoints; ++i) {
        xFilters_[i]->Filter(lm.points[i].x, dt);
        yFilters_[i]->Filter(lm.points[i].y, dt);
        zFilters_[i]->Filter(lm.points[i].z, dt);
        xFilters_[i]->Reset();
        yFilters_[i]->Reset();
        zFilters_[i]->Reset();
      }
      initialized_ = true;
      return;
    }
    for (int i = 0; i < HandLandmarks::kNumPoints; ++i) {
      lm.points[i].x = static_cast<float>(xFilters_[i]->Filter(lm.points[i].x, dt));
      lm.points[i].y = static_cast<float>(yFilters_[i]->Filter(lm.points[i].y, dt));
      lm.points[i].z = static_cast<float>(zFilters_[i]->Filter(lm.points[i].z, dt));
    }
  }

  void Reset() {
    for (auto& f : xFilters_) f->Reset();
    for (auto& f : yFilters_) f->Reset();
    for (auto& f : zFilters_) f->Reset();
    initialized_ = false;
  }

  // v0.5: re-tune the One-Euro filter parameters in place from the
  // adaptive controller's noise-floor + motion-magnitude readout.
  // Updates the cached freq/mincutoff/beta and forwards the new
  // values to every per-axis OneEuroFilter instance.
  void AdaptParams(double newFreq, double newMincutoff, double newBeta) {
    freq_ = newFreq;
    mincutoff_ = newMincutoff;
    beta_ = newBeta;
    for (auto& f : xFilters_) f->AdaptParams(newFreq, newMincutoff, newBeta);
    for (auto& f : yFilters_) f->AdaptParams(newFreq, newMincutoff, newBeta);
    for (auto& f : zFilters_) f->AdaptParams(newFreq, newMincutoff, newBeta);
  }

 private:
  std::array<std::optional<OneEuroFilter>, 21> xFilters_;
  std::array<std::optional<OneEuroFilter>, 21> yFilters_;
  std::array<std::optional<OneEuroFilter>, 21> zFilters_;
  double freq_;
  double mincutoff_;
  double beta_;
  bool initialized_;
};

}  // namespace vmosue
