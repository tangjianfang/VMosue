#pragma once
#include "inference/HandDetector.h"

namespace vmosue {

// Heuristic used by both PauseDetector and the two-hand-open emergency
// stop (Task 21). Treats a hand as "open" (palm facing the camera, all
// four fingers extended) when every fingertip's y-coordinate is above
// (smaller than) the corresponding MCP joint. Same y-down convention
// as the rest of the gesture pipeline.
//
// Returns false if `hand` does not have 21 landmarks (defensive -- the
// detector should always emit 21, but tests sometimes construct
// partial HandLandmarks structs).
inline bool IsHandOpen(const HandLandmarks& hand) {
  if (hand.points.size() != 21) return false;
  // Index, middle, ring, pinky. Thumb is excluded because its MCP
  // (point 2) sits in a different plane and the "tip above MCP" test
  // is unreliable for it.
  return (hand.points[8].y  < hand.points[5].y)  &&
         (hand.points[12].y < hand.points[9].y)  &&
         (hand.points[16].y < hand.points[13].y) &&
         (hand.points[20].y < hand.points[17].y);
}

}  // namespace vmosue