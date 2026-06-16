#include "app/App.h"

#include "app/Watchdog.h"
#include "config/Config.h"
#include "input/InputInjector.h"
#include "platform/Hotkey.h"
#include "util/FrameResampler.h"
#include "util/Logger.h"
#include "util/ProfileGuard.h"
#include "ui/TrayIcon.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <string>
#include <thread>
#include <unordered_map>

namespace vmosue {

namespace {
// Static window class registration for the tray message-only window.
// Done in the .cpp TU so the class name and WndProc are local. The
// class is registered once (the first time App::Run is called) and
// left registered for the process lifetime — App is a singleton in
// practice.
const wchar_t* const kTrayMsgClass = L"VMosueTrayMessageWindow";
bool g_trayClassRegistered = false;

// Task 28: PerfMode lookup keyed on AppConfig::performanceMode.
// Strings match the spec table:
//   battery    -> 15/15 fps, GPU off (laptop on battery)
//   balanced   -> 30/30 fps, GPU on  (default)
//   performance-> 30/60 fps, GPU on  (capture at 60, infer at 30)
//
// The map is a static local so it pays the construction cost
// exactly once on first use. Strings are ASCII so we can
// case-insensitive compare without locale lookups.
const std::unordered_map<std::string, PerfMode>& PerfModeTable() {
  static const std::unordered_map<std::string, PerfMode> kTable = {
      {"battery",     {15, 15, false}},
      {"balanced",    {30, 30, true}},
      {"performance", {30, 60, true}},
  };
  return kTable;
}

// ASCII-lowercase helper for the case-insensitive match below.
std::string AsciiLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) {
                   return (c >= 'A' && c <= 'Z')
                              ? static_cast<char>(c - 'A' + 'a')
                              : static_cast<char>(c);
                 });
  return s;
}

// Sleep the calling thread for the remainder of the period that a
// fixed-rate loop with the given `fps` should occupy, given that
// `elapsed` time has already passed since the last tick.
//
// For example, with `fps=30` and `elapsed=10ms` the function sleeps
// ~23ms so the next tick lands at 33ms after the last. We clamp
// `period_us` to >= 1ms to avoid a division-by-zero / busy spin if
// `fps` is ever set to zero.
//
// `elapsed_us` is taken as an int64 microsecond count so callers can
// pass std::chrono::microseconds values without losing precision.
void SleepForFps(int fps, int64_t elapsed_us) {
  if (fps <= 0) return;
  const int64_t period_us = 1000000 / fps;
  const int64_t remaining = period_us - elapsed_us;
  if (remaining > 0) {
    std::this_thread::sleep_for(std::chrono::microseconds(remaining));
  }
}
}  // namespace

PerfMode App::CurrentPerfMode() {
  const auto& cfg = Config::Get().Data();
  const auto& table = PerfModeTable();
  const std::string key = AsciiLower(cfg.performanceMode);
  auto it = table.find(key);
  if (it != table.end()) return it->second;
  // Unknown / empty / mis-spelled -> default to balanced (the
  // AppConfig in-class default is also "balanced"). This matches
  // the SettingsWindow's fallback behavior on a corrupt config.
  return PerfMode{30, 30, true};
}

LRESULT CALLBACK App::TrayMsgWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  // Forward to TrayIcon's WndProc, which knows how to translate the
  // tray callback message and menu commands into App-level callbacks.
  return TrayIcon::WndProc(h, m, w, l);
}

App::App() = default;

App::~App() { Shutdown(); }

int App::Run() {
  // Mark running BEFORE the first thread spawn so the workers never
  // observe a torn state. exchange() in Shutdown() is what actually
  // tears us down.
  running_.store(true);

  // Task 28: read the user's camera + perf-mode preferences from
  // Config before initializing the camera and detector. The
  // performance mode's useGpu flag is forwarded to the HandDetector
  // (currently a stored-only flag; the GPU delegate is not wired up
  // in v0.2). The captureFps is passed to CameraCapture as the
  // requested media-type frame rate; the camera driver may grant
  // less if it can't sustain it. Camera selection is by
  // deviceIndex — no live switching in v0.2, so the choice takes
  // effect on next startup.
  const PerfMode initialPerf = CurrentPerfMode();
  const auto& initialCfg = Config::Get().Data();

  // HandDetector is a stub in this build (no MediaPipe linked),
  // so its init is essentially free; we do it on the main thread
  // for symmetry with v0.2. The moment it becomes a real model
  // load, move it to startupT_ alongside cam_.Init().
  HandDetector::Config hcfg;
  hcfg.useGpu = initialPerf.useGpu;
  hcfg.inferenceWidth = 640;
  hcfg.inferenceHeight = 480;
  if (!detector_.Init(hcfg).isOk()) {
    VMOSUE_LOG_ERROR("HandDetector init failed");
    return 1;
  }
  currentFps_.store(std::max(1, initialPerf.captureFps),
                    std::memory_order_relaxed);
  VMOSUE_LOG_INFO("PerfMode initial: infer={}fps capture={}fps gpu={} "
                  "inferRes={}x{}",
                  initialPerf.inferenceFps, initialPerf.captureFps,
                  initialPerf.useGpu ? "on" : "off",
                  hcfg.inferenceWidth, hcfg.inferenceHeight);

  sm_.Init({});

  if (!overlay_.Init(nullptr)) VMOSUE_LOG_WARN("Overlay init failed");

  // Task 26: register the message-only window class once and create
  // the hidden HWND that will receive tray icon callbacks. We do the
  // class registration here (not in TrayIcon::Init) because the class
  // name and WndProc live in this TU — TrayIcon::WndProc forwards
  // through here.
  if (!g_trayClassRegistered) {
    WNDCLASSEX wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &App::TrayMsgWndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = kTrayMsgClass;
    if (RegisterClassEx(&wc) ||
        GetLastError() == ERROR_CLASS_ALREADY_EXISTS) {
      g_trayClassRegistered = true;
    } else {
      VMOSUE_LOG_WARN("Failed to register tray message window class");
    }
  }
  trayMsgWnd_ = CreateWindowEx(
      WS_EX_TOOLWINDOW, kTrayMsgClass, L"", 0, 0, 0, 0, 0,
      HWND_MESSAGE, nullptr, GetModuleHandle(nullptr), nullptr);
  if (!trayMsgWnd_) {
    VMOSUE_LOG_WARN("Failed to create tray message window");
  } else {
    TrayIcon::MenuCallbacks cb{};
    // Pause/Resume is the only callback with real behavior in v0.2:
    // it forwards to the GestureStateMachine's Pause/Resume API so
    // the overlay indicator reflects the new state. The other menu
    // items wire to windows that haven't been built yet (Tasks 27,
    // 29, 30) — we install no-op lambdas that log the request.
    cb.onTogglePause = [this]() {
      auto s = sm_.State();
      if (s == GlobalState::Paused) sm_.Resume(); else sm_.Pause();
      VMOSUE_LOG_INFO("Tray: toggle pause -> {}", (int)sm_.State());
    };
    cb.onOpenSettings = [this]() {
      if (settings_) settings_->Show();
      else           VMOSUE_LOG_WARN("Tray: Settings unavailable");
    };
    cb.onOpenDebug    = [this]() {
      if (debug_) {
        // Log a one-shot "debug opened" line so the action log in
        // the debug window itself shows when the user clicked the
        // tray item (only visible if they re-open the window).
        debug_->PushLog(L"Tray: Debug window opened");
        debug_->Show();
      } else {
        VMOSUE_LOG_WARN("Tray: Debug unavailable");
      }
    };
    cb.onOpenTutorial = [this]()  {
      if (tutorial_) tutorial_->Show();
      else           VMOSUE_LOG_WARN("Tray: Tutorial unavailable");
    };
    cb.onExit         = [this]() { VMOSUE_LOG_INFO("Tray: Exit requested"); Shutdown(); };
    if (!tray_.Init(trayMsgWnd_, cb)) {
      VMOSUE_LOG_WARN("Tray icon init failed");
    }
  }

  // Task 27: build the settings window modelessly. It is parented
  // to the tray message window (which is hidden anyway) so the
  // settings dialog also has no visible parent. The window is
  // created in the hidden state; the tray's onOpenSettings callback
  // (installed above) flips it visible. We construct it AFTER the
  // trayMsgWnd_ exists because the SettingsWindow needs an HWND
  // parent at Create() time. Failure to create is non-fatal: the
  // tray's callback checks for a null unique_ptr and logs.
  settings_ = std::make_unique<SettingsWindow>();
  if (!settings_->Create(trayMsgWnd_)) {
    VMOSUE_LOG_WARN("SettingsWindow init failed");
    settings_.reset();
  }

  // Task 29: build the debug window. Same lifetime model as the
  // settings window: parented to the tray message window, created
  // in the hidden state, shown on demand by the tray's "Debug"
  // menu item, hidden in Shutdown. The 10Hz update thread starts
  // on the first Show() and stays running until Shutdown() joins
  // it. Failure to create is non-fatal: the tray's callback
  // already logs "Debug unavailable" if debug_ is null.
  debug_ = std::make_unique<DebugWindow>();
  if (!debug_->Create(trayMsgWnd_)) {
    VMOSUE_LOG_WARN("DebugWindow init failed");
    debug_.reset();
  } else {
    // v0.3 (Task 36): auto-show the debug window on startup. The
    // user is iterating on hand-detection effects and needs the
    // live camera preview visible immediately, without having to
    // dig through the tray menu. The tray's "Debug" item still
    // works to re-show the window after the user closes it. We
    // push a log line so the action log inside the window itself
    // makes the auto-open behaviour visible.
    debug_->PushLog(L"Auto-open: debug window shown on startup");
    debug_->Show();
  }

  // Task 30: build the 6-step tutorial window. Parented to the
  // tray message window (hidden), created in the hidden state, and
  // shown on demand by the tray's "Tutorial" menu item. The
  // showTutorialOnLaunch check below auto-shows the window ~3s
  // after startup if the user has not disabled that flag in the
  // config. Failure to create is non-fatal: the tray's callback
  // already logs "Tutorial unavailable" if tutorial_ is null.
  tutorial_ = std::make_unique<TutorialWindow>();
  if (!tutorial_->Init(trayMsgWnd_)) {
    VMOSUE_LOG_WARN("TutorialWindow init failed");
    tutorial_.reset();
  }

  // Task 21: register the two emergency-stop triggers. Both call into
  // the state machine which sets state_=EmergencyStopped, drains
  // pending actions, and runs SafeReleaseAll on the InputInjector.
  // The Ctrl+Alt+G chord fires immediately on press; the Esc hotkey
  // fires after a 1000 ms hold to avoid accidental triggers (Esc is
  // also used by many apps to dismiss dialogs).
  Hotkey::RegisterCtrlAltG([this]() { sm_.EmergencyStop(); });
  Hotkey::RegisterEsc([this]() { sm_.EmergencyStop(); }, 1000);

  // v0.3 (Task 37): camera + workers + watchdog all live in a
  // single background thread. The Media Foundation SourceReader
  // init (MFCreateDeviceSource + SetCurrentMediaType) can take
  // 1-3 seconds on USB cameras while the driver and capture
  // pipeline enumerate. Previously this ran on the main thread
  // BEFORE the GetMessage loop, so the DebugWindow appeared as
  // a white unrendered rectangle with a busy mouse cursor for
  // the entire warmup window. Now the main thread skips past
  // this work and enters the message pump immediately; the
  // DebugWindow shows "Waiting for camera..." during the
  // background init and starts rendering real frames the moment
  // the camera delivers its first sample.
  //
  // We also move the worker-thread spawn + watchdog Start into
  // this same thread so the watchdog can register the worker
  // thread IDs while they're still valid. Heartbeat calls from
  // the workers before Watchdog::Start() is invoked are harmless
  // no-ops (Watchdog's Heartbeat is documented as such).
  startupT_ = std::thread([this, initialPerf, initialCfg]() {
    CameraCapture::Config ccfg;
    ccfg.deviceIndex = static_cast<uint32_t>(
        std::max(0, initialCfg.cameraIndex));
    ccfg.width = 1280;
    ccfg.height = 720;
    ccfg.fps = static_cast<uint32_t>(std::max(1, initialPerf.captureFps));
    if (!cam_.Init(ccfg).isOk()) {
      VMOSUE_LOG_ERROR("Camera init failed");
      return;
    }
    if (debug_) debug_->PushLog(L"Camera initialized (background)");

    cam_.Start();
    captureT_ = std::thread(&App::captureLoop, this);
    inferenceT_ = std::thread(&App::inferenceLoop, this);
    smT_ = std::thread(&App::stateMachineLoop, this);

    // Task 24: register each worker with the watchdog and start
    // the health monitor. 5s timeout gives plenty of slack over
    // the ~33ms-per-frame cadence. The callback just logs; a
    // future task can escalate to tearing the app down on
    // persistent timeouts.
    auto& wd = Watchdog::Get();
    wd.RegisterThread(captureT_.get_id(), "capture", std::chrono::seconds(5));
    wd.RegisterThread(inferenceT_.get_id(), "inference", std::chrono::seconds(5));
    wd.RegisterThread(smT_.get_id(), "stateMachine", std::chrono::seconds(5));
    wd.Start([](const std::string& name) {
      VMOSUE_LOG_WARN("Watchdog timeout: {}", name);
    });
    if (debug_) debug_->PushLog(L"Worker threads + watchdog live");
  });

  VMOSUE_LOG_INFO("App started (UI up; camera init in background). "
                  "Press Ctrl+C in console to exit.");

  // Task 30: if the user has AppConfig::showTutorialOnLaunch set
  // (the in-class default is `true` for a fresh install; existing
  // users can opt out via the config), auto-show the tutorial
  // window after a short delay. The delay gives the main UI time
  // to appear (the tray icon is registered, the overlay is up)
  // before a tutorial window pops in front of it. We use the
  // message-pump's `steady_clock` checkpoints to fire the show
  // ~3s into the run; if the user dismisses the tutorial (or
  // never opens the menu), Shutdown() will tear it down cleanly.
  const auto tutorialLaunchAt = std::chrono::steady_clock::now() +
                                std::chrono::seconds(3);
  bool tutorialAutoShown = !Config::Get().Data().showTutorialOnLaunch;

  // Main thread pumps messages for ALL top-level windows on this
  // thread: the tray message-only window, the overlay, the debug
  // window, the settings dialog, and the tutorial window. The
  // second GetMessageW parameter is a window filter — passing the
  // tray HWND here restricts the pump to that window only, which
  // silently swallows WM_PAINT / WM_MOVE / WM_SIZE for every other
  // top-level window we create. The visible symptom was that the
  // DebugWindow never repainted (the cursor spun over it because the
  // main thread was technically responsive but never serviced the
  // paint); right-clicking the tray icon "fixed" it because
  // TrackPopupMenu's internal modal loop pumps the entire thread
  // queue and finally drains the backed-up WM_PAINTs. Passing
  // nullptr here makes the pump thread-wide, which is the standard
  // Win32 UI pump and what every other window in this app needs.
  //
  // We also use the running_ flag as a "quit" signal: when
  // Shutdown() flips it, we break out of the pump. This replaces
  // the prior busy-wait sleep loop so the tray menu events are
  // responsive.
  MSG msg;
  while (running_.load()) {
    BOOL got = GetMessageW(&msg, nullptr, 0, 0);
    if (got == 0 || got == -1) break;  // WM_QUIT or error
    TranslateMessage(&msg);
    DispatchMessageW(&msg);

    // Task 30: cheap deadline check on every pump iteration. The
    // overhead is a single steady_clock::now() call, no lock or
    // syscall. We check `running_` again so a Shutdown() that
    // races the deadline doesn't try to show a window whose
    // parent is in the middle of being destroyed.
    if (!tutorialAutoShown && tutorial_ &&
        std::chrono::steady_clock::now() >= tutorialLaunchAt &&
        running_.load()) {
      tutorial_->Show();
      tutorialAutoShown = true;
    }
  }

  Shutdown();
  return 0;
}

void App::Shutdown() {
  // Idempotent: the very first Shutdown flips running_ to false and
  // joins everything; subsequent calls early-out.
  if (!running_.exchange(false)) return;

  // Task 21: unregister the two emergency-stop hotkeys so their
  // watcher thread is joined before we tear down the rest of the
  // app. Otherwise the watcher can outlive the destructors of the
  // lambdas it captured (sm_, this) and crash on the next callback.
  Hotkey::UnregisterCtrlAltG();
  Hotkey::UnregisterEsc();

  // v0.3 (Task 37): join the background-init thread FIRST. It may
  // still be running if the user clicks Exit during the camera
  // warmup window; if it is, the join blocks until the MF init
  // finishes (or fails) and the worker threads are spawned (or
  // not). After this point, captureT_/inferenceT_/smT_ are
  // either valid threads (camera init succeeded) or not joinable
  // (camera init failed), and the watchdog is either started or
  // not. The two subsequent join blocks are guarded by joinable()
  // so they handle both cases.
  if (startupT_.joinable()) startupT_.join();

  if (captureT_.joinable()) captureT_.join();
  if (inferenceT_.joinable()) inferenceT_.join();
  if (smT_.joinable()) smT_.join();

  // Task 24: stop the watchdog AFTER the workers are joined, so
  // the watcher thread can never observe a stale thread id and so
  // we don't fire a spurious timeout during teardown. Stop is
  // idempotent so the second Shutdown() call from the destructor
  // is a no-op.
  Watchdog::Get().Stop();

  cam_.Stop();

  overlay_.Shutdown();

  // Task 27: hide the settings window before tearing down the tray
  // message window (which is its parent). Destroying the parent
  // would otherwise destroy the settings child for us, which is
  // fine for a hidden window but tidier to do explicitly via
  // Hide(). The unique_ptr's destructor will DestroyWindow the
  // hidden window when it goes out of scope.
  if (settings_) settings_->Hide();

  // Task 29: tear down the debug window. Shutdown() joins the
  // 10Hz update thread, releases the D2D factory / render target,
  // and destroys the HWND. We do this BEFORE the tray message
  // window goes away (the debug window is parented to it).
  if (debug_) debug_->Shutdown();

  // Task 30: tear down the tutorial window. Shutdown() destroys
  // the HWND. We do this BEFORE the tray message window goes
  // away (the tutorial window is parented to it) and after the
  // debug window since both share the same parent and the debug
  // window's WM_DESTROY will pump a few final messages.
  if (tutorial_) tutorial_->Shutdown();

  // Task 26: tear the tray icon down BEFORE the message-only window.
  // Shell_NotifyIcon(NIM_DELETE) needs the HWND to be valid; if we
  // destroyed the window first the shell would leak the icon until
  // the process exits (and may show a phantom icon on next logon).
  tray_.Shutdown();
  if (trayMsgWnd_) {
    DestroyWindow(trayMsgWnd_);
    trayMsgWnd_ = nullptr;
  }

  // Belt-and-braces: make sure the OS cursor state is sane before the
  // process goes away. SafeReleaseAll() is a no-op if nothing is held.
  InputInjector::Get().SafeReleaseAll();
}

void App::captureLoop() {
  try {
    // Task 28: throttle the capture poll to PerfMode::captureFps.
    // The CameraCapture SourceReader produces frames at the
    // camera's native rate (whatever the driver granted), so the
    // frame we'll get on each tick is the freshest one. A long
    // sleep on a slow mode just means the inference thread sees
    // older frames — acceptable. We also use `last_tick` to track
    // wall-clock so the throttle is rate-based, not loop-based
    // (otherwise a CPU-starved system would run slower than
    // requested without us ever catching up).
    using clock = std::chrono::steady_clock;
    auto last_tick = clock::now();
    while (running_.load()) {
      // Task 24: heartbeating at the top of the loop means a
      // thread that is alive and scheduling gets a tick at least
      // once per iteration. Heartbeat is internally throttled to
      // 1Hz so the lock cost is negligible.
      Watchdog::Get().Heartbeat(std::this_thread::get_id());

      // Read the perf mode on every tick so a user-driven
      // dropdown change in the Settings window takes effect
      // immediately on the next frame. The `currentFps_` atomic
      // (Task 33) is the live rate including the idle down-shift:
      // the inference loop downshifts to kIdleFps after 5s of no
      // detections and upshifts back to mode.captureFps on any
      // detection. We use min() so a user-initiated lower rate
      // (e.g., battery mode's 15fps) is respected even if the
      // idle down-shift would otherwise force 10Hz.
      const PerfMode mode = CurrentPerfMode();
      const int liveFps  = currentFps_.load(std::memory_order_relaxed);
      const int targetFps = std::max(1, std::min(mode.captureFps, liveFps));

      Frame f;
      if (cam_.TryGetLatestFrame(f)) {
        // SPSC: push() only fails if the queue is full, in which case
        // we drop the new frame. The capture thread runs at the
        // configured captureFps, the inference thread pulls at
        // inferenceFps, so the queue depth stays bounded.
        frameQ_.push(f);
        // v0.3 (Task 36): push the frame directly to the debug
        // window's own SPSC queue. The previous design pushed to a
        // mirror queue on App and expected DebugWindow to be its
        // consumer, but DebugWindow actually has its own internal
        // queue and never connected to App's mirror — so the
        // preview was permanently empty. PushFrame is a single
        // SPSC push (no-op when the window is hidden / not yet
        // created) and does not back-pressure the capture loop.
        if (debug_) debug_->PushFrame(f);
      }

      // Sleep for the remainder of the period. If the loop body
      // took longer than the period (CPU-starved host), we don't
      // sleep at all — just run as fast as we can.
      auto now = clock::now();
      int64_t elapsed_us =
          std::chrono::duration_cast<std::chrono::microseconds>(
              now - last_tick).count();
      last_tick = now;
      SleepForFps(targetFps, elapsed_us);
    }
  } catch (const std::exception& e) {
    VMOSUE_LOG_ERROR("captureLoop exception: {}", e.what());
  } catch (...) {
    VMOSUE_LOG_ERROR("captureLoop unknown exception");
  }
}

void App::inferenceLoop() {
  try {
    using clock = std::chrono::steady_clock;
    auto last_tick = clock::now();
    // Track the last useGpu we applied to the detector so we
    // only call SetUseGpu when the value actually changes (the
    // setter is cheap, but the log line on every iteration would
    // be noisy).
    bool lastGpu = detector_.UseGpu();
    // Task 33: a single downscale buffer reused across frames.
    // The detector's configured inference resolution is fixed
    // for the App's lifetime (640x480 default), so allocating
    // once is correct. We still re-check the resolution on each
    // frame so a future "live resolution switch" feature can
    // reconfigure without a restart — for v0.3 the value never
    // changes.
    Frame inferenceFrame;
    bool haveInferenceFrame = false;
    while (running_.load()) {
      // Task 24: see captureLoop().
      Watchdog::Get().Heartbeat(std::this_thread::get_id());

      const PerfMode mode = CurrentPerfMode();
      // Apply a useGpu change pushed via the Settings window.
      // In v0.2 the flag is stored-only (no MediaPipe GPU
      // delegate), but the wiring is in place for when the
      // delegate lands.
      if (mode.useGpu != lastGpu) {
        detector_.SetUseGpu(mode.useGpu);
        lastGpu = mode.useGpu;
        VMOSUE_LOG_INFO("PerfMode: HandDetector::useGpu -> {}",
                        mode.useGpu ? "on" : "off");
      }

      Frame f;
      if (frameQ_.pop(f)) {
        // Task 33: downscale the camera frame to the detector's
        // configured inference resolution before running the
        // model. ResizeFrame() is a no-op if the source is
        // already the right size (the camera may already be
        // configured for 640x480). The ProfileGuard wraps the
        // entire detect path so we can log a warn if P95
        // exceeds the 60ms spec budget.
        const uint32_t iw = static_cast<uint32_t>(detector_.InferenceWidth());
        const uint32_t ih = static_cast<uint32_t>(detector_.InferenceHeight());
        // Refresh the detector's stored resolution if the App
        // config changed it externally (not used in v0.3 but
        // cheap to keep the setter in the loop).
        if (iw > 0 && ih > 0) {
          if (!haveInferenceFrame ||
              inferenceFrame.width != iw || inferenceFrame.height != ih) {
            detector_.SetFrameSize(static_cast<int>(iw), static_cast<int>(ih));
            haveInferenceFrame = true;
          }
          ResizeFrame(f, inferenceFrame, iw, ih);
        } else {
          inferenceFrame = f;  // pass-through if config invalid
        }

        std::vector<HandLandmarks> hands;
        {
          PROFILE_GUARD("inference");
          hands = detector_.Detect(inferenceFrame);
          // Use the perf-mode inference rate for the smoother's
          // time step (dt = 1/fps). This keeps the One-Euro
          // filter tuned to the actual cadence of incoming
          // landmarks.
          const double dt = 1.0 / std::max(1, mode.inferenceFps);
          for (auto& h : hands) {
            smoother_.Smooth(h, dt);
          }
        }

        // Task 33: idle down-shift. If we got at least one
        // hand, mark the timestamp and (re)raise the capture
        // rate to the perf-mode ceiling. If the hands list is
        // empty and 5s have passed since the last detection,
        // lower the capture rate to kIdleFps so the camera +
        // detector are running at ~10Hz instead of 30Hz (or
        // 60Hz, on performance mode).
        const int64_t now_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                clock::now().time_since_epoch()).count();
        if (!hands.empty()) {
          lastDetectionMs_.store(now_ms, std::memory_order_relaxed);
          const int target = std::max(1, mode.captureFps);
          const int prev = currentFps_.load(std::memory_order_relaxed);
          if (prev != target) {
            currentFps_.store(target, std::memory_order_relaxed);
            VMOSUE_LOG_INFO("PerfMode: upshift to {} fps (hand detected)",
                            target);
          }
        } else {
          const int64_t last = lastDetectionMs_.load(std::memory_order_relaxed);
          if (now_ms - last >= kIdleDownshiftMs) {
            const int prev = currentFps_.load(std::memory_order_relaxed);
            if (prev != kIdleFps) {
              currentFps_.store(kIdleFps, std::memory_order_relaxed);
              VMOSUE_LOG_INFO("PerfMode: downshift to {} fps (idle >= {}ms)",
                              kIdleFps, kIdleDownshiftMs);
            }
          }
        }

        // v0.3 (Task 36): push landmarks directly to the debug
        // window. See the matching comment in captureLoop for why
        // we no longer use the App-side mirror queue. PushLandmarks
        // takes the vector by value, so it makes a copy into the
        // DebugWindow's own SPSC; hands is still valid for the
        // move into landmarkQ_ below. The copy is cheap relative
        // to the inference cost and only happens on frames where
        // a hand was actually detected.
        if (debug_) debug_->PushLandmarks(hands);

        // SPSC: same drop-on-full semantics as the capture queue. We
        // move() to avoid a deep copy of the HandLandmarks arrays.
        landmarkQ_.push(std::move(hands));
      }

      // Throttle the loop to the configured inference rate. The
      // sleep runs regardless of whether we processed a frame
      // so a queue with no data doesn't trigger a tight
      // busy-spin.
      auto now = clock::now();
      int64_t elapsed_us =
          std::chrono::duration_cast<std::chrono::microseconds>(
              now - last_tick).count();
      last_tick = now;
      SleepForFps(std::max(1, mode.inferenceFps), elapsed_us);
    }
  } catch (const std::exception& e) {
    VMOSUE_LOG_ERROR("inferenceLoop exception: {}", e.what());
  } catch (...) {
    VMOSUE_LOG_ERROR("inferenceLoop unknown exception");
  }
}

void App::stateMachineLoop() {
  try {
    auto last = std::chrono::steady_clock::now();
    while (running_.load()) {
      // Task 24: see captureLoop().
      Watchdog::Get().Heartbeat(std::this_thread::get_id());
      std::vector<HandLandmarks> hands;
      if (landmarkQ_.pop(hands)) {
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - last).count();
        last = now;

        sm_.OnLandmarks(hands, NowMicros() / 1000, dt);
        auto acts = sm_.ConsumeActions();
        auto& inj = InputInjector::Get();

        // Build Feedback for the overlay from the right-hand landmarks
        // (index MCP, point 5) and the current state.
        const HandLandmarks* right = nullptr;
        for (const auto& h : hands) {
          if (h.handedness == 1) { right = &h; break; }
        }
        Feedback fb{};
        if (right) {
          fb.cursorX = right->points[5].x;
          fb.cursorY = right->points[5].y;
          fb.confidence = right->score;
          fb.rightHandCount = 1;
          // v0.4: pipe the dominant hand's 21 landmarks to the overlay
          // so it can render the skeleton. points is std::array<Point2F, 21>
          // which matches Feedback::landmarks — assignment does a deep copy.
          fb.landmarks = right->points;
          fb.hasHand = true;
        } else {
          // No right hand this tick — clear the flag so the overlay draws
          // nothing (instead of drawing a stale skeleton).
          fb.hasHand = false;
        }
        for (const auto& h : hands) {
          if (h.handedness == 0) { fb.leftHandCount = 1; break; }
        }
        fb.paused = (sm_.State() == GlobalState::Paused);
        overlay_.Update(fb);

        // Emergency stop: the GestureStateMachine has tripped a safe
        // release. Clear any held LMB, flush the OS state, and exit
        // this thread. We use `break` (not `return`) so the
        // post-loop SafeReleaseAll() cleanup below still runs.
        if (acts.safeRelease) {
          inj.SafeReleaseAll();
          inj.LeftUp();
          break;
        }

        if (acts.leftClick) inj.LeftClick();
        // Spec bug fix #3: a system-level double click requires two
        // LeftClick() calls within the OS double-click window
        // (~500ms). The original spec emitted a single click here,
        // which is functionally a single-click. Emit two.
        if (acts.leftDoubleClick) {
          inj.LeftClick();
          inj.LeftClick();
        }
        if (acts.leftDown) inj.LeftDown();
        if (acts.leftUp)   inj.LeftUp();
        if (acts.rightClick) inj.RightClick();

        // Spec bug fix #1: the state machine publishes cursor deltas
        // (cursorDx, cursorDy) and the original spec consumed them
        // into local variables but never injected them. This is the
        // whole point of the gesture-driven mouse, so do it here.
        if (acts.cursorDx != 0 || acts.cursorDy != 0) {
          inj.MoveCursor(acts.cursorDx, acts.cursorDy);
        }

        // Spec bug fix #2: ActionSet has a `wheel` field that was
        // never being injected. Forward it.
        if (acts.wheel != 0) {
          inj.Wheel(acts.wheel);
        }

        // Task 29: publish a one-line summary of the action set to
        // the debug window's action log. We only push the "interesting"
        // events (anything that actually mutates OS state) — empty
        // action sets are the common case and would flood the log
        // at 30Hz. Format mirrors the action log convention.
        if (debug_ && (acts.leftClick || acts.leftDoubleClick ||
                       acts.leftDown || acts.leftUp || acts.rightClick ||
                       acts.cursorDx != 0 || acts.cursorDy != 0 ||
                       acts.wheel != 0 || acts.safeRelease)) {
          wchar_t buf[128];
          swprintf_s(buf,
              L"sm: click=%d dclick=%d down=%d up=%d rclick=%d "
              L"dx=%d dy=%d wheel=%d",
              acts.leftClick ? 1 : 0,
              acts.leftDoubleClick ? 1 : 0,
              acts.leftDown ? 1 : 0,
              acts.leftUp ? 1 : 0,
              acts.rightClick ? 1 : 0,
              acts.cursorDx, acts.cursorDy, acts.wheel);
          debug_->PushLog(buf);
        }
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }

      // Task 29: publish state + FPS rolling averages to the
      // debug window. The state machine ticks are derived from
      // the perf-mode inferenceFps when data is flowing; for v0.2
      // we just report that as the state machine rate. The
      // capture and inference FPS are computed inside their own
      // loops (see below) using a 1Hz sliding counter.
      if (debug_) {
        const PerfMode mode = CurrentPerfMode();
        const double smFps = static_cast<double>(mode.inferenceFps);
        debug_->PushFps(0.0, 0.0, smFps);
        debug_->PushState(static_cast<int>(sm_.State()));
      }
    }
  } catch (const std::exception& e) {
    VMOSUE_LOG_ERROR("stateMachineLoop exception: {}", e.what());
  } catch (...) {
    VMOSUE_LOG_ERROR("stateMachineLoop unknown exception");
  }

  // After the loop exits (emergency stop, Shutdown, or exception)
  // make sure the OS is left in a clean state: no held mouse buttons,
  // no held modifier keys.
  InputInjector::Get().SafeReleaseAll();
}

}  // namespace vmosue
