#pragma once
#include <chrono>
#include <cstdint>
#include <vector>

namespace vmosue {

enum class PixelFormat { NV12, BGR24, RGBA32 };

struct Frame {
  uint64_t timestampUs = 0;  // monotonic microseconds
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t rowPitch = 0;     // bytes per row (for YUV with padding)
  PixelFormat format = PixelFormat::BGR24;
  std::vector<uint8_t> data;  // raw pixel data, contiguous

  size_t byteSize() const { return data.size(); }
  bool empty() const { return data.empty(); }
};

// Monotonic clock in microseconds, suitable for frame timestamps.
inline uint64_t NowMicros() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

}  // namespace vmosue
