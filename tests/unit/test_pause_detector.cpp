#include <gtest/gtest.h>
#include "gesture/PauseDetector.h"

using vmosue::PauseDetector;

static vmosue::HandLandmarks openHand() {
  vmosue::HandLandmarks lm{};
  // fingertips above MCPs (in normalized y, image origin top-left, so y< = up)
  lm.points[5]  = {0.5f, 0.7f, 0.0f};  // index MCP
  lm.points[8]  = {0.5f, 0.5f, 0.0f};  // index tip (above MCP)
  lm.points[9]  = {0.5f, 0.7f, 0.0f};  // middle MCP
  lm.points[12] = {0.5f, 0.5f, 0.0f};  // middle tip
  lm.points[13] = {0.5f, 0.7f, 0.0f};  // ring MCP
  lm.points[16] = {0.5f, 0.5f, 0.0f};  // ring tip
  lm.points[17] = {0.5f, 0.7f, 0.0f};  // pinky MCP
  lm.points[20] = {0.5f, 0.5f, 0.0f};  // pinky tip
  return lm;
}

static vmosue::HandLandmarks closedHand() {
  vmosue::HandLandmarks lm{};
  // fingertips below MCPs (fist)
  lm.points[5]  = {0.5f, 0.5f, 0.0f};
  lm.points[8]  = {0.5f, 0.7f, 0.0f};
  lm.points[9]  = {0.5f, 0.5f, 0.0f};
  lm.points[12] = {0.5f, 0.7f, 0.0f};
  lm.points[13] = {0.5f, 0.5f, 0.0f};
  lm.points[16] = {0.5f, 0.7f, 0.0f};
  lm.points[17] = {0.5f, 0.5f, 0.0f};
  lm.points[20] = {0.5f, 0.7f, 0.0f};
  return lm;
}

TEST(PauseDetector, EmitsPauseToggleOnOpenHold1s) {
  PauseDetector d;
  d.SetConfig({});
  int64_t t = 0;
  EXPECT_EQ(d.OnLandmarks(openHand(), t), vmosue::PauseDetector::Event::None);
  EXPECT_EQ(d.OnLandmarks(openHand(), t+500), vmosue::PauseDetector::Event::None);
  EXPECT_EQ(d.OnLandmarks(openHand(), t+1100), vmosue::PauseDetector::Event::PauseToggle);
  // After toggle, subsequent open-hand calls should NOT re-emit (toggled_ guards)
  EXPECT_EQ(d.OnLandmarks(openHand(), t+1200), vmosue::PauseDetector::Event::None);
}

TEST(PauseDetector, ResetsOnClose) {
  PauseDetector d;
  d.SetConfig({});
  int64_t t = 0;
  EXPECT_EQ(d.OnLandmarks(openHand(), t), vmosue::PauseDetector::Event::None);
  EXPECT_EQ(d.OnLandmarks(openHand(), t+500), vmosue::PauseDetector::Event::None);
  // close the hand at t+800
  EXPECT_EQ(d.OnLandmarks(closedHand(), t+800), vmosue::PauseDetector::Event::None);
  // re-open at t+900: hold starts over
  EXPECT_EQ(d.OnLandmarks(openHand(), t+900), vmosue::PauseDetector::Event::None);
  // at t+1850 (900+1000 hold), toggle
  EXPECT_EQ(d.OnLandmarks(openHand(), t+1850), vmosue::PauseDetector::Event::PauseToggle);
}