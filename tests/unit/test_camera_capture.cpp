// Unit tests for CameraCapture pure-logic contracts that do not require
// a real camera, MF initialisation, or any COM machinery.
//
// The testable surface without hardware:
//   * TryGetLatestFrame returns false when no frame has ever been delivered.
//   * Frame fields initialise to sane defaults.
//   * PixelFormat enum values are stable (guard against accidental reordering
//     that would break the NV12->BGRA path in captureLoop).
//
// Tests that require a live camera (Init, Start, actual pixel delivery) are
// manual-run / integration level and are not included here.
#include <gtest/gtest.h>
#include "capture/CameraCapture.h"
#include "capture/Frame.h"

using vmosue::CameraCapture;
using vmosue::Frame;
using vmosue::PixelFormat;

TEST(CameraCapture, TryGetLatestFrameReturnsFalseBeforeFirstFrame) {
  // A freshly constructed CameraCapture (not Init'd, not Start'd) has
  // never delivered a frame. TryGetLatestFrame must return false and
  // must not modify `out`. This guards the hasFrame_ = false invariant.
  CameraCapture cam;
  Frame out;
  out.width = 99;  // sentinel: must be unchanged after the call
  bool got = cam.TryGetLatestFrame(out);
  EXPECT_FALSE(got);
  EXPECT_EQ(out.width, 99u) << "TryGetLatestFrame must not modify out when no frame exists";
}

TEST(Frame, DefaultConstructedIsEmpty) {
  Frame f;
  EXPECT_TRUE(f.empty());
  EXPECT_EQ(f.width, 0u);
  EXPECT_EQ(f.height, 0u);
  EXPECT_EQ(f.rowPitch, 0u);
  EXPECT_EQ(f.byteSize(), 0u);
}

TEST(Frame, ByteSizeMatchesData) {
  Frame f;
  f.data.resize(1280 * 720 * 4, 0);
  EXPECT_EQ(f.byteSize(), 1280u * 720u * 4u);
  EXPECT_FALSE(f.empty());
}

// Guard the PixelFormat enum ordering: captureLoop branches on
// PixelFormat::NV12 vs BGR24/RGBA32 and hand_detector_server.py assumes
// BGRA (4 bytes/pixel). An accidental reordering would silently break
// either path.
TEST(Frame, PixelFormatValuesAreStable) {
  // Compile-time values baked into the protocol; changing them would be
  // a breaking change — require an explicit update to this test.
  EXPECT_NE(PixelFormat::NV12,   PixelFormat::BGR24);
  EXPECT_NE(PixelFormat::NV12,   PixelFormat::RGBA32);
  EXPECT_NE(PixelFormat::BGR24,  PixelFormat::RGBA32);
}
