#pragma once
#include <optional>
#include "inference/HandDetector.h"
#include "input/InputInjector.h"

namespace vmosue {

// CursorController maps right-hand index-finger motion to system cursor
// movement. The pivot is the index finger MCP (landmark 5) - using MCP
// rather than the fingertip avoids a hard cursor jump on click/press
// gestures that move the fingertip sharply.
//
// All inputs are normalized landmark coordinates in [0, 1] and output deltas
// are pixel deltas computed against an assumed 1920x1080 frame; the real
// display size is substituted at injection time (Task 12 / 14).
class CursorController {
 public:
  struct Config {
    float sensitivityX = 1.5f;
    float sensitivityY = 1.5f;
    float deadZoneNorm = 0.02f;  // 2% of frame
    bool useIndexMcpAsPivot = true;
  };

  void SetConfig(const Config&);
  void OnLandmarks(const HandLandmarks& right, double dt);
  void Reset();

 private:
  Config cfg_;
  std::optional<Point2F> prevPivot_;
  bool initialized_ = false;
};

}  // namespace vmosue
