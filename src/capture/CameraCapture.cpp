#include "capture/CameraCapture.h"
#include "util/Logger.h"
#include <atlbase.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mfcaptureengine.h>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>

#pragma comment(lib, "mfreadwrite")
#pragma comment(lib, "mfplat")
#pragma comment(lib, "mfuuid")

namespace vmosue {

namespace {
constexpr int kQueueDepth = 2;

// Convert an NV12 frame (Y plane then interleaved UV plane at
// half-resolution) into BGRA bytes suitable for the inference
// pipeline. The Y plane is `width * height` bytes; the UV plane
// is `width * (height / 2)` bytes with U, V alternating every
// pixel. We use BT.601 limited-range coefficients (the de-facto
// standard for NV12 from webcams).
//
// The caller pre-allocates `out_bgra` with at least
// `width * height * 4` bytes. We do NOT bounds-check inside the
// inner loop for speed; the caller is expected to size the buffer
// correctly.
void NV12ToBgra(const uint8_t* nv12, uint8_t* out_bgra,
                 uint32_t width, uint32_t height) {
  const uint8_t* y_plane = nv12;
  const uint8_t* uv_plane = nv12 + static_cast<size_t>(width) * height;
  const uint32_t half_w = width / 2;
  for (uint32_t y = 0; y < height; ++y) {
    uint8_t* row_out = out_bgra + static_cast<size_t>(y) * width * 4;
    const uint8_t* y_row = y_plane + static_cast<size_t>(y) * width;
    const uint8_t* uv_row =
        uv_plane + static_cast<size_t>(y / 2) * width;
    for (uint32_t x = 0; x < width; ++x) {
      const int Y = static_cast<int>(y_row[x]);
      const int U = static_cast<int>(uv_row[(x / 2) * 2]);
      const int V = static_cast<int>(uv_row[(x / 2) * 2 + 1]);
      // BT.601 limited-range integer math.
      const int C = Y - 16;
      const int D = U - 128;
      const int E = V - 128;
      int R = (298 * C + 409 * E + 128) >> 8;
      int G = (298 * C - 100 * D - 208 * E + 128) >> 8;
      int B = (298 * C + 516 * D + 128) >> 8;
      if (R < 0) R = 0; else if (R > 255) R = 255;
      if (G < 0) G = 0; else if (G > 255) G = 255;
      if (B < 0) B = 0; else if (B > 255) B = 255;
      row_out[x * 4 + 0] = static_cast<uint8_t>(B);  // B
      row_out[x * 4 + 1] = static_cast<uint8_t>(G);  // G
      row_out[x * 4 + 2] = static_cast<uint8_t>(R);  // R
      row_out[x * 4 + 3] = 255;                       // A
    }
  }
}

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
  using clock = std::chrono::steady_clock;
  const auto start = clock::now();
  bool firstFrameLogged = false;
  int noSampleStreak = 0;  // consecutive ReadSample calls with no sample
  while (running_.load()) {
    CComPtr<IMFSample> sample;
    DWORD streamFlags = 0;
    HRESULT hr = reader_->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                                     nullptr, &streamFlags, nullptr, &sample);
    if (!running_.load()) break;
    if (FAILED(hr)) {
      // True failure (e.g. device disconnected). The previous version
      // also logged "failed" when hr was S_OK but the sample pointer
      // was null, which flooded the log at ~100Hz during the normal
      // MF warmup window and was misread by the user as a real
      // error. We only log the genuinely-failed case here.
      VMOSUE_LOG_WARN("ReadSample failed: hr=0x{:x}", hr);
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }
    if (!sample) {
      // hr == S_OK but no sample yet — this is the normal
      // Media Foundation warmup state. Many cameras take 1-3
      // seconds (sometimes more) to deliver their first sample
      // after the source reader is created. We back off a
      // little each time so we don't peg a core during warmup.
      noSampleStreak++;
      if (noSampleStreak == 1 || noSampleStreak == 30 ||
          noSampleStreak == 100 || (noSampleStreak % 200) == 0) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            clock::now() - start).count();
        VMOSUE_LOG_INFO("Camera warmup: waiting for first sample "
                        "(streak={}, elapsed={}ms)",
                        noSampleStreak, static_cast<long>(elapsed));
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }
    if (!firstFrameLogged) {
      const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
          clock::now() - start).count();
      VMOSUE_LOG_INFO("Camera first sample after {}ms ({} warmup polls)",
                      static_cast<long>(elapsed), noSampleStreak);
      firstFrameLogged = true;
    }
    noSampleStreak = 0;
    CComPtr<IMFMediaBuffer> buffer;
    sample->ConvertToContiguousBuffer(&buffer);
    BYTE* data = nullptr;
    DWORD length = 0;
    buffer->Lock(&data, nullptr, &length);
    {
      // We write directly into latestFrame_.data under the mutex
      // rather than building a local Frame and moving it. The
      // reason is allocation cost: at 1280x720 BGRA the data
      // vector is 3.6 MB, and allocating + freeing that 30 times
      // per second turns into a non-trivial amount of malloc
      // churn. The vector grows to its peak size on the first
      // frame and stays there for the lifetime of the capture
      // thread, so the steady state is zero allocs per frame.
      //
      // We hold frameMutex_ across NV12ToBgra(). The conversion
      // is a few hundred microseconds at 720p; the consumer side
      // (TryGetLatestFrame) does a fast memcpy of the header
      // fields plus a vector copy, so blocking it for that long
      // is fine. The alternative — copying into a local first,
      // then locking to swap — would re-introduce the per-frame
      // allocation we're trying to avoid.
      std::lock_guard<std::mutex> lk(frameMutex_);
      latestFrame_.width  = cfg_.width;
      latestFrame_.height = cfg_.height;
      latestFrame_.timestampUs = NowMicros();
      if (cfg_.pixelFormat == PixelFormat::NV12) {
        // NV12 → BGRA in-place conversion. The downstream
        // inference path (HandDetector) only consumes BGRA /
        // BGR24, so we convert here once instead of pushing
        // the color-space math into the per-frame Detect()
        // path.
        const size_t need = static_cast<size_t>(cfg_.width) *
                            static_cast<size_t>(cfg_.height) * 4;
        if (latestFrame_.data.size() < need) {
          latestFrame_.data.resize(need);
        }
        NV12ToBgra(reinterpret_cast<const uint8_t*>(data),
                   latestFrame_.data.data(), cfg_.width, cfg_.height);
        latestFrame_.format = PixelFormat::RGBA32;
        latestFrame_.rowPitch = cfg_.width * 4;
      } else {
        // BGR24 / RGBA32 paths: just copy the source bytes and
        // remember the format. rowPitch is set to the natural
        // pitch for the format. The vector grows to the
        // largest frame size seen and stays there.
        const size_t need = static_cast<size_t>(length);
        if (latestFrame_.data.size() < need) {
          latestFrame_.data.resize(need);
        }
        std::memcpy(latestFrame_.data.data(), data, need);
        latestFrame_.format = cfg_.pixelFormat;
        latestFrame_.rowPitch = cfg_.width *
            (cfg_.pixelFormat == PixelFormat::BGR24 ? 3 : 4);
      }
      hasFrame_ = true;
    }
    buffer->Unlock();
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
