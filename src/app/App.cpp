#include "app/App.h"

#include "input/InputInjector.h"
#include "util/Logger.h"

#include <chrono>
#include <exception>

namespace vmosue {

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

  cam_.Start();
  captureT_ = std::thread(&App::captureLoop, this);
  inferenceT_ = std::thread(&App::inferenceLoop, this);
  smT_ = std::thread(&App::stateMachineLoop, this);

  VMOSUE_LOG_INFO("App started. Press Ctrl+C in console to exit.");

  // Main thread just sleeps until Shutdown() flips running_. A real
  // product would install a console control handler and call Shutdown
  // from there; that's the responsibility of a later task (e.g.
  // EmergencyStop hotkey).
  while (running_.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  Shutdown();
  return 0;
}

void App::Shutdown() {
  // Idempotent: the very first Shutdown flips running_ to false and
  // joins everything; subsequent calls early-out.
  if (!running_.exchange(false)) return;

  if (captureT_.joinable()) captureT_.join();
  if (inferenceT_.joinable()) inferenceT_.join();
  if (smT_.joinable()) smT_.join();

  cam_.Stop();

  overlay_.Shutdown();

  // Belt-and-braces: make sure the OS cursor state is sane before the
  // process goes away. SafeReleaseAll() is a no-op if nothing is held.
  InputInjector::Get().SafeReleaseAll();
}

void App::captureLoop() {
  try {
    while (running_.load()) {
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
