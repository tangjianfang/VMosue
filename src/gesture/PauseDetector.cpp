#include "gesture/PauseDetector.h"

namespace vmosue {

void PauseDetector::SetConfig(const Config& c) { cfg_ = c; }
void PauseDetector::Reset() { open_ = false; openStartMs_ = 0; toggled_ = false; }

PauseDetector::Event PauseDetector::OnLandmarks(const HandLandmarks& left, int64_t ts) {
  bool handOpen = false;
  if (left.points.size() == 21) {
    // Heuristic: all four fingertips above MCPs in y
    handOpen = (left.points[8].y < left.points[5].y) &&
               (left.points[12].y < left.points[9].y) &&
               (left.points[16].y < left.points[13].y) &&
               (left.points[20].y < left.points[17].y);
  }
  if (handOpen && !open_) { open_ = true; openStartMs_ = ts; toggled_ = false; }
  else if (!handOpen) { open_ = false; toggled_ = false; }
  if (open_ && !toggled_ && (ts - openStartMs_) >= cfg_.holdMs) {
    toggled_ = true;
    return Event::PauseToggle;
  }
  return Event::None;
}

}  // namespace vmosue