#include "util/FrameConvert.h"

#include <cstdint>
#include <vector>

namespace vmosue {

namespace {

constexpr uint8_t kOpaqueAlpha = 0xFF;

// Branchless-ish byte clamp used by Nv12FrameToBgra. The signed
// integer intermediate can briefly go negative during the
// chroma-mixing math; we have to clamp to [0, 255] before storing
// into a uint8_t (a negative store would underflow and become
// 0xFE / 0xFF depending on twos-complement).
inline uint8_t Clamp8(int v) {
  return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

}  // namespace

std::vector<uint8_t> Bgr24FrameToBgra(const Frame& f) {
  std::vector<uint8_t> out;
  if (f.width == 0 || f.height == 0) return out;
  if (f.format != PixelFormat::BGR24) return out;
  const size_t expected = static_cast<size_t>(f.rowPitch) *
                          static_cast<size_t>(f.height);
  if (f.data.size() < expected) return out;
  out.resize(static_cast<size_t>(f.width) *
             static_cast<size_t>(f.height) * 4u);
  const uint8_t* src = f.data.data();
  uint8_t* dst = out.data();
  for (uint32_t y = 0; y < f.height; ++y) {
    const uint8_t* row = src + static_cast<size_t>(y) * f.rowPitch;
    uint8_t* drow = dst + static_cast<size_t>(y) * f.width * 4u;
    for (uint32_t x = 0; x < f.width; ++x) {
      // BGR -> BGRA (B stays, G stays, R stays, alpha = 255).
      drow[x * 4 + 0] = row[x * 3 + 0];  // B
      drow[x * 4 + 1] = row[x * 3 + 1];  // G
      drow[x * 4 + 2] = row[x * 3 + 2];  // R
      drow[x * 4 + 3] = kOpaqueAlpha;
    }
  }
  return out;
}

// RGBA32 -> BGRA pass-through. The pixel-format enum is named
// RGBA32 (32 bits per pixel) but the byte order in our camera
// pipeline is actually BGRA — see the long comment in
// capture/Frame.h about the naming convention. The bytes
// produced by CameraCapture::NV12ToBgra are already in
// DXGI_FORMAT_B8G8R8A8 layout, which is what D2D wants, so the
// "conversion" is a straight copy. We still validate that the
// buffer is correctly sized (width*height*4) and copy row-by-row
// so a non-trivial rowPitch (e.g. driver-supplied padding) is
// honored. Alpha channel is overwritten to 255 to make the frame
// fully opaque regardless of what the source put there — some
// webcams leave A=0 in MFVideoFormat_RGB32, which renders
// transparent in D2D and produces a black preview.
std::vector<uint8_t> Rgba32FrameToBgra(const Frame& f) {
  std::vector<uint8_t> out;
  if (f.width == 0 || f.height == 0) return out;
  if (f.format != PixelFormat::RGBA32) return out;
  const size_t expected = static_cast<size_t>(f.rowPitch) *
                          static_cast<size_t>(f.height);
  if (f.data.size() < expected) return out;
  out.resize(static_cast<size_t>(f.width) *
             static_cast<size_t>(f.height) * 4u);
  const uint8_t* src = f.data.data();
  uint8_t* dst = out.data();
  for (uint32_t y = 0; y < f.height; ++y) {
    const uint8_t* row = src + static_cast<size_t>(y) * f.rowPitch;
    uint8_t* drow = dst + static_cast<size_t>(y) * f.width * 4u;
    for (uint32_t x = 0; x < f.width; ++x) {
      // BGRA -> BGRA (B stays, G stays, R stays, alpha = 255).
      // The four-line copy is clearer than memcpy here because
      // we want to overwrite the alpha channel.
      drow[x * 4 + 0] = row[x * 4 + 0];  // B
      drow[x * 4 + 1] = row[x * 4 + 1];  // G
      drow[x * 4 + 2] = row[x * 4 + 2];  // R
      drow[x * 4 + 3] = kOpaqueAlpha;    // A forced opaque
    }
  }
  return out;
}

// NV12 -> BGRA. NV12 is YUV 4:2:0 semi-planar, limited (TV/MPEG)
// range: a `width * height` byte Y plane followed by a
// `width * height / 2` byte UV plane interleaved as (U,V,U,V,...)
// at half horizontal and vertical resolution. BT.601 limited-range
// math with fixed-point pre-multiplied constants (>> 10 ≈ / 1024):
//   C = 1192 * (Y - 16)                    // ≈ 1.164 * 1024
//   R = (C                   + 1634*(V-128)) >> 10   // + 1.596
//   G = (C - 400*(U-128) - 833*(V-128)) >> 10         // -.391, -.813
//   B = (C + 2066*(U-128))                >> 10        // + 2.018
// Output is clamped to [0, 255] and packed as BGRA with A=255.
std::vector<uint8_t> Nv12FrameToBgra(const Frame& f) {
  std::vector<uint8_t> out;
  if (f.width == 0 || f.height == 0) return out;
  if (f.format != PixelFormat::NV12) return out;
  const size_t yPlaneBytes = static_cast<size_t>(f.width) *
                             static_cast<size_t>(f.height);
  const size_t uvPlaneBytes = yPlaneBytes / 2u;
  if (f.data.size() < yPlaneBytes + uvPlaneBytes) return out;
  out.resize(yPlaneBytes * 4u);
  const uint8_t* yPlane = f.data.data();
  const uint8_t* uvPlane = yPlane + yPlaneBytes;
  uint8_t* dst = out.data();
  const int W = static_cast<int>(f.width);
  const int H = static_cast<int>(f.height);
  for (int y = 0; y < H; ++y) {
    const uint8_t* yRow  = yPlane  + static_cast<size_t>(y) * static_cast<size_t>(W);
    const uint8_t* uvRow = uvPlane + static_cast<size_t>(y / 2) * static_cast<size_t>(W);
    uint8_t* drow = dst + static_cast<size_t>(y) * static_cast<size_t>(W) * 4u;
    for (int x = 0; x < W; ++x) {
      const int Y = static_cast<int>(yRow[x]);
      const int U = static_cast<int>(uvRow[(x / 2) * 2 + 0]);
      const int V = static_cast<int>(uvRow[(x / 2) * 2 + 1]);
      const int C = 1192 * (Y - 16);
      const int R = (C                    + 1634 * (V - 128)) >> 10;
      const int G = (C -  400 * (U - 128) -  833 * (V - 128)) >> 10;
      const int B = (C + 2066 * (U - 128))                   >> 10;
      drow[x * 4 + 0] = Clamp8(B);
      drow[x * 4 + 1] = Clamp8(G);
      drow[x * 4 + 2] = Clamp8(R);
      drow[x * 4 + 3] = kOpaqueAlpha;
    }
  }
  return out;
}

}  // namespace vmosue
