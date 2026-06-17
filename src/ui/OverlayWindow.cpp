#include "ui/OverlayWindow.h"
#include "ui/HandSkeleton.h"
#include "ui/OverlayGeometry.h"
#include "util/Adaptive.h"
#include "util/I18n.h"
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
      GetSignalObserver().RecordVirtualDesktop(self->virtX_, self->virtY_,
                                               self->virtW_, self->virtH_);
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
  // v0.5: cache the virtual-desktop size in the adaptive observer
  // so CursorController and any other "screen-size dependent"
  // tunable can read it via GetAdaptive().DesktopPixels(). The
  // previous hard-coded 1920x1080 made cursor motion wrong on 4K
  // and multi-monitor setups. We also cache the origin (virtX,
  // virtY) so multi-monitor rigs where the primary is not at
  // (0, 0) still get correct SetCursorPos targets.
  GetSignalObserver().RecordVirtualDesktop(virtX_, virtY_, virtW_, virtH_);
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
    // v0.5 perf: instead of polling on Sleep, wait on a
    // condition variable that Update() signals. The wait_for
    // timeout still caps the maximum render rate (== the
    // adaptive RenderSleepMs) so we don't paint faster than
    // the camera produces frames. Without the CV, a hand motion
    // arriving just after a Sleep could wait up to one full
    // RenderSleepMs before the next render -- with the CV, the
    // wait returns as soon as Update() signals (microseconds).
    std::unique_lock<std::mutex> lk(renderMu_);
    while (running_.load()) {
      auto t0 = std::chrono::steady_clock::now();
      lk.unlock();
      Render();
      lk.lock();
      auto dt = std::chrono::steady_clock::now() - t0;
      GetSignalObserver().RecordRenderDuration(dt);
      int sleepMs = static_cast<int>(GetAdaptive().RenderSleepMs());
      // The predicate is checked before the wait and on every
      // notify, so a dirty_ set by Update() between renders
      // wakes us immediately. The timeout keeps the adaptive
      // rate cap in effect.
      renderCv_.wait_for(lk, std::chrono::milliseconds(sleepMs),
                         [this] { return !running_.load() || dirty_; });
      // The wait consumed the dirty flag; clear it so the next
      // wait can block until the next Update().
      dirty_ = false;
    }
  });
  return true;
}

void OverlayWindow::Shutdown() {
  if (!running_.exchange(false)) return;
  // v0.5 perf: signal the render thread to wake and see
  // running_=false. Without the notify, a render thread
  // sleeping in wait_for would block until the next timeout
  // (up to RenderSleepMs = 16-100ms), delaying shutdown.
  renderCv_.notify_all();
  if (renderT_.joinable()) renderT_.join();
  ReleaseBrushes();
  if (renderTarget_) { renderTarget_->Release(); renderTarget_ = nullptr; }
  if (d2dFactory_) { d2dFactory_->Release(); d2dFactory_ = nullptr; }
  if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
}

void OverlayWindow::Update(const Feedback& f) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    feedback_ = f;
  }
  // Set the dirty flag and wake the render thread. Holding
  // renderMu_ around the flag set keeps the order of "set
  // feedback" -> "set dirty" preserved -- the render thread
  // won't wake up and see dirty_=true with a stale feedback_.
  {
    std::lock_guard<std::mutex> lk(renderMu_);
    dirty_ = true;
  }
  renderCv_.notify_one();
}

void OverlayWindow::CreateBrushes() {
  // All six brushes are created up-front so Render() never has
  // to hit CreateSolidColorBrush on the hot path. Colors match
  // the previous per-frame values: gray for paused, green /
  // yellow / red for the three confidence tiers, plus a bright
  // yellow "Pending" for the dwell-preview bar and a white
  // "Text" brush for the "About to:" label.
  if (!renderTarget_) return;
  const D2D1_COLOR_F colors[] = {
      D2D1::ColorF(0.5f, 0.5f, 0.5f, 0.7f),  // Paused
      D2D1::ColorF(0.2f, 1.0f, 0.4f, 0.9f),  // Green
      D2D1::ColorF(1.0f, 1.0f, 0.2f, 0.9f),  // Yellow
      D2D1::ColorF(1.0f, 0.2f, 0.2f, 0.9f),  // Red
      D2D1::ColorF(1.0f, 0.85f, 0.1f, 0.95f),// Pending (v0.6)
      D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f),// Text (v0.6)
  };
  for (int i = 0; i < static_cast<int>(BrushTier::Count); ++i) {
    if (brushes_[i]) continue;
    ID2D1SolidColorBrush* b = nullptr;
    HRESULT hr = renderTarget_->CreateSolidColorBrush(colors[i], &b);
    if (FAILED(hr) || !b) {
      VMOSUE_LOG_WARN("OverlayWindow: CreateSolidColorBrush[{}] failed "
                      "hr=0x{:x}", i, hr);
      continue;
    }
    brushes_[i] = b;
  }
}

void OverlayWindow::ReleaseBrushes() {
  for (auto& b : brushes_) {
    if (b) { b->Release(); b = nullptr; }
  }
}

void OverlayWindow::Render() {
  if (needsResize_.exchange(false)) {
    ResizeRenderTarget();
  }
  if (!renderTarget_) return;
  // D2D brushes are bound to the render target that created
  // them. ResizeRenderTarget() rebuilds the target, so any
  // brushes we held become invalid; rebuild them here on the
  // render thread. After the first valid frame, brushes_ stays
  // populated until the next resize.
  if (!brushes_[0]) {
    CreateBrushes();
  }

  Feedback f;
  { std::lock_guard<std::mutex> lk(mu_); f = feedback_; }

  renderTarget_->BeginDraw();
  renderTarget_->Clear(D2D1::ColorF(0, 0, 0, 0));  // transparent

  if (f.hasHand) {
    // v0.5: feed the per-frame confidence into the rolling window
    // so the color-tier cutoffs adapt to the observed distribution.
    GetSignalObserver().RecordConfidence(f.confidence);

    // Color tiers are percentile-adaptive: green = c > mu+sigma,
    // yellow = mu-sigma < c <= mu+sigma, red = c < mu-sigma. Cold-
    // start falls back to (0.8, 0.5). The tier is just an index
    // into the brush cache; no allocation, no Release per frame.
    auto cutoffs = GetAdaptive().ConfidenceCutoffs();
    BrushTier tier;
    if (f.paused) {
      tier = BrushTier::Paused;
    } else if (f.confidence > cutoffs.first) {
      tier = BrushTier::Green;
    } else if (f.confidence > cutoffs.second) {
      tier = BrushTier::Yellow;
    } else {
      tier = BrushTier::Red;
    }
    ID2D1SolidColorBrush* brush = brushes_[static_cast<int>(tier)];

    // v0.5: bone width and dot radius scale with virtual-desktop
    // width so the skeleton stays visible at 4K without
    // overwhelming a 1080p display.
    auto stroke = GetAdaptive().OverlayStrokeAndDot(virtW_);
    const float boneWidth = stroke.first;
    const float dotRadius = stroke.second;

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
                                brush, boneWidth);
      }
      for (int i = 0; i < 21; ++i) {
        renderTarget_->DrawEllipse(
            D2D1::Ellipse(pts[i], dotRadius, dotRadius),
            brush, 2.0f);
      }
    }
    // v0.6.2: render the settle-in countdown. We do this
    // BEFORE the dwell preview so the two never overlap on
    // screen (the dwell countdown only appears once the
    // user is past the grace window, but if both somehow
    // happen on the same frame the dwell preview takes
    // visual priority).
    DrawSettlingCountdown(renderTarget_, f);
    // v0.6: render the dwell-preview label + progress bar above
    // the wrist joint. The user wanted to know what command is
    // about to fire — this is the affordance. Drawn after the
    // skeleton so it sits on top.
    DrawDwellPreview(renderTarget_, f);
  }
  renderTarget_->EndDraw();
}

void OverlayWindow::ResizeRenderTarget() {
  // Brushes are bound to the OLD render target. Release them
  // before the old target goes away so they don't leak.
  ReleaseBrushes();
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

// Map a DwellGate::Kind to an i18n key. The keys live in
// resources/i18n/{en,zh}.json; the lookup falls back to the key
// itself if a translation is missing, so a partially-translated
// build still shows something readable.
static const char* DwellActionI18nKey(int actionId) {
  switch (actionId) {
    case 1: return "action.leftClick";
    case 2: return "action.rightClick";
    case 3: return "action.middleClick";
    case 4: return "action.doubleClick";
    default: return "";
  }
}

// Render a single line of text via the D2D-GDI interop path.
// Identical to DebugWindow::DrawLine; duplicated here so
// OverlayWindow.cpp does not need to drag in the debug window
// headers. v0.6.2: optional `fontHeight` (negative, as GDI
// convention dictates) — the dwell preview wants a much larger
// font than the wrist-anchored debug overlay used.
static void DrawOverlayText(ID2D1RenderTarget* rt,
                            ID2D1SolidColorBrush* brush,
                            float x, float y, const std::wstring& s,
                            int fontHeight = -18) {
  if (!rt || !brush || s.empty()) return;
  ID2D1GdiInteropRenderTarget* interop = nullptr;
  if (FAILED(rt->QueryInterface(&interop))) return;
  HDC hdc = nullptr;
  if (FAILED(interop->GetDC(D2D1_DC_INITIALIZE_MODE_COPY, &hdc))) {
    interop->Release();
    return;
  }
  if (hdc) {
    const D2D1_COLOR_F& c = brush->GetColor();
    SetTextColor(hdc, RGB(
        static_cast<int>(c.r * 255.0f),
        static_cast<int>(c.g * 255.0f),
        static_cast<int>(c.b * 255.0f)));
    SetBkMode(hdc, TRANSPARENT);
    // v0.6.2: pick a heavier font size for the dwell preview.
    // We use a per-call HFONT (created and destroyed here) rather
    // than caching one in the OverlayWindow because the font size
    // is now a parameter and GDI font handles are not safe to
    // share across threads. The cost is a CreateFontW + DeleteObject
    // per frame, both ~microsecond-scale, well under our render
    // budget.
    HFONT font = CreateFontW(fontHeight, 0, 0, 0, FW_BOLD, FALSE, FALSE,
                             FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HGDIOBJ prev = SelectObject(hdc, font);
    TextOutW(hdc,
             static_cast<int>(x),
             static_cast<int>(y),
             s.c_str(),
             static_cast<int>(s.size()));
    SelectObject(hdc, prev);
    DeleteObject(font);
    interop->ReleaseDC(nullptr);
  }
  interop->Release();
}

void OverlayWindow::DrawDwellPreview(ID2D1RenderTarget* rt,
                                     const Feedback& f) {
  // v0.6: dwell-preview. Caller (Render) has already gated on
  // f.hasHand, so the preview only shows when the hand is
  // visible. We anchor the label to the wrist (landmark 0) —
  // fingertips move too much during a pinch to anchor
  // reliably.
  if (!f.dwellActive) return;
  if (!rt) return;

  const char* key = DwellActionI18nKey(f.dwellActionId);
  if (!key || !*key) return;

  // Compose the localized label. The "preview.aboutTo" i18n key
  // resolves to "About to: {action}" or "即将执行：{action}". We
  // support a {action} placeholder, falling back to a plain
  // append if the value doesn't have one (future-proofing).
  std::wstring label = I18n::Get().TW("preview.aboutTo");
  std::wstring actionName = I18n::Get().TW(key);
  if (actionName.empty()) actionName = L"action";

  std::wstring text;
  if (label.find(L"{action}") != std::wstring::npos) {
    size_t pos = 0;
    while ((pos = label.find(L"{action}", pos)) != std::wstring::npos) {
      label.replace(pos, 8, actionName);
      pos += actionName.size();
    }
    text = label;
  } else {
    text = label + L" " + actionName;
  }

  // Append the remaining-time countdown.
  wchar_t buf[64];
  float sec = f.dwellRemainingMs / 1000.0f;
  if (sec < 0.0f) sec = 0.0f;
  swprintf_s(buf, L"  %.1fs", sec);
  text += buf;

  // v0.6.3: explicit cancel hint. The user reported "随便一动
  // 它就瞎乱点" — their anxiety was that any sustained pinch
  // could fire. Showing "(release to cancel)" right under the
  // countdown makes the abort path obvious: pull the fingers
  // apart (or take the hand out of frame) and the click won't
  // fire. We draw this in a smaller white font on the line
  // BELOW the main countdown so the eye reads the big yellow
  // number first, then the "you can still back out" hint.
  std::wstring cancelHint = I18n::Get().TW("preview.releaseToCancel");

  // v0.6.1: anchor the preview to the TOP-CENTER of the virtual
  // desktop, not the wrist. The wrist-anchored layout from the
  // initial v0.6 implementation was hard to see — the user is
  // looking at the screen, not their hand. A fixed top-center
  // position is always in the user's eye-line and large enough
  // to read at a glance.
  //
  // v0.6.2: scaled up further. The 360x6 layout was still easy
  // to miss when the user was focused on something else on
  // screen. Now: 520x14 progress bar, 36pt bold Segoe UI text,
  // positioned 80px from the top of the virtual desktop. The
  // final 200ms flashes a bright white border to make the
  // imminent fire unmissable.
  const float barW = 520.0f;
  const float barH = 14.0f;
  float textX = static_cast<float>(virtX_) +
                (static_cast<float>(virtW_) - barW) / 2.0f;
  float textY = static_cast<float>(virtY_) + 80.0f;

  ID2D1SolidColorBrush* textBrush =
      brushes_[static_cast<int>(BrushTier::Text)];
  // 36pt bold — large enough to read at 1080p from a chair.
  DrawOverlayText(rt, textBrush, textX, textY, text, /*fontHeight=*/-36);

  // v0.6.3: render the cancel hint just below the main text
  // (smaller, white). Sits BETWEEN the text and the progress
  // bar so the visual hierarchy is: "what's about to happen"
  // (yellow, big) → "you can still cancel" (white, small) →
  // "how close to firing" (progress bar). We skip drawing if
  // i18n returned empty (defensive against missing key) so the
  // preview never shows a stale "Lorem ipsum" placeholder.
  if (!cancelHint.empty()) {
    DrawOverlayText(rt, textBrush, textX, textY + 32.0f, cancelHint,
                    /*fontHeight=*/-16);
  }

  ID2D1SolidColorBrush* pending =
      brushes_[static_cast<int>(BrushTier::Pending)];
  if (!pending) return;

  // Bar is 44px below the text baseline (text is 36pt ≈ 48px tall).
  float barX = textX;
  float barY = textY + 56.0f;

  // v0.6.2: final-200ms flash. When the action is about to
  // commit (dwellRemainingMs <= 200), the bar gets a 4px white
  // border that pulses each frame. This is the
  // "un-missable" affordance: even if the user is looking at
  // something else, peripheral vision picks up the flashing
  // border and gives them a chance to release before the
  // action fires. The wall-clock-derived ms count is the
  // canonical signal here; we deliberately do NOT use
  // f.dwellProgress (which is float) for the time check
  // because 1.0 - progress is not the same as remaining/total
  // when the adaptive controller blends dwell values.
  bool flashing = f.dwellRemainingMs <= 200.0f;

  D2D1_RECT_F bg = D2D1::RectF(barX, barY, barX + barW, barY + barH);
  rt->DrawRectangle(bg, pending, 2.0f);
  if (flashing) {
    // Bright white flash — 4px border that extends outside the
    // bar's normal stroke. We draw it as a slightly inset
    // rectangle so it sits on top of the yellow border. We
    // reuse the Text brush (already white) rather than
    // creating a per-flash brush, since D2D requires an
    // ID2D1Brush* for DrawRectangle (no D2D1_COLOR_F overload).
    D2D1_RECT_F flash = D2D1::RectF(barX - 4.0f, barY - 4.0f,
                                    barX + barW + 4.0f, barY + barH + 4.0f);
    rt->DrawRectangle(flash, textBrush, 4.0f);
  }

  float progress = f.dwellProgress;
  if (progress < 0.0f) progress = 0.0f;
  if (progress > 1.0f) progress = 1.0f;
  if (progress > 0.0f) {
    D2D1_RECT_F fill = D2D1::RectF(barX, barY,
                                  barX + barW * progress,
                                  barY + barH);
    rt->FillRectangle(fill, pending);
  }
}

// v0.6.2: render the "Calibrating... 1.2s" countdown while the
// settle-in grace gate is open, and a brief "✓ Ready" flash when
// it ends. Layout: same top-center anchor as the dwell preview,
// but ~30px above it so the two never overlap. The flash uses
// the Text brush (white) for the checkmark and the Pending
// brush (yellow) for the trailing "Ready" word.
void OverlayWindow::DrawSettlingCountdown(ID2D1RenderTarget* rt,
                                          const Feedback& f) {
  if (!rt) return;
  // Settling is gated by `settlingActive`; the "just ended"
  // flash is gated by `settlingJustEnded`. Both are false in
  // the steady-state so this helper is a no-op in the common
  // case (zero per-frame cost on the hot path).
  if (!f.settlingActive && !f.settlingJustEnded) return;
  if (f.settlingActive && f.settlingTotalMs <= 0) return;

  const float barW = 520.0f;
  float textX = static_cast<float>(virtX_) +
                (static_cast<float>(virtW_) - barW) / 2.0f;
  // Sit ~30px above the dwell preview (which is at virtY + 80).
  // This way both can be visible at once: settling on the
  // upper line, dwell countdown on the lower line.
  float textY = static_cast<float>(virtY_) + 24.0f;

  ID2D1SolidColorBrush* textBrush =
      brushes_[static_cast<int>(BrushTier::Text)];
  if (!textBrush) return;

  std::wstring text;
  if (f.settlingActive) {
    // "Calibrating... 1.2s" — built directly to avoid an
    // i18n lookup on the hot path. The text is short enough
    // that hardcoding is fine; if a translation is later
    // needed we can add a "settling.countdown" key.
    int tenths = (f.settlingRemainingMs + 50) / 100;
    if (tenths < 0) tenths = 0;
    int wholeSec = tenths / 10;
    int frac = tenths % 10;
    wchar_t buf[64];
    swprintf_s(buf, L"Calibrating... %d.%ds  (waiting for stable hand)",
               wholeSec, frac);
    text = buf;
  } else {
    // settlingJustEnded: brief "✓ Ready" flash. We don't
    // gate this by a duration — the caller is expected to
    // clear settlingJustEnded after ~one frame. If the user
    // misses it, the next dwell countdown will appear in
    // the same screen region so they'll know the system is
    // live.
    text = L"Ready";
  }
  DrawOverlayText(rt, textBrush, textX, textY, text, /*fontHeight=*/-28);
}

}  // namespace vmosue
