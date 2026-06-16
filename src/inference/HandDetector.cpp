#include "inference/HandDetector.h"

#include "util/Logger.h"
#include <utility>

// MediaPipe Tasks C++ integration.
//
// The full MediaPipe C API lives in
//   mediapipe/tasks/c/vision/hand_landmarker.h
// which is fetched by FetchContent in src/inference/CMakeLists.txt.
//
// This build environment cannot reach github.com or storage.googleapis.com
// (see docs/build-notes.md), so we deliberately do NOT include the
// MediaPipe header here. Doing so would prevent the file from compiling
// standalone and would make MSVC parse-check fail.
//
// Instead we provide a small HandLandmarkerWrapper that mirrors the
// lifecycle of the C opaque type (Create -> Detect -> Destroy). When
// network access is available and the FetchContent step succeeds, swap
// the implementation of HandLandmarkerWrapper for the real C calls:
//
//   HandLandmarkerWrapper(const char* model, int max_hands, float min_conf) {
//     HandLandmarkerOptionsGraph options = {
//       .base_options.model_asset_path = model,
//       .base_options.delegate = delegate,
//     };
//     HandLandmarkerOptionsGraph num_hands = {.num_hands = max_hands};
//     HandLandmarkerOptions opts = {
//       .base_options = options.base_options,
//       .running_mode = IMAGE,
//       .num_hands = max_hands,
//       .min_hand_detection_confidence = min_conf,
//     };
//     landmarker_ = HandLandmarker_Create(&opts);
//   }
//   ~HandLandmarkerWrapper() { if (landmarker_) HandLandmarker_Close(landmarker_); }
//   ...
//
// The full version requires the actual API header to be present. The
// signatures have changed across MediaPipe releases, so the exact
// field names above should be verified against the fetched source under
// build/_deps/mediapipe_tasks-src/ at integration time.

namespace vmosue {

namespace {

// Placeholder for the MediaPipe HandLandmarker C handle. v0.1 leaves it
// as a sentinel; the real opaque pointer type (HandLandmarker*) will
// replace `void*` once the header is available.
struct HandLandmarkerWrapper {
  void* handle = nullptr;  // would be HandLandmarker* in real build
  int max_hands = 1;
  float min_conf = 0.5f;
  char model_path[260] = {0};

  bool Create(const char* model, int max_hands_in, float min_conf_in) {
    if (!model || !*model) return false;
    // Truncate-copy; model_path is a fixed-size buffer. strncpy triggers
    // MSVC C4996 unless _CRT_SECURE_NO_WARNINGS is set, so use the
    // non-deprecated form explicitly.
    const size_t cap = sizeof(model_path);
    size_t i = 0;
    for (; i < cap - 1 && model[i] != '\0'; ++i) model_path[i] = model[i];
    model_path[i] = '\0';
    max_hands = max_hands_in;
    min_conf = min_conf_in;
    // In the real build, set handle = HandLandmarker_Create(&opts);
    // and return handle != nullptr. For v0.1 we leave handle null so
    // Detect() short-circuits to "no hands".
    return true;
  }
  void Destroy() {
    // if (handle) HandLandmarker_Close(handle);
    handle = nullptr;
  }
};

// simple global for v0.1; later move to member
HandLandmarkerWrapper g_landmarker;

}  // namespace

Result<void> HandDetector::Init(const Config& cfg) {
  cfg_ = cfg;
  if (!g_landmarker.Create(cfg_.modelPath.c_str(), cfg_.maxHands,
                           cfg_.minHandConfidence)) {
    initialized_ = false;
    return Result<void>::Err("HandLandmarkerWrapper::Create failed for model: " +
                             cfg_.modelPath);
  }
  // Be explicit that the model file was opened but no real detection
  // is wired up: this build environment cannot reach github.com /
  // storage.googleapis.com to fetch MediaPipe (see docs/build-notes.md),
  // so Detect() returns an empty vector until the real C API is
  // linked. Without this line the log "HandDetector initialized:"
  // misled users into thinking hand tracking was live. We log at
  // WARN so it's hard to miss; the message says "does NOT load"
  // explicitly because "STUB mode" was misread as "still loading".
  initialized_ = true;
  VMOSUE_LOG_WARN(
      "HandDetector: NO real hand tracking (MediaPipe is not "
      "linked in this build). Detect() will return no hands; "
      "gesture / overlay is driven only by the DebugWindow. "
      "model path is recorded for reference: {}, maxHands={}",
      cfg_.modelPath, cfg_.maxHands);
  return Result<void>::Ok({});
}

std::vector<HandLandmarks> HandDetector::Detect(const Frame& frame) {
  std::vector<HandLandmarks> out;
  if (!initialized_ || frame.empty()) return out;

  // Build MediaPipe Image (NV12 or RGB) from frame.
  //   In the real build, construct an MpImage from frame.data + frame.format
  //   and width/height, then call HandLandmarker_DetectImage(g_landmarker.handle, &mp_image, ...).
  //
  // For each detected hand, populate HandLandmarks{points, world, handedness, score}.
  //
  // IMPORTANT: the actual MediaPipe API has changed across releases. Always refer to
  // docs/superpowers/specs/2026-06-13-vmosue-design.md §2.2 and the linked MediaPipe
  // C++ tasks reference for the exact functions in the version you fetched.

  // v0.1 stub: no MediaPipe available offline, so we return an empty result.
  return out;
}

}  // namespace vmosue
