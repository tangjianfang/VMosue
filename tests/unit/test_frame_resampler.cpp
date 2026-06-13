#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "capture/Frame.h"
#include "util/FrameResampler.h"

namespace {

using vmosue::Frame;
using vmosue::PixelFormat;
using vmosue::ResizeFrame;

// Helper: build a BGR24 frame where every pixel = its row*width+col.
// Lets us assert on individual source/destination pixels without
// hauling in an image library.
Frame MakeBgrRamp(uint32_t w, uint32_t h) {
  Frame f;
  f.width  = w;
  f.height = h;
  f.rowPitch = w * 3;
  f.format = PixelFormat::BGR24;
  f.data.resize(static_cast<size_t>(w) * h * 3);
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      const uint8_t v = static_cast<uint8_t>(y * w + x);
      const size_t i = (static_cast<size_t>(y) * w + x) * 3;
      f.data[i + 0] = v;  // B
      f.data[i + 1] = v;  // G
      f.data[i + 2] = v;  // R
    }
  }
  return f;
}

// Pass-through: src size == dst size must short-circuit to a copy.
TEST(FrameResampler, PassThroughSameSize) {
  Frame src = MakeBgrRamp(8, 6);
  src.timestampUs = 12345;
  Frame dst;
  ResizeFrame(src, dst, 8, 6);
  ASSERT_EQ(dst.width,  8u);
  ASSERT_EQ(dst.height, 6u);
  ASSERT_EQ(dst.data.size(), src.data.size());
  EXPECT_EQ(dst.timestampUs, 12345u);
  EXPECT_EQ(dst.data, src.data);
}

// Downscaling by integer factor must average the contributing
// pixels. 2x2 source -> 1x1 destination where each channel equals
// the per-pixel average (rounded).
TEST(FrameResampler, BoxFilter2x2To1x1) {
  Frame src = MakeBgrRamp(2, 2);
  // Pixels: (0,1,2,3) per channel. Average = 1.5 -> rounds to 2
  // (the implementation uses (sum + count/2) / count).
  Frame dst;
  ResizeFrame(src, dst, 1, 1);
  ASSERT_EQ(dst.data.size(), 3u);
  for (int c = 0; c < 3; ++c) {
    EXPECT_EQ(dst.data[c], 2);
  }
}

// 4x4 -> 2x2 must downscale by 2 in each dim. Each output pixel
// averages 4 source pixels. For a ramp 0..15 the four 2x2 blocks
// are:
//   {0,1,4,5}  -> avg 2 (rounds 2.5->2 with count/2 trick? sum=10
//                  + 4/2 = 12 / 4 = 3? Let's compute exactly.)
//   {2,3,6,7}  -> sum=18, (18+2)/4=5
//   {8,9,12,13}-> sum=42, (42+2)/4=11
//   {10,11,14,15}-> sum=50, (50+2)/4=13
TEST(FrameResampler, BoxFilter4x4To2x2) {
  Frame src = MakeBgrRamp(4, 4);
  Frame dst;
  ResizeFrame(src, dst, 2, 2);
  ASSERT_EQ(dst.data.size(), 2u * 2u * 3u);
  const std::vector<uint8_t> expected = {
      3, 3, 3,
      5, 5, 5,
      11, 11, 11,
      13, 13, 13,
  };
  EXPECT_EQ(dst.data, expected);
}

// Upscaling must also work (the spec says "downscale", but the
// helper is generic). 2x2 -> 4x4 simply replicates — each
// destination pixel maps to a single source pixel.
TEST(FrameResampler, Upscale2x2To4x4) {
  Frame src = MakeBgrRamp(2, 2);
  Frame dst;
  ResizeFrame(src, dst, 4, 4);
  ASSERT_EQ(dst.data.size(), 4u * 4u * 3u);
  // Each output pixel replicates one source pixel:
  //   (0,0): src(0,0)=0   (1,0): src(0,1)=1   (2,0): src(0,1)=1   (3,0): src(0,1)=1
  //   (0,1): src(1,0)=2   ...
  // We just spot-check a few points.
  const auto at = [&](int x, int y, int c) {
    return dst.data[(static_cast<size_t>(y) * 4 + x) * 3 + c];
  };
  EXPECT_EQ(at(0, 0, 0), 0);
  EXPECT_EQ(at(0, 1, 0), 2);
  EXPECT_EQ(at(1, 0, 0), 1);
  EXPECT_EQ(at(1, 1, 0), 3);
}

// An empty source must produce an empty destination (caller
// should skip inference).
TEST(FrameResampler, EmptySourceYieldsEmptyDest) {
  Frame src;
  Frame dst;
  ResizeFrame(src, dst, 640, 480);
  EXPECT_TRUE(dst.empty());
  EXPECT_EQ(dst.width,  0u);
  EXPECT_EQ(dst.height, 0u);
}

// Zero target dimensions must produce an empty destination.
TEST(FrameResampler, ZeroTargetYieldsEmptyDest) {
  Frame src = MakeBgrRamp(8, 8);
  Frame dst;
  ResizeFrame(src, dst, 0, 0);
  EXPECT_TRUE(dst.empty());
}

// NV12 pass-through uses the Y plane; we make sure the row pitch
// is set correctly so MediaPipe (or a future real graph) can
// index into it.
TEST(FrameResampler, Nv12RespectsRowPitch) {
  Frame src;
  src.width = 8;
  src.height = 4;
  src.format = PixelFormat::NV12;
  // Y plane 8x4, UV plane 4x2 (interleaved).
  const size_t ySize = 8u * 4u;
  const size_t uvSize = 4u * 2u * 2u;
  src.data.resize(ySize + uvSize);
  for (size_t i = 0; i < ySize; ++i) src.data[i] = static_cast<uint8_t>(i);
  for (size_t i = 0; i < uvSize; ++i) src.data[ySize + i] = 0x80;
  src.rowPitch = 8;

  Frame dst;
  ResizeFrame(src, dst, 4, 2);
  EXPECT_EQ(dst.format, PixelFormat::NV12);
  EXPECT_EQ(dst.width,  4u);
  EXPECT_EQ(dst.height, 2u);
  EXPECT_EQ(dst.rowPitch, 4u);
  // Y plane is 4*2=8 bytes, UV plane is 2*1*2=4 bytes, total 12.
  EXPECT_EQ(dst.data.size(), 8u + 4u);
}

}  // namespace
