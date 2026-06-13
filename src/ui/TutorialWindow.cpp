#include "ui/TutorialWindow.h"

#include <windows.h>

#include <string>

namespace vmosue {

TutorialWindow::~TutorialWindow() { Shutdown(); }

bool TutorialWindow::Init(HWND parent) {
  parent_ = parent;
  initialized_ = true;
  // v0.2 stub: no native window yet. See Task 30.
  return true;
}

void TutorialWindow::ShowStep(int current, int total, const std::wstring& message) {
  if (!initialized_) return;
  // Format a one-line header so the user knows how far through the
  // flow they are. Kept on a single line because MessageBox titles
  // cannot wrap.
  std::wstring header = L"Step " + std::to_wstring(current) + L"/" +
                        std::to_wstring(total);
  // MB_TOPMOST so the indicator is visible even if the user's focus
  // is on a fullscreen app; the calibration flow needs to be seen.
  MessageBoxW(parent_, message.c_str(), header.c_str(),
              MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
}

void TutorialWindow::Shutdown() {
  initialized_ = false;
  parent_ = nullptr;
}

}  // namespace vmosue
