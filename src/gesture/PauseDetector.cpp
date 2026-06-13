#include "gesture/PauseDetector.h"

#include "gesture/HandOpen.h"

namespace vmosue {

void PauseDetector::SetConfig(const Config& c) { cfg_ = c; }
void PauseDetector::Reset() { open_ = false; openStartMs_ = 0; toggled_ = false; }

PauseDetector::Event PauseDetector::OnLandmarks(const HandLandmarks& left, int64_t ts) {
  bool handOpen = IsHandOpen(left);
  if (handOpen && !open_) { open_ = true; openStartMs_ = ts; toggled_ = false; }
  else if (!handOpen) { open_ = false; toggled_ = false; }
  if (open_ && !toggled_ && (ts - openStartMs_) >= cfg_.holdMs) {
    toggled_ = true;
    return Event::PauseToggle;
  }
  return Event::None;
}

}  // namespace vmosue