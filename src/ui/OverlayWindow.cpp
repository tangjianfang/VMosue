#include "ui/OverlayWindow.h"
#include "ui/HandSkeleton.h"
#include "ui/OverlayGeometry.h"
#include "util/Logger.h"
#include <d2d1.h>
#include <d2d1helper.h>

namespace vmosue {

static const wchar_t kClassName[] = L"VMosueOverlay";

LRESULT CALLBACK OverlayWindow::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_NCHITTEST) return HTTRANSPARENT;  // click-through
  return DefWindowProc(h, m, w, l);
}

bool OverlayWindow::Init(HWND hwndParent) {
  WNDCLASSEX wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = WndProc;
  wc.hInstance = GetModuleHandle(nullptr);
  wc.lpszClassName = kClassName;
  if (!RegisterClassEx(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    VMOSUE_LOG_ERROR("RegisterClass overlay failed");
    return false;
  }
  virtX_ = GetSystemMetrics(SM_XVIRTUALSCREEN);
  virtY_ = GetSystemMetrics(SM_YVIRTUALSCREEN);
  virtW_ = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  virtH_ = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  hwnd_ = CreateWindowEx(
      WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
      kClassName, L"", WS_POPUP,
      virtX_, virtY_, virtW_, virtH_,
      hwndParent, nullptr,
      GetModuleHandle(nullptr), nullptr);
  if (!hwnd_) return false;
  SetLayeredWindowAttributes(hwnd_, RGB(0, 0, 0), 0, LWA_COLORKEY);
  ShowWindow(hwnd_, SW_SHOW);
  D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2dFactory_);
  RECT rc; GetClientRect(hwnd_, &rc);
  d2dFactory_->CreateHwndRenderTarget(
      D2D1::RenderTargetProperties(),
      D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(rc.right, rc.bottom)),
      &renderTarget_);
  running_.store(true);
  renderT_ = std::thread([this]() {
    while (running_.load()) {
      Render();
      Sleep(16);
    }
  });
  return true;
}

void OverlayWindow::Shutdown() {
  if (!running_.exchange(false)) return;
  if (renderT_.joinable()) renderT_.join();
  if (renderTarget_) { renderTarget_->Release(); renderTarget_ = nullptr; }
  if (d2dFactory_) { d2dFactory_->Release(); d2dFactory_ = nullptr; }
  if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
}

void OverlayWindow::Update(const Feedback& f) {
  std::lock_guard<std::mutex> lk(mu_);
  feedback_ = f;
}

void OverlayWindow::Render() {
  if (!renderTarget_) return;
  Feedback f;
  { std::lock_guard<std::mutex> lk(mu_); f = feedback_; }
  renderTarget_->BeginDraw();
  renderTarget_->Clear(D2D1::ColorF(0, 0, 0, 0));  // transparent

  RECT rc; GetClientRect(hwnd_, &rc);
  float cx = f.cursorX * rc.right, cy = f.cursorY * rc.bottom;
  ID2D1SolidColorBrush* brush = nullptr;
  D2D1_COLOR_F col = (f.paused) ? D2D1::ColorF(0.5f, 0.5f, 0.5f, 0.7f)
                    : (f.confidence > 0.8f ? D2D1::ColorF(0.2f, 1.0f, 0.4f, 0.9f)
                       : (f.confidence > 0.5f ? D2D1::ColorF(1.0f, 1.0f, 0.2f, 0.9f)
                          : D2D1::ColorF(1.0f, 0.2f, 0.2f, 0.9f)));
  renderTarget_->CreateSolidColorBrush(col, &brush);
  float r = 14.0f;
  renderTarget_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), brush, 3.0f);
  if (brush) brush->Release();
  renderTarget_->EndDraw();
}

void OverlayWindow::ResizeRenderTarget() {
  if (renderTarget_) {
    renderTarget_->Release();
    renderTarget_ = nullptr;
  }
  if (!d2dFactory_ || !hwnd_) return;
  RECT rc;
  GetClientRect(hwnd_, &rc);
  HRESULT hr = d2dFactory_->CreateHwndRenderTarget(
      D2D1::RenderTargetProperties(),
      D2D1::HwndRenderTargetProperties(
          hwnd_, D2D1::SizeU(rc.right, rc.bottom)),
      &renderTarget_);
  if (FAILED(hr)) {
    VMOSUE_LOG_WARN("OverlayWindow: render target recreate failed hr=0x{:x}", hr);
  }
}

}  // namespace vmosue
