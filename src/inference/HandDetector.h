#pragma once
#include <array>
#include <string>
#include <vector>
#include "capture/Frame.h"
#include "inference/HandStabilityFilter.h"
#include "util/Result.h"

// v0.3: HandDetector drives a Python subprocess (MediaPipe Hands).
// The subprocess's Win32 handles are managed by an internal RAII
// struct defined in HandDetector.cpp; the destructor for that
// struct needs the complete type, which is why we use a raw
// pointer here (a unique_ptr would require the full type to be
// visible at the point where HandDetector's destructor is
// instantiated, including in test TUs that only see this header).

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
  HandDetector() = default;
  // Destructor is defined in HandDetector.cpp: the std::unique_ptr
  // member needs the complete ChildHandles type to call its dtor,
  // which a forward declaration in this header can't supply.
  ~HandDetector();
  struct Config {
    std::string modelPath = "resources/models/hand_landmarker.task";
    int maxHands = 2;
    // Score floor for phantom-hand rejection. The MediaPipe model
    // returns up to num_hands detections per frame; when only one
    // real hand is visible the second slot is often filled with a
    // low-confidence phantom (~0.4-0.55). The C++ side applies its
    // own adaptive score-gap filter (AdaptiveController::MinHandScore,
    // hard floor 0.3) so we can afford a lower source-side floor and
    // keep more borderline-real hands in the candidate set. Also
    // forwarded to MediaPipe's `min_hand_detection_confidence` and
    // `min_hand_presence_confidence` via the Python server.
    float minHandConfidence = 0.4f;
    bool useGpu = true;
    // Task 33: target resolution of the frame the detector will see.
    // The App downsamples the camera frame to these dimensions
    // before calling Detect(). For v0.3 the values are stored-only
    // (the stub doesn't actually consume pixels) but wiring them
    // through now means the real MediaPipe graph can be configured
    // at integration time without changing the App.
    int inferenceWidth = 640;
    int inferenceHeight = 480;
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

  // Task 33: live-update the inference frame size when the App
  // reallocates its downscale buffer (e.g., on a camera-mode
  // switch). The stub treats it as a stored value; the real
  // MediaPipe graph will be reconfigured with the new image
  // dimensions.
  void SetFrameSize(int w, int h) {
    if (w > 0) cfg_.inferenceWidth = w;
    if (h > 0) cfg_.inferenceHeight = h;
  }

  int InferenceWidth()  const { return cfg_.inferenceWidth; }
  int InferenceHeight() const { return cfg_.inferenceHeight; }

  // v0.6: anti-phantom stability filter. Owned here (rather than in
  // the inference loop) so it lives as long as the detector does;
  // GestureStateMachine::Reset() also calls Reset() on it so a
  // "phantom accepted" can't bleed across a Pause/Resume cycle.
  void ResetStabilityFilter() { stability_.Reset(); }
  const HandStabilityFilter& StabilityForTest() const { return stability_; }

  // Visible for testing: how many consecutive automatic respawns have
  // been attempted since the last healthy frame. Reset to 0 after any
  // successful Detect() round-trip.
  int RespawnCount() const { return respawnCount_; }

  // v0.6: map the user-facing antiInterference string to a
  // numeric min-hand-confidence forwarded to MediaPipe. The
  // mapping is intentionally coarse — the per-handedness
  // HandStabilityFilter on the C++ side is the stronger
  // anti-phantom signal; this is just a pre-filter at the
  // detector's own confidence gate. The App also applies the
  // same mapping when it constructs the HandDetector::Config,
  // so unit tests can verify the table directly.
  static float MinHandConfidenceForAntiInterference(
      const std::string& antiInterference) {
    if (antiInterference == "off")  return 0.4f;
    if (antiInterference == "low")  return 0.5f;
    if (antiInterference == "high") return 0.7f;
    return 0.6f;  // "medium" or unknown
  }

 private:
  // Attempt to bring the Python subprocess back up after it has died
  // (GetExitCodeProcess != STILL_ACTIVE) or after an I/O failure.
  // Bounded by kMaxRespawns within a single dead streak so a
  // permanently broken environment (e.g. mediapipe uninstalled)
  // doesn't spin the CPU respawning forever. Returns true if a fresh
  // child is ready. Defined in the .cpp.
  bool TryRespawn();

  Config cfg_;
  bool initialized_ = false;
  int respawnCount_ = 0;
  static constexpr int kMaxRespawns = 3;
  // v0.6 anti-phantom filter state. Tracked per handedness; survives
  // across Detect() calls so a hand has to be continuously above the
  // adaptive floor for kStabilityFrames (=3) frames before it is
  // surfaced to the gesture pipeline.
  HandStabilityFilter stability_{};
  // v0.3: Python subprocess driving MediaPipe Hands. The full
  // definition of ChildHandles lives in HandDetector.cpp (it owns
  // Win32 HANDLEs whose dtor needs the complete type). We use a
  // raw pointer in the header so HandDetector's destructor can be
  // declared here but defined in the .cpp.
  struct ChildHandles* child_ = nullptr;
};

// Forward declaration of the subprocess-handle holder. Defined in
// HandDetector.cpp.
struct vmosue::ChildHandles;

}  // namespace vmosue
