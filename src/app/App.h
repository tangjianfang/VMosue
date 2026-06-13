#pragma once
#include <atomic>
#include <thread>
#include <vector>
#include <boost/lockfree/spsc_queue.hpp>
#include "capture/CameraCapture.h"
#include "capture/Frame.h"
#include "inference/HandDetector.h"
#include "inference/LandmarkSmoother.h"
#include "gesture/GestureStateMachine.h"
#include "ui/OverlayWindow.h"
#include "ui/TrayIcon.h"

namespace vmosue {

// App wires together the three worker threads that turn camera frames
// into OS-level input events:
//
//   T1 captureLoop        : CameraCapture -> frameQ_
//   T2 inferenceLoop      : frameQ_ -> HandDetector -> LandmarkSmoother -> landmarkQ_
//   T3 stateMachineLoop   : landmarkQ_ -> GestureStateMachine -> InputInjector
//
// T1 -> T2 and T2 -> T3 communicate via lock-free single-producer /
// single-consumer ring buffers (boost::lockfree::spsc_queue). All
// cross-thread teardown is driven by a single atomic `running_` flag
// checked at the top of each loop iteration; Shutdown() flips the flag
// and joins the threads.
class App {
 public:
  App();
  ~App();

  // Initialize camera + detector + state machine, spawn the three
  // worker threads, and block (sleeping on the running_ flag) until
  // Shutdown() is called. Returns a non-zero status if any subsystem
  // fails to initialize.
  int Run();

  // Idempotent: flips `running_` to false and joins all worker
  // threads, then asks the InputInjector to release any held buttons.
  // Safe to call multiple times and from the destructor.
  void Shutdown();

 private:
  void captureLoop();
  void inferenceLoop();
  void stateMachineLoop();

  std::atomic<bool> running_{false};
  std::thread captureT_;
  std::thread inferenceT_;
  std::thread smT_;

  CameraCapture cam_;
  HandDetector detector_;
  LandmarkSmoother smoother_;
  GestureStateMachine sm_;
  OverlayWindow overlay_;
  TrayIcon tray_;

  // Task 26: a hidden message-only window that owns the tray icon
  // callbacks. TrayIcon::Shutdown() does NOT destroy this window;
  // it's owned by App and torn down in Shutdown() AFTER the tray
  // icon has been removed. The main thread pumps its message queue
  // while waiting for Shutdown().
  HWND trayMsgWnd_ = nullptr;
  static LRESULT CALLBACK TrayMsgWndProc(HWND, UINT, WPARAM, LPARAM);

  // SPSC queue between T1 (producer) and T2 (consumer). Capacity 2
  // gives one slot of slack without forcing the producer to block.
  boost::lockfree::spsc_queue<Frame, boost::lockfree::capacity<2>> frameQ_;

  // SPSC queue between T2 (producer) and T3 (consumer). Capacity 4
  // lets the state machine absorb a short inference stall without
  // dropping frames.
  boost::lockfree::spsc_queue<std::vector<HandLandmarks>,
                              boost::lockfree::capacity<4>>
      landmarkQ_;
};

}  // namespace vmosue
