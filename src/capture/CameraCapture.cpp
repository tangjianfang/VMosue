#include "capture/CameraCapture.h"
#include "util/Logger.h"
#include <condition_variable>
#include <mutex>

namespace vmosue {

CameraCapture::CameraCapture() = default;
CameraCapture::~CameraCapture() { Stop(); }

Result<void> CameraCapture::Init(const Config&) { return Result<void>::Ok({}); }
void CameraCapture::Start() {}
void CameraCapture::Stop() {}
bool CameraCapture::TryGetLatestFrame(Frame&) { return false; }
void CameraCapture::captureLoop() {}

}  // namespace vmosue
