#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <boost/lockfree/spsc_queue.hpp>
#include "capture/CameraCapture.h"
#include "capture/Frame.h"
#include "inference/HandDetector.h"
#include "inference/LandmarkSmoother.h"
#include "gesture/GestureStateMachine.h"
#include "ui/DebugWindow.h"
#include "ui/OverlayWindow.h"
#include "ui/SettingsWindow.h"
#include "ui/TrayIcon.h"
#include "ui/TutorialWindow.h"

namespace vmosue {

// Task 28: performance mode descriptor. The App's capture and
// inference loops read CurrentPerfMode() on every iteration so a
// user changing the dropdown at runtime takes effect on the next
// frame; no thread signaling required. Values map to the strings
// stored in AppConfig::performanceMode ("battery", "balanced",
// "performance"); unknown strings fall back to balanced.
struct PerfMode {
  int  inferenceFps;  // Hz — cap on HandDetector::Detect() rate
  int  captureFps;    // Hz — cap on CameraCapture::TryGetLatestFrame poll rate
  bool useGpu;        // forwarded to HandDetector::SetUseGpu()
};

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

  // Task 28: returns the PerfMode corresponding to
  // Config::Get().Data().performanceMode. Unknown / unset values
  // fall back to "balanced". Pure function; safe to call from
  // worker threads (reads Config under the Config singleton's
  // internal mutex via Data()).
  static PerfMode CurrentPerfMode();

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
  // Task 27: modeless Settings dialog. Created in Run(), shown via
  // the tray icon's "Settings" menu item, hidden (not destroyed) in
  // Shutdown so reopening is instant. unique_ptr owns the lifetime.
  std::unique_ptr<SettingsWindow> settings_;
  // Task 29: modeless Debug window. Created in Run() (so the tray
  // menu callback can Show() it on demand), hidden + joined in
  // Shutdown(). Lifetime: app process. The window itself owns its
  // D2D resources and 10Hz update thread; the App just wires the
  // tray menu and the producer side of the debug queues.
  std::unique_ptr<DebugWindow> debug_;
  // Task 30: modeless 6-step tutorial window. Created in Run() so
  // the tray menu callback can Show() it on demand. If the user
  // has AppConfig::showTutorialOnLaunch set, the App also auto-
  // shows the window ~3 seconds after the main UI has appeared
  // (see App::Run()). Shutdown() tears it down after the tray
  // message window so the parent's DestroyWindow can't pull the
  // child out from under it.
  std::unique_ptr<TutorialWindow> tutorial_;

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

  // Task 29: mirror queues feeding the DebugWindow. The capture
  // loop pushes the same Frame it pushes to frameQ_ onto
  // debugFrameQ_; the inference loop pushes the same smoothed
  // hand set it pushes to landmarkQ_ onto debugLandmarkQ_. The
  // DebugWindow's update thread is the sole consumer of each. We
  // keep these queues independent of the main pipeline so a slow
  // debug consumer (or a hidden debug window) cannot back-pressure
  // the production path. drop-on-full is the right policy here:
  // a debug visualization that lags by one frame is fine, a
  // production mouse that lags is not.
  boost::lockfree::spsc_queue<Frame, boost::lockfree::capacity<2>>
      debugFrameQ_;
  boost::lockfree::spsc_queue<std::vector<HandLandmarks>,
                              boost::lockfree::capacity<4>>
      debugLandmarkQ_;

  // Task 33: shared, lock-free current capture rate. The inference
  // loop downshifts to kIdleFps after `kIdleDownshiftMs` of no
  // detected hands and upshifts back to the perf-mode rate on any
  // detection. The capture loop reads this value once per tick
  // (it's an atomic int, not a mutex-protected variable) so the
  // throttle updates without any cross-thread signaling.
  std::atomic<int> currentFps_{30};

  // Task 33: timestamp of the most recent successful hand
  // detection. Monotonic steady_clock milliseconds. Initialized to
  // "now" at construction so we don't downshift immediately after
  // startup before the first frame has had a chance to come back
  // from the detector.
  std::atomic<int64_t> lastDetectionMs_{
      static_cast<int64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now().time_since_epoch())
              .count())};

  // Constants for the idle down-shift. kIdleFps is well below the
  // 30Hz default so the capture + inference CPU cost is cut ~3x
  // during the long stretches where the user isn't actively using
  // the gesture mouse. kIdleDownshiftMs is the dead-band: 5s of
  // no detection before we downshift, hysteresis 0 (any detection
  // brings us back up).
  static constexpr int    kIdleFps          = 10;
  static constexpr int64_t kIdleDownshiftMs  = 5000;
};

}  // namespace vmosue
