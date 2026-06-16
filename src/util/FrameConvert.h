#pragma once
// Pixel format conversion utilities for the camera preview.
//
// The debug window (src/ui/DebugWindow.cpp) is the only consumer in
// the current code, but the conversion math is pure (no Win32 / D2D
// dependency), so it lives in src/util/ rather than the UI module
// for two reasons:
//   1. tests/unit/test_frame_convert.cpp can link it without pulling
//      in d2d1, dwrite, or any of the Win32 GUI surface;
//   2. any future offline tool (e.g. a unit test fixture loader for
//      recorded camera frames) can reuse the same conversion.
//
// Supported conversions:
//   - NV12 (YUV 4:2:0 semi-planar, limited range) -> BGRA
//   - BGR24 -> BGRA
//   - RGBA32 (already BGRA bytes in memory; see comment in
//     capture/Frame.h about the naming) -> BGRA (pass-through copy)
//
// All are exposed as free functions taking a Frame (capture/Frame.h)
// and returning a heap-allocated BGRA buffer. Empty vector = invalid
// input (zero dimensions, wrong format, undersized data).

#include <cstdint>
#include <vector>

#include "capture/Frame.h"

namespace vmosue {

std::vector<uint8_t> Nv12FrameToBgra(const Frame& f);
std::vector<uint8_t> Bgr24FrameToBgra(const Frame& f);
std::vector<uint8_t> Rgba32FrameToBgra(const Frame& f);

}  // namespace vmosue
