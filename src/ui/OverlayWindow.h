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

  // Release the existing D2D render target (if any) and re-create
  // it sized to the current window client area. Called from the
  // render thread after a needsResize_ flip.
  void ResizeRenderTarget();
};

}  // namespace vmosue