#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d2d1.h>
#include <atomic>
#include <mutex>
#include <thread>

namespace vmosue {

struct Feedback {
  float cursorX = 0.5f, cursorY = 0.5f;  // normalized
  float confidence = 0.0f;  // 0..1
  int leftHandCount = 0, rightHandCount = 0;
  bool paused = false;
  bool lastClickLeft = false, lastClickRight = false;
  uint64_t lastClickTimeUs = 0;
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
};

}  // namespace vmosue
