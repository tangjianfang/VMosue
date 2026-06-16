#pragma once
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <atlbase.h>
// Order matters: mfreadwrite.h assumes the base MF types (IMFAttributes,
// IUnknown-derived COM, IMFMediaSource, ...) are already declared.
// mfapi.h provides the runtime, mfobjects.h provides the COM base,
// mfidl.h provides the Media Foundation IDL interfaces. Including
// mfapi alone is not enough on the Windows 10 10.0.26100 SDK.
#include <mfapi.h>
#include <mfobjects.h>
#include <mfidl.h>
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
    // NV12 is the format the Windows Media Foundation driver stack
    // grants for almost every USB webcam (YUY2 is the other common
    // one). RGB32 is not always available at the requested
    // resolution / frame rate, so we ask for NV12 and convert to
    // BGRA in captureLoop() below — see NV12ToBgra().
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
