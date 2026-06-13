#pragma once
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <atlbase.h>
#include <mfreadwrite.h>
#include "capture/Frame.h"
#include "util/Result.h"

namespace vmosue {

class CameraCapture {
 public:
  struct Config {
    uint32_t deviceIndex = 0;
    uint32_t width = 1280;
    uint32_t height = 720;
    uint32_t fps = 60;
    PixelFormat pixelFormat = PixelFormat::NV12;
  };

  CameraCapture();
  ~CameraCapture();

  Result<void> Init(const Config&);
  void Start();
  void Stop();

  // Always returns the most recent frame; false if no frame yet.
  bool TryGetLatestFrame(Frame& out);

  // Task 28: enumerate friendly names of all video capture devices
  // currently visible to Media Foundation. Static so the Settings
  // window can populate its camera dropdown without owning a
  // CameraCapture instance. Returns an empty vector on failure
  // (e.g. when the Media Foundation runtime is not initialized —
  // the SettingsWindow handles that gracefully by offering only a
  // "Default Camera" fallback).
  static std::vector<std::wstring> EnumerateDevices();

 private:
  void captureLoop();
  Config cfg_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  Frame latestFrame_;
  std::mutex frameMutex_;
  std::condition_variable frameCv_;
  bool hasFrame_ = false;
  CComPtr<IMFSourceReader> reader_;
};

}  // namespace vmosue
