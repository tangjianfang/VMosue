#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d2d1.h>
#include <array>
#include <atomic>
#include <mutex>
#include <thread>

#include "inference/HandDetector.h"  // Point2F
#include "ui/HandSkeleton.h"          // (transitively: kHandBones)

namespace vmosue {

struct Feedback {
  float cursorX = 0.5f, cursorY = 0.5f;        // normalized (legacy, unused by v0.4 render)
  float confidence = 0.0f;                    // 0..1
  int leftHandCount = 0, rightHandCount = 0;  // legacy
  bool paused = false;
  bool lastClickLeft = false, lastClickRight = false;
  uint64_t lastClickTimeUs = 0;
  // v0.4: dominant-hand skeleton, rendered by OverlayWindow::Render.
  // hasHand=false means "no right hand detected" — the overlay is
  // blank. When true, landmarks are 21 normalized [0,1] points
  // (post-smoothing) and the render maps them 1:1 to virtual-desktop
  // pixels.
  std::array<Point2F, 21> landmarks{};
  bool hasHand = false;
};

class OverlayWindow {
 public:
  bool Init(HWND hwndParent);
  void Shutdown();
  void Update(const Feedback& f);
  void Render();

 private:
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  HWND hwnd_ = nullptr;
  std::atomic<bool> running_{false};
  std::thread renderT_;
  std::mutex mu_;
  Feedback feedback_;
  ID2D1Factory* d2dFactory_ = nullptr;
  ID2D1HwndRenderTarget* renderTarget_ = nullptr;

  // Cached SM_*VIRTUALSCREEN values, refreshed on Init and on
  // WM_DISPLAYCHANGE. Used by Render to map landmarks to pixels.
  int virtX_ = 0, virtY_ = 0, virtW_ = 0, virtH_ = 0;

  // Set by WM_DISPLAYCHANGE; the render thread sees the flag at
  // the top of Render and calls ResizeRenderTarget before drawing.
  std::atomic<bool> needsResize_{false};

  // Render thread wakes up on Update() instead of polling on
  // Sleep(). The condition variable saves up to one full
  // RenderSleepMs of input latency: a hand motion that arrives
  // just after a Sleep call used to wait for the next tick to
  // be drawn; now it can wake the render thread within
  // microseconds. We still cap the maximum render rate via the
  // wait_for timeout (== RenderSleepMs), so the adaptive
  // throttle continues to work.
  std::condition_variable renderCv_;
  std::mutex renderMu_;
  // Set by Update() to tell the render thread "a new frame is
  // waiting". Coalesced — multiple updates between renders set
  // it to true once, so the render thread only re-renders the
  // latest feedback. Guarded by renderMu_.
  bool dirty_ = false;

  // Release the existing D2D render target (if any) and re-create
  // it sized to the current window client area. Called from the
  // render thread after a needsResize_ flip.
  void ResizeRenderTarget();

  // Cached solid-color brushes for the four hand-tier colors
  // (paused, green, yellow, red). D2D brushes are bound to the
  // render target that created them, so they must be released
  // and re-created when ResizeRenderTarget() rebuilds the
  // target. The cache turns 4 CreateSolidColorBrush + 4 Release
  // calls per frame into zero in the common case (the color
  // tier is stable across consecutive frames because confidence
  // only changes when the hand does).
  enum class BrushTier : int {
    Paused = 0,
    Green,
    Yellow,
    Red,
    Count,
  };
  std::array<ID2D1SolidColorBrush*, static_cast<int>(BrushTier::Count)>
      brushes_{};
  void CreateBrushes();
  void ReleaseBrushes();
};

}  // namespace vmosue