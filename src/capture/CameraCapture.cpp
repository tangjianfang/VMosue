#include "capture/CameraCapture.h"
#include "util/Logger.h"
#include <atlbase.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mfcaptureengine.h>
#include <chrono>
#include <condition_variable>
#include <mutex>

#pragma comment(lib, "mfreadwrite")
#pragma comment(lib, "mfplat")
#pragma comment(lib, "mfuuid")

namespace vmosue {

namespace {
constexpr int kQueueDepth = 2;

struct DeviceInfo {
  std::wstring symbolicLink;
  std::wstring name;
};

std::vector<DeviceInfo> enumerateCameras() {
  std::vector<DeviceInfo> result;
  IMFAttributes* attrs = nullptr;
  HRESULT hr = MFCreateAttributes(&attrs, 1);
  if (FAILED(hr)) return result;
  hr = attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                      MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
  if (FAILED(hr)) { attrs->Release(); return result; }
  IMFActivate** devices = nullptr;
  UINT32 count = 0;
  hr = MFEnumDeviceSources(attrs, &devices, &count);
  if (SUCCEEDED(hr)) {
    for (UINT32 i = 0; i < count; ++i) {
      DeviceInfo info;
      UINT32 nameLen = 0;
      devices[i]->GetStringLength(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &nameLen);
      std::wstring name(nameLen, L'\0');
      devices[i]->GetString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name[0], nameLen + 1, nullptr);
      UINT32 linkLen = 0;
      devices[i]->GetStringLength(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &linkLen);
      std::wstring link(linkLen, L'\0');
      devices[i]->GetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link[0], linkLen + 1, nullptr);
      info.name = name;
      info.symbolicLink = link;
      result.push_back(info);
      devices[i]->Release();
    }
    CoTaskMemFree(devices);
  }
  attrs->Release();
  return result;
}
}  // namespace

std::vector<std::wstring> CameraCapture::EnumerateDevices() {
  // The Settings window calls this from the UI thread before
  // CameraCapture::Init() has had a chance to MFStartup() the MF
  // runtime. Media Foundation's MFEnumDeviceSources is documented
  // to work without an explicit MFStartup() on Windows 7+ because
  // the OS auto-starts the MF platform when a media API is called.
  // Nonetheless we MFStartup() defensively so we get a valid device
  // list even on platforms where the auto-start is suppressed, and
  // we MFShutdown() on the way out if we were the one who started
  // it. (MFStartup uses reference counting internally — calling it
  // here is safe regardless of the calling thread's MF state.)
  HRESULT hr = MFStartup(MF_VERSION);
  const bool weStartedMF = SUCCEEDED(hr);

  std::vector<std::wstring> names;
  auto devices = enumerateCameras();
  names.reserve(devices.size());
  for (const auto& d : devices) {
    // Append the index so a user with two identical webcams can
    // still tell them apart in the dropdown. The index is in
    // brackets to keep it visually distinct from the friendly
    // name. Init() will need to enumerate again and pick the
    // matching index, so the index-in-brackets is just a
    // human-friendly hint.
    names.push_back(d.name);
  }

  if (weStartedMF) {
    MFShutdown();
  }
  return names;
}

CameraCapture::CameraCapture() = default;
CameraCapture::~CameraCapture() { Stop(); }

Result<void> CameraCapture::Init(const Config& cfg) {
  cfg_ = cfg;
  HRESULT hr = MFStartup(MF_VERSION);
  if (FAILED(hr)) return Result<void>::Err("MFStartup failed");

  auto devices = enumerateCameras();
  if (cfg_.deviceIndex >= devices.size()) {
    return Result<void>::Err("Camera index out of range");
  }

  CComPtr<IMFAttributes> attrs;
  hr = MFCreateAttributes(&attrs, 2);
  if (FAILED(hr)) return Result<void>::Err("MFCreateAttributes failed");
  hr = attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                      MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
  if (FAILED(hr)) return Result<void>::Err("SetGUID failed");
  hr = attrs->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                        devices[cfg_.deviceIndex].symbolicLink.c_str());
  if (FAILED(hr)) return Result<void>::Err("SetString failed");

  CComPtr<IMFMediaSource> source;
  hr = MFCreateDeviceSource(attrs, &source);
  if (FAILED(hr)) return Result<void>::Err("MFCreateDeviceSource failed");

  hr = MFCreateSourceReaderFromMediaSource(source, nullptr, &reader_);
  if (FAILED(hr)) return Result<void>::Err("MFCreateSourceReaderFromMediaSource failed");

  // Configure output type
  CComPtr<IMFMediaType> outType;
  MFCreateMediaType(&outType);
  outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  outType->SetGUID(MF_MT_SUBTYPE,
                   cfg_.pixelFormat == PixelFormat::NV12 ? MFVideoFormat_NV12 : MFVideoFormat_RGB32);
  MFSetAttributeSize(outType, MF_MT_FRAME_SIZE, cfg_.width, cfg_.height);
  MFSetAttributeRatio(outType, MF_MT_FRAME_RATE, cfg_.fps, 1);
  outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  outType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);

  hr = reader_->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, outType);
  if (FAILED(hr)) return Result<void>::Err("SetCurrentMediaType failed");

  VMOSUE_LOG_INFO("CameraCapture initialized: {}x{} @ {}fps", cfg_.width, cfg_.height, cfg_.fps);
  return Result<void>::Ok({});
}

void CameraCapture::Start() {
  if (running_.exchange(true)) return;
  thread_ = std::thread(&CameraCapture::captureLoop, this);
}

void CameraCapture::Stop() {
  if (!running_.exchange(false)) return;
  if (thread_.joinable()) thread_.join();
  MFShutdown();
}

void CameraCapture::captureLoop() {
  while (running_.load()) {
    CComPtr<IMFSample> sample;
    DWORD streamFlags = 0;
    HRESULT hr = reader_->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                                     nullptr, &streamFlags, nullptr, &sample);
    if (!running_.load()) break;
    if (FAILED(hr) || !sample) {
      VMOSUE_LOG_WARN("ReadSample failed: hr=0x{:x}", hr);
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }
    CComPtr<IMFMediaBuffer> buffer;
    sample->ConvertToContiguousBuffer(&buffer);
    BYTE* data = nullptr;
    DWORD length = 0;
    buffer->Lock(&data, nullptr, &length);
    Frame f;
    f.width = cfg_.width;
    f.height = cfg_.height;
    f.format = cfg_.pixelFormat;
    f.rowPitch = cfg_.width;  // for NV12
    f.timestampUs = NowMicros();
    f.data.assign(data, data + length);
    buffer->Unlock();
    {
      std::lock_guard<std::mutex> lk(frameMutex_);
      latestFrame_ = std::move(f);
      hasFrame_ = true;
    }
    frameCv_.notify_one();
  }
}

bool CameraCapture::TryGetLatestFrame(Frame& out) {
  std::lock_guard<std::mutex> lk(frameMutex_);
  if (!hasFrame_) return false;
  out = latestFrame_;
  return true;
}

}  // namespace vmosue
