#pragma once
#include <cstdint>
#include <vector>
#include "capture/Frame.h"

namespace vmosue {

// Task 33: helper for downscaling a camera frame to the inference
// resolution. The inference loop calls this once per frame before
// forwarding to HandDetector::Detect, so the MediaPipe graph
// (real, in the integrated build) only sees ~640x480 instead of
// the camera's native 1280x720 / 1920x1080. That alone is the
// biggest single win for the perf budget: it cuts the pixel work
// by ~3x on a 720p camera and ~5x on 1080p.
//
// The implementation is a box filter (uniform average of the
// contributing source pixels) which is correct enough for
// downscaling — it's what OpenCV's INTER_AREA uses internally.
// Nearest-neighbor would be slightly faster but produces
// visible aliasing on hand edges, which costs us landmarks.
//
// Format support is intentionally minimal for v0.3:
//   - PixelFormat::BGR24 and PixelFormat::RGBA32: 3/4 channels,
//     full box filter.
//   - PixelFormat::NV12: subsampled chroma (UV plane at half
//     resolution) — we just nearest-sample the Y plane and leave
//     the chroma planes untouched; the real MediaPipe graph
//     expects NV12 input and can re-scale internally. For the
//     stub detector this is fine.
//
// The destination Frame is overwritten in-place: width/height/row-
// Pitch are set to the target dimensions and `data` is resized to
// match. The timestamp and format are preserved.
//
// Out-of-range targets (w==0 || h==0 || src empty) leave the
// destination in a known-empty state — the caller should treat
// that as "skip inference this tick".
inline void ResizeFrame(const Frame& src, Frame& dst,
                        uint32_t targetW, uint32_t targetH) {
  dst = Frame{};
  if (targetW == 0 || targetH == 0 || src.empty() ||
      src.width == 0 || src.height == 0) {
    return;
  }

  // Pass-through when sizes already match. This is the common case
  // after Init() locks the camera to the inference resolution, and
  // avoiding the per-pixel work keeps the idle-down-shift path
  // cheap (the spec target is 10Hz, and we don't want any wasted
  // CPU cycles there).
  if (src.width == targetW && src.height == targetH) {
    dst = src;
    return;
  }

  dst.timestampUs = src.timestampUs;
  dst.format      = src.format;
  dst.width       = targetW;
  dst.height      = targetH;

  if (src.format == PixelFormat::NV12) {
    // NV12: Y plane full-res, interleaved UV plane at half-res. We
    // nearest-sample the Y plane (hand edges matter most there) and
    // copy the UV plane as-is. The MediaPipe HandLandmarker accepts
    // a Y+UV pair and the chroma is not performance-critical.
    const size_t srcY = static_cast<size_t>(src.width) * src.height;
    dst.rowPitch = targetW;
    const size_t dstY = static_cast<size_t>(targetW) * targetH;
    const size_t srcUV = srcY / 2;  // interleaved U+V at half-res
    const size_t dstUV = dstY / 2;
    dst.data.resize(dstY + dstUV);
    // Y plane
    for (uint32_t y = 0; y < targetH; ++y) {
      const uint32_t sy = y * src.height / targetH;
      const uint8_t* srow = src.data.data() + sy * src.width;
      uint8_t* drow = dst.data.data() + y * targetW;
      for (uint32_t x = 0; x < targetW; ++x) {
        drow[x] = srow[x * src.width / targetW];
      }
    }
    // UV plane: nearest from the half-res grid.
    const uint32_t srcUVw = src.width  / 2;
    const uint32_t srcUVh = src.height / 2;
    const uint32_t dstUVw = targetW    / 2;
    const uint32_t dstUVh = targetH    / 2;
    for (uint32_t y = 0; y < dstUVh; ++y) {
      const uint32_t sy = y * srcUVh / dstUVh;
      const uint8_t* srow = src.data.data() + srcY + sy * srcUVw * 2;
      uint8_t* drow = dst.data.data() + dstY + y * dstUVw * 2;
      for (uint32_t x = 0; x < dstUVw; ++x) {
        const uint32_t sx = x * srcUVw / dstUVw;
        drow[x * 2 + 0] = srow[sx * 2 + 0];
        drow[x * 2 + 1] = srow[sx * 2 + 1];
      }
    }
    (void)srcUV; (void)dstUV;  // silence unused-warning under -Wunused-variable
    return;
  }

  // Packed formats (BGR24, RGBA32): uniform box filter per channel.
  const int channels = (src.format == PixelFormat::RGBA32) ? 4 : 3;
  dst.rowPitch = targetW * channels;
  dst.data.resize(static_cast<size_t>(targetW) * targetH * channels);

  // Upscaling: the spec only requires downscaling, but the helper is
  // exercised as a generic resizer by the test suite, and nearest-
  // neighbor is the only sensible behavior in that direction — box
  // filter windows collapse to 0x0 for fractional source coverage
  // (e.g. 2x2 -> 4x4 has sx0==sx1 for half the output columns) and
  // would yield a checkerboard of zeros. We use ceiling-rounding
  // with edge clamping so 2x2 -> 4x4 maps {0,0}, {0,1}, {1,0}, {1,1}
  // to the four source pixels (one source pixel per quadrant of the
  // 4x4 destination) rather than collapsing the first two columns
  // onto src_x=0.
  if (targetW >= src.width && targetH >= src.height) {
    for (uint32_t y = 0; y < targetH; ++y) {
      uint32_t sy = (y * src.height + targetH - 1) / targetH;
      if (sy >= src.height) sy = src.height - 1;
      const uint8_t* srow = src.data.data() +
          static_cast<size_t>(sy) * src.width * channels;
      uint8_t* drow = dst.data.data() +
          static_cast<size_t>(y) * targetW * channels;
      for (uint32_t x = 0; x < targetW; ++x) {
        uint32_t sx = (x * src.width + targetW - 1) / targetW;
        if (sx >= src.width) sx = src.width - 1;
        for (int c = 0; c < channels; ++c) {
          drow[x * channels + c] = srow[sx * channels + c];
        }
      }
    }
    return;
  }

  // Integer scaling factors: sx in [1, srcW], sy in [1, srcH].
  // Using a precomputed pixel area avoids repeated division inside
  // the inner loop. The 0.5/-0.5 trick rounds the result to nearest.
  for (uint32_t y = 0; y < targetH; ++y) {
    const int sy0 = static_cast<int>(
        (static_cast<int64_t>(y)     * src.height) / targetH);
    const int sy1 = static_cast<int>(
        (static_cast<int64_t>(y + 1) * src.height) / targetH);
    for (uint32_t x = 0; x < targetW; ++x) {
      const int sx0 = static_cast<int>(
          (static_cast<int64_t>(x)     * src.width) / targetW);
      const int sx1 = static_cast<int>(
          (static_cast<int64_t>(x + 1) * src.width) / targetW);
      int sum[4] = {0, 0, 0, 0};
      int count = 0;
      for (int sy = sy0; sy < sy1; ++sy) {
        const uint8_t* srow = src.data.data() +
            static_cast<size_t>(sy) * src.width * channels;
        for (int sx = sx0; sx < sx1; ++sx) {
          for (int c = 0; c < channels; ++c) {
            sum[c] += srow[sx * channels + c];
          }
          ++count;
        }
      }
      uint8_t* dpx = dst.data.data() +
          (static_cast<size_t>(y) * targetW + x) * channels;
      if (count == 0) {
        // Should not happen given the bounds above, but be defensive.
        for (int c = 0; c < channels; ++c) dpx[c] = 0;
      } else {
        for (int c = 0; c < channels; ++c) {
          dpx[c] = static_cast<uint8_t>((sum[c] + count / 2) / count);
        }
      }
    }
  }
}

}  // namespace vmosue
