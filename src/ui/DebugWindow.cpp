#include "ui/DebugWindow.h"

#include "capture/Frame.h"
#include "config/Config.h"
#include "inference/HandDetector.h"
#include "util/FrameConvert.h"
#include "util/Logger.h"

#include <d2d1helper.h>

#include <algorithm>
#include <cstdio>
#include <sstream>
#include <string>

namespace vmosue {

namespace {

// Window class name chosen to avoid colliding with OverlayWindow
// ("VMosueOverlay"), SettingsWindow ("VMosueSettingsWindow"), or
// TutorialWindow ("VMosueTutorial").
static const wchar_t kClassName[] = L"VMosueDebugWindow";
static DebugWindow* g_debug = nullptr;

// BGR24 -> BGRA color constant. The D2D_BITMAP we create for the
// camera preview uses DXGI_FORMAT_B8G8R8A8_UNORM (the canonical
// HWND-render-target-compatible format), so we have to expand
// BGR24 / NV12 pixels to BGRA at upload time. The actual conversion
// math lives in src/util/FrameConvert.{h,cpp} (so unit tests can
// link it without d2d1); this file only does the format dispatch
// and the D2D1 upload. Alpha channel = 255 (opaque).
constexpr uint8_t kOpaqueAlpha = 0xFF;

// Generic dispatch. Returns the BGRA pixel buffer for any format
// the DebugWindow knows how to render. The format label returned
// via `labelOut` is shown above the preview so the user can tell
// at a glance whether they're looking at the real camera or a
// placeholder. CameraCapture's current default path delivers
// RGBA32 (BGRA bytes in memory) after NV12->BGRA conversion in
// the capture loop; BGR24 is supported as a fallback so a future
// config change to the camera format still renders without a
// code edit. The raw NV12 path is also kept because offline
// tools or future camera drivers might feed us NV12 directly
// (e.g. a recorded stream).
std::vector<uint8_t> FrameToBgra(const Frame& f, std::wstring& labelOut) {
  if (f.format == PixelFormat::RGBA32) {
    auto buf = Rgba32FrameToBgra(f);
    if (!buf.empty()) { labelOut = L"Camera: RGBA32 live"; return buf; }
  }
  if (f.format == PixelFormat::NV12) {
    auto buf = Nv12FrameToBgra(f);
    if (!buf.empty()) { labelOut = L"Camera: NV12 live"; return buf; }
  }
  if (f.format == PixelFormat::BGR24) {
    auto buf = Bgr24FrameToBgra(f);
    if (!buf.empty()) { labelOut = L"Camera: BGR24 live"; return buf; }
  }
  labelOut = L"";
  return {};
}

// Format a `double` to two decimals in a wide string. Used for the
// FPS counters and other numeric fields.
std::wstring Fmt2(double v) {
  wchar_t buf[32];
  swprintf_s(buf, L"%.2f", v);
  return buf;
}

// Format a `double` to one decimal.
std::wstring Fmt1(double v) {
  wchar_t buf[32];
  swprintf_s(buf, L"%.1f", v);
  return buf;
}

// Render a single line of text at (x, y) with the given D2D brush
// color. The render target must already be in the BeginDraw/EndDraw
// block. We use the D2D-GDI interop path: QueryInterface the render
// target for ID2D1GdiInteropRenderTarget, GetDC(), set the text
// color, call TextOutW, and ReleaseDC(). This avoids the full
// IDWriteFactory + IDWriteTextFormat stack but still gives us
// crisp system-font rendering at any size.
//
// The D2D_GDI_COMPATIBLE render-target property (set on creation)
// is required for GetDC() to work. We assume the render target is
// GDI-compatible because the OverlayWindow pattern in Task 15 uses
// D2D1::RenderTargetProperties() defaults which include that flag.
void DrawLine(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush,
              float x, float y, const std::wstring& s) {
  if (!rt || !brush || s.empty()) return;
  ID2D1GdiInteropRenderTarget* interop = nullptr;
  // Use the templated QueryInterface(Q**) overload so the IID is
  // derived from the destination type — this matches the SDK's
  // expected signature (REFIID), which is reference-to-IID, not
  // pointer-to-IID.
  if (FAILED(rt->QueryInterface(&interop))) {
    return;
  }
  HDC hdc = nullptr;
  if (FAILED(interop->GetDC(D2D1_DC_INITIALIZE_MODE_COPY, &hdc))) {
    interop->Release();
    return;
  }
  if (hdc) {
    const D2D1_COLOR_F& c = brush->GetColor();
    // Convert sRGB float to 8-bit GDI colorref. GDI uses 0x00BBGGRR
    // ordering for the COLORREF; D2D uses straight alpha RGB.
    SetTextColor(hdc, RGB(
        static_cast<int>(c.r * 255.0f),
        static_cast<int>(c.g * 255.0f),
        static_cast<int>(c.b * 255.0f)));
    SetBkMode(hdc, TRANSPARENT);
    TextOutW(hdc,
             static_cast<int>(x),
             static_cast<int>(y),
             s.c_str(),
             static_cast<int>(s.size()));
    interop->ReleaseDC(nullptr);
  }
  interop->Release();
}

}  // namespace

DebugWindow::DebugWindow() = default;

DebugWindow::~DebugWindow() {
  Shutdown();
}

HWND DebugWindow::Create(HWND hwndParent) {
  if (hwnd_) return hwnd_;

  HINSTANCE hinst = GetModuleHandle(nullptr);

  WNDCLASSEX wc{};
  wc.cbSize        = sizeof(wc);
  wc.style         = 0;
  wc.lpfnWndProc   = &DebugWindow::WndProc;
  wc.hInstance     = hinst;
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
  wc.lpszClassName = kClassName;
  if (!RegisterClassEx(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    VMOSUE_LOG_ERROR("DebugWindow: RegisterClassEx failed");
    return nullptr;
  }

  g_debug = this;

  // Standard top-level window. NOT WS_VISIBLE — the caller decides
  // when to Show(). We pass `this` as lpCreateParams so WndProc can
  // recover the instance via GWLP_USERDATA on WM_NCCREATE.
  DWORD style = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  hwnd_ = CreateWindowEx(
      WS_EX_TOOLWINDOW,  // skip the taskbar entry; this is a dev tool
      kClassName,
      L"VMosue Debug",
      style,
      CW_USEDEFAULT, CW_USEDEFAULT,
      kWindowW, kWindowH,
      hwndParent, nullptr, hinst, this);
  if (!hwnd_) {
    VMOSUE_LOG_ERROR("DebugWindow: CreateWindowEx failed");
    g_debug = nullptr;
    return nullptr;
  }

  // ---- D2D init ----
  D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2dFactory_);
  if (d2dFactory_) {
    RECT rc; GetClientRect(hwnd_, &rc);
    d2dFactory_->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(hwnd_,
                                         D2D1::SizeU(rc.right, rc.bottom)),
        &renderTarget_);
    if (renderTarget_) {
      // Pre-create the colored brushes we use most often. Drawing
      // per-frame CreateSolidColorBrush() calls would leak the
      // previous brush (the render target's CreateSolidColorBrush
      // returns a *new* IUnknown every call). We create each brush
      // once at init and Release() them in Shutdown().
      renderTarget_->CreateSolidColorBrush(
          D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &brushWhite_);
      renderTarget_->CreateSolidColorBrush(
          D2D1::ColorF(0.2f, 1.0f, 0.4f, 1.0f), &brushGreen_);
      renderTarget_->CreateSolidColorBrush(
          D2D1::ColorF(1.0f, 0.9f, 0.2f, 1.0f), &brushYellow_);
      renderTarget_->CreateSolidColorBrush(
          D2D1::ColorF(1.0f, 0.2f, 0.2f, 1.0f), &brushRed_);
      renderTarget_->CreateSolidColorBrush(
          D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f), &brushBlack_);
    }
  }
  return hwnd_;
}

void DebugWindow::Show() {
  if (!hwnd_) return;
  ShowWindow(hwnd_, SW_SHOW);
  SetForegroundWindow(hwnd_);

  // Start the update thread on the first Show(). The thread is
  // restarted by Hide()/Show() cycles — we just leave it running
  // and rely on the visible-vs-hidden state to gate work. This
  // keeps startup latency low on subsequent opens (no thread
  // spawn cost). The thread checks hwnd_'s IsWindowVisible() at
  // the top of each tick; if hidden it sleeps an extra 100ms
  // before re-checking.
  if (!running_.exchange(true)) {
    updateT_ = std::thread([this]() { updateLoop(); });
  }
}

void DebugWindow::Hide() {
  if (!hwnd_) return;
  ShowWindow(hwnd_, SW_HIDE);
}

void DebugWindow::Shutdown() {
  // Idempotent: first call joins everything, subsequent calls no-op.
  if (!running_.exchange(false)) return;
  if (updateT_.joinable()) updateT_.join();

  if (brushWhite_)  { brushWhite_->Release();  brushWhite_  = nullptr; }
  if (brushGreen_)  { brushGreen_->Release();  brushGreen_  = nullptr; }
  if (brushYellow_) { brushYellow_->Release(); brushYellow_ = nullptr; }
  if (brushRed_)    { brushRed_->Release();    brushRed_    = nullptr; }
  if (brushBlack_)  { brushBlack_->Release();  brushBlack_  = nullptr; }
  if (renderTarget_) { renderTarget_->Release(); renderTarget_ = nullptr; }
  if (d2dFactory_)   { d2dFactory_->Release();   d2dFactory_   = nullptr; }
  if (hwnd_) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
  if (g_debug == this) g_debug = nullptr;
}

void DebugWindow::PushFrame(const Frame& f) {
  frameQ_.push(f);
}

void DebugWindow::PushLandmarks(std::vector<HandLandmarks> hands) {
  landmarkQ_.push(std::move(hands));
}

void DebugWindow::PushLog(const std::wstring& text) {
  DebugLogLine line;
  line.timestampUs = NowMicros();
  line.text = text;
  std::lock_guard<std::mutex> lk(snapshotMu_);
  appendLogLineLocked(std::move(line));
}

void DebugWindow::PushFps(double capture, double inference, double sm) {
  std::lock_guard<std::mutex> lk(snapshotMu_);
  snapshot_.captureFps      = capture;
  snapshot_.inferenceFps    = inference;
  snapshot_.stateMachineFps = sm;
}

void DebugWindow::PushState(int stateValue) {
  std::lock_guard<std::mutex> lk(snapshotMu_);
  snapshot_.stateValue = stateValue;
}

void DebugWindow::appendLogLineLocked(DebugLogLine line) {
  snapshot_.log.push_back(std::move(line));
  // Trim from the front if we've exceeded the cap. vector::erase
  // at the front is O(n) but n is bounded by kMaxLogLines (100) so
  // the cost is negligible.
  if (snapshot_.log.size() > kMaxLogLines) {
    size_t excess = snapshot_.log.size() - kMaxLogLines;
    snapshot_.log.erase(snapshot_.log.begin(),
                        snapshot_.log.begin() + excess);
  }
}

void DebugWindow::updateLoop() {
  using clock = std::chrono::steady_clock;
  const auto period = std::chrono::milliseconds(1000 / kUpdateHz);
  while (running_.load()) {
    auto tick = clock::now();

    // Drain all available frames / landmarks into the snapshot. We
    // only keep the *latest* of each (the queues are SPSC; the
    // producer pushes every frame, so by the time we get here the
    // freshest entry is at the back). Other entries are dropped.
    {
      Frame f;
      Frame latest;
      bool haveLatest = false;
      while (frameQ_.pop(f)) {
        latest = f;
        haveLatest = true;
      }
      if (haveLatest) {
        std::lock_guard<std::mutex> lk(snapshotMu_);
        snapshot_.frame = std::move(latest);
        snapshot_.frameFresh = true;
      }
    }
    {
      std::vector<HandLandmarks> hands;
      std::vector<HandLandmarks> latest;
      bool haveLatest = false;
      while (landmarkQ_.pop(hands)) {
        latest = std::move(hands);
        haveLatest = true;
      }
      if (haveLatest) {
        std::lock_guard<std::mutex> lk(snapshotMu_);
        snapshot_.landmarks = std::move(latest);
      }
    }

    // Schedule a repaint. We only invalidate when the window is
    // visible so a hidden window doesn't accumulate a flood of
    // pending WM_PAINTs.
    if (hwnd_ && IsWindowVisible(hwnd_)) {
      InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // Sleep for the remainder of the 100ms period. The
    // renderTarget_/brush* are touched only by the WndProc (on the
    // main thread), so there's no D2D threading concern.
    auto elapsed = clock::now() - tick;
    if (elapsed < period) {
      std::this_thread::sleep_for(period - elapsed);
    }
  }
}

void DebugWindow::render() {
  if (!renderTarget_ || !hwnd_) return;

  // Snapshot under the mutex so the update thread can't tear the
  // data while we're drawing.
  DebugSnapshot snap;
  {
    std::lock_guard<std::mutex> lk(snapshotMu_);
    snap = snapshot_;
  }

  renderTarget_->BeginDraw();
  renderTarget_->Clear(D2D1::ColorF(0.08f, 0.08f, 0.10f, 1.0f));

  // ---- Camera preview (top-left quadrant) ----
  // The preview area is 400x300, anchored at (10, 10). We draw a
  // dark backing rectangle first so the placeholder text is
  // readable on top of the system window background.
  const float previewX = 10.0f;
  const float previewY = 10.0f;
  // DrawEllipse on a rectangle would be wrong; use DrawRectangle
  // for the backing box. The stub D2D doesn't have DrawRectangle
  // (it only has DrawEllipse), so we emulate a rectangle with
  // four side fills would be overkill; for v0.2 we just clear the
  // preview area to a slightly darker shade via Clear() and skip
  // the backing rect. The dark window clear above is fine.

  // Render the camera frame, if any. v0.2 strategy (spec issue
  // #6): the camera frame is converted to BGRA and drawn as a
  // placeholder label ("Camera: WxH") because the parse-check stub
  // does not expose ID2D1RenderTarget::CreateBitmap / DrawBitmap.
  // In a production build with the real D2D1 SDK headers, the
  // BGRA buffer is uploaded via CreateBitmap and the bitmap is
  // blitted into the preview area. For v0.2 the placeholder
  // suffices to verify the pipeline is wired.
  if (!snap.frame.empty() && snap.frame.width > 0 &&
      snap.frame.height > 0 && brushWhite_) {
    if (snap.frame.format == PixelFormat::BGR24 ||
        snap.frame.format == PixelFormat::NV12 ||
        snap.frame.format == PixelFormat::RGBA32) {
      // v0.3: actually blit the camera frame. CameraCapture feeds
      // us NV12 by default; the BGR24 branch is a fallback for a
      // future config change. We convert to BGRA, upload to a
      // D2D1 bitmap, then DrawBitmap scales it into the preview
      // rectangle. The bitmap is recreated each tick (10Hz) so we
      // don't need a cache or size-change detector — the cost of
      // ~900KB / tick at 640x360 is well within a debug window's
      // budget.
      std::wstring label;
      std::vector<uint8_t> bgra = FrameToBgra(snap.frame, label);
      bool drew = false;
      if (!bgra.empty() && renderTarget_) {
        D2D1_BITMAP_PROPERTIES props{};
        props.pixelFormat = D2D1::PixelFormat(
            DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE);
        props.dpiX = 96.0f;
        props.dpiY = 96.0f;
        ID2D1Bitmap* bmp = nullptr;
        HRESULT hr = renderTarget_->CreateBitmap(
            D2D1::SizeU(snap.frame.width, snap.frame.height),
            bgra.data(),
            static_cast<UINT32>(snap.frame.width) * 4u,
            props, &bmp);
        if (SUCCEEDED(hr) && bmp) {
          D2D1_RECT_F dest = D2D1::RectF(
              previewX, previewY,
              previewX + static_cast<float>(kPreviewW),
              previewY + static_cast<float>(kPreviewH));
          renderTarget_->DrawBitmap(
              bmp, dest, 1.0f,
              D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
              D2D1::RectF(0, 0,
                          static_cast<float>(snap.frame.width),
                          static_cast<float>(snap.frame.height)));
          bmp->Release();
          drew = true;
        }
      }
      if (!drew) {
        std::wstring info = L"Camera: ";
        info += std::to_wstring(snap.frame.width);
        info += L"x";
        info += std::to_wstring(snap.frame.height);
        info += L" (preview blit failed)";
        DrawLine(renderTarget_, brushWhite_,
                 previewX + 8.0f, previewY + 8.0f, info);
      } else if (!label.empty()) {
        DrawLine(renderTarget_, brushWhite_,
                 previewX + 6.0f, previewY + 6.0f, label);
      }
    } else {
      // Truly unsupported format (e.g. RGBA32). Label it so the
      // user can still tell the pipeline is alive.
      std::wstring info = L"Camera: ";
      info += std::to_wstring(snap.frame.width);
      info += L"x";
      info += std::to_wstring(snap.frame.height);
      info += L" (format unsupported)";
      DrawLine(renderTarget_, brushWhite_,
               previewX + 8.0f, previewY + 8.0f, info);
    }
  } else if (brushWhite_) {
    // The camera hasn't delivered a frame yet. This is the normal
    // state during the Media Foundation warmup window (1-3s on a
    // fast camera, longer on slow USB cams). Show an explicit
    // "Waiting for camera..." message with an elapsed-seconds
    // counter so the user can tell the app is alive — without
    // this the static text looks like a frozen window.
    if (waitingSince_.time_since_epoch().count() == 0) {
      waitingSince_ = std::chrono::steady_clock::now();
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - waitingSince_).count();
    std::wstring info = L"Waiting for camera... (";
    info += std::to_wstring(static_cast<long>(elapsed));
    info += L"s)";
    DrawLine(renderTarget_, brushYellow_,  // yellow so it stands out
             previewX + 8.0f, previewY + 8.0f, info);
    // Add a hint line below the title so the user understands
    // why the preview is blank. The Media Foundation
    // SourceReader can take a few seconds to deliver its first
    // sample; this is not a hang.
    DrawLine(renderTarget_, brushWhite_,
             previewX + 8.0f, previewY + 28.0f,
             L"First camera frame can take 1-3 seconds");
  }
  if (hasShownFirstFrame_ == false &&
      !snap.frame.empty() && snap.frame.width > 0) {
    hasShownFirstFrame_ = true;
  }

  // ---- Landmark overlay (drawn on top of the preview) ----
  // For each hand, draw 21 small filled circles at the normalized
  // (x, y) positions, mapped to the 400x300 preview area. We don't
  // have a D2D DrawFilledEllipse in the stub, so we draw the
  // outline only (DrawEllipse) — the user can still see the dots.
  if (brushGreen_) {
    for (const auto& h : snap.landmarks) {
      for (int i = 0; i < HandLandmarks::kNumPoints; ++i) {
        float nx = std::clamp(h.points[i].x, 0.0f, 1.0f);
        float ny = std::clamp(h.points[i].y, 0.0f, 1.0f);
        float px = previewX + nx * static_cast<float>(kPreviewW);
        float py = previewY + ny * static_cast<float>(kPreviewH);
        // Right hand = green, left hand = yellow (color by handedness).
        ID2D1SolidColorBrush* b = (h.handedness == 1)
                                       ? brushGreen_
                                       : brushYellow_;
        renderTarget_->DrawEllipse(
            D2D1::Ellipse(D2D1::Point2F(px, py), 3.0f, 3.0f),
            b, 1.5f);
      }
    }
  }

  // ---- Right-hand side text panel (state, FPS, config, log) ----
  // panelX is just past the right edge of the preview rectangle
  // (previewX=10 + kPreviewW=640 + 20px gutter).
  const float panelX = 670.0f;
  float y = 12.0f;

  // Header: state machine state.
  std::wstring stateStr;
  ID2D1SolidColorBrush* stateBrush = brushWhite_;
  switch (snap.stateValue) {
    case 0: stateStr = L"State: ACTIVE";         stateBrush = brushGreen_;  break;
    case 1: stateStr = L"State: PAUSED";         stateBrush = brushYellow_; break;
    case 2: stateStr = L"State: EMERGENCY";      stateBrush = brushRed_;    break;
    default: stateStr = L"State: (unknown)";
  }
  DrawLine(renderTarget_, stateBrush, panelX, y, stateStr);
  y += 24.0f;

  // FPS lines.
  DrawLine(renderTarget_, brushWhite_, panelX, y,
           L"Capture FPS:        " + Fmt2(snap.captureFps));
  y += 18.0f;
  DrawLine(renderTarget_, brushWhite_, panelX, y,
           L"Inference FPS:      " + Fmt2(snap.inferenceFps));
  y += 18.0f;
  DrawLine(renderTarget_, brushWhite_, panelX, y,
           L"StateMachine FPS:   " + Fmt2(snap.stateMachineFps));
  y += 24.0f;

  // Landmark count.
  int numHands = static_cast<int>(snap.landmarks.size());
  DrawLine(renderTarget_, brushWhite_, panelX, y,
           L"Hands detected: " + std::to_wstring(numHands));
  y += 24.0f;

  // ---- Config dump ----
  DrawLine(renderTarget_, brushYellow_, panelX, y, L"-- Config --");
  y += 18.0f;
  const auto& cfg = Config::Get().Data();
  DrawLine(renderTarget_, brushWhite_, panelX, y,
           L"Profile:        " + std::wstring(cfg.activeProfile.begin(),
                                              cfg.activeProfile.end()));
  y += 18.0f;
  DrawLine(renderTarget_, brushWhite_, panelX, y,
           L"Camera index:   " + std::to_wstring(cfg.cameraIndex));
  y += 18.0f;
  std::wstring perfMode(cfg.performanceMode.begin(),
                        cfg.performanceMode.end());
  DrawLine(renderTarget_, brushWhite_, panelX, y,
           L"Perf mode:      " + perfMode);
  y += 18.0f;
  // v0.5 (Wave 4): the per-user `sensitivity` field is gone. Show
  // a one-line "Sensitivity: adaptive" marker so the debug panel
  // makes the change visible; the actual current value is
  // available in the Settings window's live readouts.
  DrawLine(renderTarget_, brushWhite_, panelX, y,
           L"Sensitivity:    adaptive (auto)");
  y += 18.0f;
  std::wstring logLevel(cfg.logLevel.begin(), cfg.logLevel.end());
  DrawLine(renderTarget_, brushWhite_, panelX, y,
           L"Log level:      " + logLevel);
  y += 18.0f;
  DrawLine(renderTarget_, brushWhite_, panelX, y,
           L"Auto start:     " +
               std::wstring(cfg.autoStart ? L"on" : L"off"));
  y += 24.0f;

  // ---- Action log (last N lines) ----
  DrawLine(renderTarget_, brushYellow_, panelX, y, L"-- Action log --");
  y += 18.0f;
  // Compute the height budget for the log. We want to fit as many
  // of the most-recent lines as possible within the remaining
  // window height.
  const float bottomMargin = 12.0f;
  const float lineHeight = 16.0f;
  float logBudget = (kWindowH - bottomMargin) - y;
  if (logBudget < 0) logBudget = 0;
  size_t maxLines = static_cast<size_t>(logBudget / lineHeight);
  if (maxLines > snap.log.size()) maxLines = snap.log.size();
  size_t start = snap.log.size() - maxLines;
  for (size_t i = start; i < snap.log.size(); ++i) {
    DrawLine(renderTarget_, brushWhite_, panelX, y,
             snap.log[i].text);
    y += lineHeight;
  }

  renderTarget_->EndDraw();

  // Clear the frame-fresh flag so consumers (none in v0.2, but
  // future code) can tell whether a new frame was actually drawn.
  {
    std::lock_guard<std::mutex> lk(snapshotMu_);
    snapshot_.frameFresh = false;
  }
}

LRESULT CALLBACK DebugWindow::WndProc(HWND hwnd, UINT msg,
                                      WPARAM w, LPARAM l) {
  // Capture the instance pointer on WM_NCCREATE (the only place
  // lpCreateParams is reliably forwarded by all SDK flavors).
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                     reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    return 1;
  }
  DebugWindow* self = reinterpret_cast<DebugWindow*>(
      GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (!self) return DefWindowProc(hwnd, msg, w, l);

  switch (msg) {
    case WM_PAINT:
      // Standard paint begin/end + render through D2D. We don't
      // call BeginPaint/EndPaint because Direct2D owns the
      // render target for the window; mixing the two (GDI
      // BeginPaint into the same DC) is unsupported.
      self->render();
      return 0;

    case WM_ERASEBKGND:
      // D2D renders the full background; suppress GDI erase to
      // avoid flicker on resize.
      return 1;

    case WM_CLOSE:
      // Hide rather than destroy so reopening is instant.
      ShowWindow(hwnd, SW_HIDE);
      return 0;

    case WM_DESTROY:
      // Window is being torn down (e.g. App shutdown). Clear
      // the hwnd_ pointer so a stale Show()/Hide() is a no-op.
      self->hwnd_ = nullptr;
      return 0;

    default:
      return DefWindowProc(hwnd, msg, w, l);
  }
}

}  // namespace vmosue
