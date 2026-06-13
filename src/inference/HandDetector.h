#pragma once
#include <array>
#include <string>
#include <vector>
#include "capture/Frame.h"
#include "util/Result.h"

namespace vmosue {

struct Point2F { float x = 0.0f, y = 0.0f, z = 0.0f; };

struct HandLandmarks {
  static constexpr int kNumPoints = 21;
  int handedness = 0;     // 0=Left, 1=Right
  float score = 0.0f;
  std::array<Point2F, kNumPoints> points{};  // normalized 0..1
  std::array<Point2F, kNumPoints> world{};   // in meters
};

class HandDetector {
 public:
  struct Config {
    std::string modelPath = "resources/models/hand_landmarker.task";
    int maxHands = 2;
    float minHandConfidence = 0.5f;
    bool useGpu = true;
  };

  Result<void> Init(const Config&);
  std::vector<HandLandmarks> Detect(const Frame&);

  // Task 28: live-update the useGpu flag driven by App's PerfMode.
  // In v0.2 the GPU delegate is a stored-only flag (see
  // HandDetector.cpp); the MediaPipe GPU delegate is not wired up
  // yet. Re-init is not required: the change takes effect on the
  // next Detect() call.
  void SetUseGpu(bool v) { cfg_.useGpu = v; }

  // Read accessor (used by App when refreshing useGpu from Config).
  bool UseGpu() const { return cfg_.useGpu; }

 private:
  Config cfg_;
  bool initialized_ = false;
};

}  // namespace vmosue
