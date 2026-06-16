#include "ui/OverlayWindow.h"
#include "ui/HandSkeleton.h"
#include "ui/OverlayGeometry.h"
#include "util/Logger.h"
#include <d2d1.h>
#include <d2d1helper.h>

namespace vmosue {

static const wchar_t kClassName[] = L"VMosueOverlay";

LRESULT CALLBACK OverlayWindow::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_NCCREATE) {
    // Thread the OverlayWindow* (passed via lpCreateParams in
    // CreateWindowEx) into GWLP_USERDATA so later messages can
    // recover it without a global.
    auto* cs = reinterpret_cast<CREATESTRUCT*>(l);
    SetWindowLongPtrW(h, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    return DefWindowProc(h, m, w, l);
  }
  if (m == WM_NCHITTEST) return HTTRANSPARENT;  // click-through
  if (m == WM_DISPLAYCHANGE) {
    // Monitor configuration changed (plug, unplug, or resolution
    // change). Re-query the four virtual-desktop metrics, resize
    // the window to the new virtual desktop, and ask the render
    // thread to recreate the D2D render target at the next
    // iteration.
    OverlayWindow* self = reinterpret_cast<OverlayWindow*>(
        GetWindowLongPtrW(h, GWLP_USERDATA));
    if (self) {
      self->virtX_ = GetSystemMetrics(SM_XVIRTUALSCREEN);
      self->virtY_ = GetSystemMetrics(SM_YVIRTUALSCREEN);
      self->virtW_ = GetSystemMetrics(SM_CXVIRTUALSCREEN);
      self->virtH_ = GetSystemMetrics(SM_CYVIRTUALSCREEN);
      SetWindowPos(h, HWND_TOP,
                   self->virtX_, self->virtY_,
                   self->virtW_, self->virtH_,
                   SWP_NOACTIVATE | SWP_NOZORDER);
      self->needsResize_.store(true);
    }
    return 0;
  }
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
      GetModuleHandle(nullptr), this);
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
  if (needsResize_.exchange(false)) {
    ResizeRenderTarget();
  }
  if (!renderTarget_) return;

  Feedback f;
  { std::lock_guard<std::mutex> lk(mu_); f = feedback_; }

  renderTarget_->BeginDraw();
  renderTarget_->Clear(D2D1::ColorF(0, 0, 0, 0));  // transparent

  if (f.hasHand) {
    // Pick a color based on the v0.2 confidence tier. The
    // palette is preserved from the v0.2 debug ring so users
    // familiar with that color scheme get the same feedback
    // signals: green=confident, yellow=marginal, red=poor,
    // gray=paused.
    D2D1_COLOR_F col;
    if (f.paused) {
      col = D2D1::ColorF(0.5f, 0.5f, 0.5f, 0.7f);
    } else if (f.confidence > 0.8f) {
      col = D2D1::ColorF(0.2f, 1.0f, 0.4f, 0.9f);
    } else if (f.confidence > 0.5f) {
      col = D2D1::ColorF(1.0f, 1.0f, 0.2f, 0.9f);
    } else {
      col = D2D1::ColorF(1.0f, 0.2f, 0.2f, 0.9f);
    }

    ID2D1SolidColorBrush* brush = nullptr;
    renderTarget_->CreateSolidColorBrush(col, &brush);
    if (brush) {
      // Map each landmark to a virtual-desktop pixel.
      D2D1_POINT_2F pts[21];
      for (int i = 0; i < 21; ++i) {
        auto sp = LandmarkToScreen(f.landmarks[i],
                                   virtX_, virtY_, virtW_, virtH_);
        pts[i] = D2D1::Point2F(sp.x, sp.y);
      }

      // Bones first (under the joints), then the joint dots on
      // top so the dots are visible at every joint.
      for (const auto& bone : kHandBones) {
        renderTarget_->DrawLine(pts[bone.first], pts[bone.second],
                                brush, 3.0f);
      }
      const float dotRadius = 5.0f;
      for (int i = 0; i < 21; ++i) {
        renderTarget_->DrawEllipse(
            D2D1::Ellipse(pts[i], dotRadius, dotRadius),
            brush, 2.0f);
      }
      brush->Release();
    }
  }
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
