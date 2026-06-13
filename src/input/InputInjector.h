#pragma once
// InputInjector - abstracts OS-level input synthesis (mouse, keyboard).
//
// This file is a STUB introduced in Task 9 (CursorController) so that the
// gesture module has a stable, compileable dependency on the input module.
// The real implementation (SendInput wrappers, virtual-key synthesis, etc.)
// lands in Task 12. CursorController.cpp currently calls into this API but
// the call site is commented out until Task 12 wires it up.
//
// API shape is deliberately minimal: a static Get() singleton plus a small
// set of methods. Do not extend this surface without a plan update.
namespace vmosue {

class InputInjector {
 public:
  // Returns the process-wide injector instance. Safe to call before Init();
  // methods that need a real backend will no-op or return false in that case.
  static InputInjector& Get();

  // Move the system cursor by (dx, dy) pixels relative to its current
  // position. dx>0 is right, dy>0 is down (screen coordinates).
  void MoveCursor(int dx, int dy);

  // Press / release the primary (left) mouse button.
  void MouseDown();
  void MouseUp();

  // Press / release a virtual-key code (VK_LBUTTON, VK_RBUTTON, ...).
  void KeyDown(int vk);
  void KeyUp(int vk);

 private:
  InputInjector() = default;
};

}  // namespace vmosue
