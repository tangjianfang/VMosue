#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <boost/lockfree/spsc_queue.hpp>
#include <optional>
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
#include "ui/ActionListWindow.h"

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

  // Set by a worker thread when it exits due to an unhandled exception.
  // The main Run() loop polls this flag and triggers a graceful
  // Shutdown() so the app stops and surfaces an error instead of
  // silently freezing with a non-functional cursor. Starts at false;
  // once set it is never cleared (one worker dying is enough to stop).
  std::atomic<bool> threadError_{false};

  // Called from the catch blocks of worker threads. Sets threadError_
  // and initiates an orderly shutdown so the user gets an observable
  // failure rather than a frozen cursor.
  void NotifyThreadError(const char* loopName);

  std::thread captureT_;
  std::thread inferenceT_;
  std::thread smT_;
  // v0.3 (Task 37): background-init thread. Media Foundation
  // camera init (MFCreateDeviceSource + SetCurrentMediaType) can
  // take 1-3 seconds on USB cameras while the driver and capture
  // pipeline enumerate and negotiate. Previously this ran on the
  // main thread BEFORE the GetMessage loop, so the DebugWindow
  // appeared as a white unrendered rectangle with a busy mouse
  // cursor for the entire warmup window. We now do the heavy
  // init in startupT_ and let the main thread enter the message
  // pump immediately after creating the windows.
  std::thread startupT_;

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
  // v0.6: modeless action-list help window. Created in Run() so
  // the F1 hotkey callback and the tray "Action list" menu item
  // can Show() it on demand. Shutdown() tears it down after the
  // tray message window, same pattern as tutorial_.
  std::unique_ptr<ActionListWindow> help_;

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

  // Task 29: DebugWindow receives frames and landmarks via its own
  // SPSC queues (DebugWindow::PushFrame / PushLandmarks). v0.3
  // (Task 36) removed the App-side mirror queues — they were never
  // consumed (DebugWindow's update thread reads from its own
  // queues, not from App's), so the live preview was permanently
  // empty. The capture and inference loops now call
  // `debug_->PushFrame(f)` / `debug_->PushLandmarks(hands)`
  // directly, which is a single SPSC push into the DebugWindow.
  // The DebugWindow's queues are independent of the production
  // path so a slow / hidden debug consumer cannot back-pressure
  // capture or inference. drop-on-full is the right policy: a
  // debug visualization that lags by one frame is fine, a
  // production mouse that lags is not.

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

  // v0.5: previous right-hand landmarks for the per-frame motion
  // delta fed to the adaptive OneEuro filter tuner. We track the
  // wrist (landmark 0) only — the index MCP / other points are
  // correlated so the wrist alone is enough to characterize hand
  // motion magnitude. Reset (cleared) whenever a hand is lost so a
  // "hand re-appearing at a different location" doesn't pollute
  // the deltas with a huge first-frame jump.
  std::optional<HandLandmarks> prevRightHand_;

  // v0.5: wall-clock timestamp of the previous inference tick,
  // used to compute the actual elapsed dt for the OneEuro
  // smoother. The IPC round-trip is non-deterministic (20-80ms
  // in practice), so using a constant `1/inferenceFps` over-
  // or under-shoots the smoother's derivative estimate and
  // causes cursor jitter under variable cadence. Stored as a
  // steady_clock time_point; initialized at construction so the
  // first frame's dt is measured from "now", not from epoch 0.
  std::chrono::steady_clock::time_point last_smooth_ts_{
      std::chrono::steady_clock::now()};
};

}  // namespace vmosue
