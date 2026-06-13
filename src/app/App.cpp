#include "app/App.h"

#include "app/Watchdog.h"
#include "input/InputInjector.h"
#include "platform/Hotkey.h"
#include "util/Logger.h"
#include "ui/TrayIcon.h"

#include <chrono>
#include <exception>

namespace vmosue {

namespace {
// Static window class registration for the tray message-only window.
// Done in the .cpp TU so the class name and WndProc are local. The
// class is registered once (the first time App::Run is called) and
// left registered for the process lifetime — App is a singleton in
// practice.
const wchar_t* const kTrayMsgClass = L"VMosueTrayMessageWindow";
bool g_trayClassRegistered = false;
}  // namespace

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

  CameraCapture::Config ccfg;
  ccfg.width = 1280;
  ccfg.height = 720;
  ccfg.fps = 30;
  if (!cam_.Init(ccfg).isOk()) {
    VMOSUE_LOG_ERROR("Camera init failed");
    return 1;
  }

  HandDetector::Config hcfg;
  if (!detector_.Init(hcfg).isOk()) {
    VMOSUE_LOG_ERROR("HandDetector init failed");
    return 1;
  }

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
    cb.onOpenSettings = []()  { VMOSUE_LOG_INFO("Tray: Settings (not implemented yet)"); };
    cb.onOpenDebug    = []()  { VMOSUE_LOG_INFO("Tray: Debug (not implemented yet)"); };
    cb.onOpenTutorial = []()  { VMOSUE_LOG_INFO("Tray: Tutorial (not implemented yet)"); };
    cb.onExit         = [this]() { VMOSUE_LOG_INFO("Tray: Exit requested"); Shutdown(); };
    if (!tray_.Init(trayMsgWnd_, cb)) {
      VMOSUE_LOG_WARN("Tray icon init failed");
    }
  }

  // Task 21: register the two emergency-stop triggers. Both call into
  // the state machine which sets state_=EmergencyStopped, drains
  // pending actions, and runs SafeReleaseAll on the InputInjector.
  // The Ctrl+Alt+G chord fires immediately on press; the Esc hotkey
  // fires after a 1000 ms hold to avoid accidental triggers (Esc is
  // also used by many apps to dismiss dialogs).
  Hotkey::RegisterCtrlAltG([this]() { sm_.EmergencyStop(); });
  Hotkey::RegisterEsc([this]() { sm_.EmergencyStop(); }, 1000);

  cam_.Start();
  captureT_ = std::thread(&App::captureLoop, this);
  inferenceT_ = std::thread(&App::inferenceLoop, this);
  smT_ = std::thread(&App::stateMachineLoop, this);

  // Task 24: register each worker with the watchdog and start the
  // health monitor. 5s timeout gives plenty of slack over the
  // ~33ms-per-frame cadence — a thread that hasn't heartbeated for
  // 5s is in serious trouble (blocked on I/O, deadlocked, or
  // crashed). The callback just logs; a future task can escalate to
  // tearing the app down on persistent timeouts.
  auto& wd = Watchdog::Get();
  wd.RegisterThread(captureT_.get_id(), "capture", std::chrono::seconds(5));
  wd.RegisterThread(inferenceT_.get_id(), "inference", std::chrono::seconds(5));
  wd.RegisterThread(smT_.get_id(), "stateMachine", std::chrono::seconds(5));
  wd.Start([](const std::string& name) {
    VMOSUE_LOG_WARN("Watchdog timeout: {}", name);
  });

  VMOSUE_LOG_INFO("App started. Press Ctrl+C in console to exit.");

  // Main thread pumps messages from the tray message-only window so
  // the tray icon callbacks can dispatch. We also use the running_
  // flag as a "quit" signal: when Shutdown() flips it, we break out
  // of the pump. This replaces the prior busy-wait sleep loop so the
  // tray menu events are responsive.
  MSG msg;
  while (running_.load()) {
    BOOL got = GetMessageW(&msg, trayMsgWnd_, 0, 0);
    if (got == 0 || got == -1) break;  // WM_QUIT or error
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
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
    while (running_.load()) {
      // Task 24: heartbeating at the top of the loop means a
      // thread that is alive and scheduling gets a tick at least
      // once per iteration. Heartbeat is internally throttled to
      // 1Hz so the lock cost is negligible even though we run
      // ~1000Hz here.
      Watchdog::Get().Heartbeat(std::this_thread::get_id());
      Frame f;
      if (cam_.TryGetLatestFrame(f)) {
        // SPSC: push() only fails if the queue is full, in which case
        // we drop the new frame. The capture thread runs slower than
        // the camera's effective frame rate so this is acceptable
        // here; the inference thread will simply see fresh data next
        // iteration.
        frameQ_.push(f);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  } catch (const std::exception& e) {
    VMOSUE_LOG_ERROR("captureLoop exception: {}", e.what());
  } catch (...) {
    VMOSUE_LOG_ERROR("captureLoop unknown exception");
  }
}

void App::inferenceLoop() {
  try {
    while (running_.load()) {
      // Task 24: see captureLoop().
      Watchdog::Get().Heartbeat(std::this_thread::get_id());
      Frame f;
      if (frameQ_.pop(f)) {
        auto hands = detector_.Detect(f);
        for (auto& h : hands) {
          smoother_.Smooth(h, 1.0 / 30.0);
        }
        // SPSC: same drop-on-full semantics as the capture queue. We
        // move() to avoid a deep copy of the HandLandmarks arrays.
        landmarkQ_.push(std::move(hands));
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
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
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
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
