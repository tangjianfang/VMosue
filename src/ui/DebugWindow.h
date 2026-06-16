#pragma once
// Task 29: developer / power-user debug window.
//
// A top-level Win32 window that, when opened from the tray's "Debug"
// menu item, shows the runtime state of the system in real time:
//
//   - Live camera preview (scaled-down; NV12/BGR24 frames converted to
//     a D2D bitmap; unsupported formats show a placeholder)
//   - Detected hand landmarks (21 dots per hand, in normalized image
//     coordinates, overlaid on the preview)
//   - State machine state (Active / Paused / EmergencyStopped)
//   - FPS counters (capture / inference / state machine)
//   - Last N action log lines (left-click, double-click, drag, scroll,
//     cursor deltas, etc.)
//   - Configuration dump (active profile, sensitivity, perf mode,
//     camera index, ...)
//
// The window runs its own 10Hz update thread that pulls the latest
// frame and landmark set from the App's debug SPSC queues, snapshots
// the data, and calls InvalidateRect to schedule a WM_PAINT. The
// WndProc's WM_PAINT handler reads the snapshot and draws everything
// with Direct2D (mirroring the OverlayWindow pattern from Task 15).
//
// Lifetime: App owns a `std::unique_ptr<DebugWindow>`. The window is
// created on demand (when the user clicks the tray's "Debug" item)
// and destroyed on app shutdown. Show()/Hide() are idempotent.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d2d1.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <boost/lockfree/spsc_queue.hpp>

#include "capture/Frame.h"
#include "inference/HandDetector.h"
#include "util/FrameConvert.h"  // Nv12FrameToBgra / Bgr24FrameToBgra

namespace vmosue {

// The pixel-format conversion helpers (Nv12FrameToBgra /
// Bgr24FrameToBgra) live in src/util/FrameConvert.{h,cpp} so the
// unit tests can link them without pulling in the Win32 / D2D
// surface. Re-exported here as a convenience for callers that
// already include this header.

// A single line in the rolling action log. Stored as a wide string so
// the D2D text render path doesn't have to do a per-frame conversion.
struct DebugLogLine {
  uint64_t timestampUs = 0;
  std::wstring text;
};

// Snapshot of the runtime state consumed by the WM_PAINT handler.
// Updated by the update thread under snapshotMu_; read by WM_PAINT
// under the same mutex. Keeping a single snapshot avoids the WM_PAINT
// handler racing with the update thread on individual fields.
struct DebugSnapshot {
  // Latest camera frame (may be empty if no frame has arrived yet).
  Frame frame;
  // True if the most recent frame snapshot is fresh (i.e. the update
  // thread pushed a new frame since the last WM_PAINT). Cleared by
  // the renderer after drawing.
  bool frameFresh = false;

  // Latest landmark set (may be empty).
  std::vector<HandLandmarks> landmarks;

  // State machine state.
  // 0 = Active, 1 = Paused, 2 = EmergencyStopped (matches GlobalState).
  int stateValue = 0;

  // FPS rolling averages.
  double captureFps   = 0.0;
  double inferenceFps = 0.0;
  double stateMachineFps = 0.0;

  // Rolling action log (most recent last). Capped at kMaxLogLines
  // entries; older entries are dropped from the front.
  std::vector<DebugLogLine> log;
};

class DebugWindow {
 public:
  DebugWindow();
  ~DebugWindow();

  DebugWindow(const DebugWindow&) = delete;
  DebugWindow& operator=(const DebugWindow&) = delete;

  // Register the window class and create the modeless top-level
  // window. Idempotent: a second call returns the existing HWND.
  HWND Create(HWND hwndParent);

  // Show the window, start the update thread (if not already running),
  // and bring it to the foreground. Idempotent.
  void Show();

  // Hide the window and stop the update thread. Safe to call when
  // the window is already hidden (no-op).
  void Hide();

  // Tear down the window and stop the update thread. Called from
  // App::Shutdown().
  void Shutdown();

  // ---- Producers (called from the App's worker threads) ----
  // Push the latest camera frame onto the debug queue. Drops on full.
  void PushFrame(const Frame& f);
  // Push the latest landmark set onto the debug queue. Drops on full.
  void PushLandmarks(std::vector<HandLandmarks> hands);
  // Push a one-line action log entry. Capped at kMaxLogLines.
  void PushLog(const std::wstring& text);
  // Publish the current rolling FPS values.
  void PushFps(double capture, double inference, double stateMachine);
  // Publish the current state machine state value.
  void PushState(int stateValue);

 private:
  // Update thread: sleeps 100ms, drains the SPSC queues into the
  // snapshot, then InvalidateRect's the window so WM_PAINT fires.
  void updateLoop();

  // WM_PAINT handler. Reads the snapshot under the mutex and draws
  // everything via Direct2D.
  void render();

  // Append a DebugLogLine to the snapshot log, evicting the oldest
  // entry if we'd exceed kMaxLogLines. Called under snapshotMu_.
  void appendLogLineLocked(DebugLogLine line);

  // Static WndProc forwarding to the instance via GWLP_USERDATA.
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

  // ---- Window state ----
  HWND hwnd_ = nullptr;
  // v0.3 (Task 36): bumped the window and preview sizes so the
  // camera feed is large enough to actually see hand details. The
  // preview is now 640x360 (16:9, matching the 1280x720 camera) and
  // the right-side text panel is correspondingly wider.
  static constexpr int kWindowW = 960;
  static constexpr int kWindowH = 720;
  static constexpr int kPreviewW = 640;
  static constexpr int kPreviewH = 360;
  // Maximum number of action log lines retained.
  static constexpr size_t kMaxLogLines = 100;
  // Update cadence (Hz). 10Hz keeps CPU usage low while still feeling
  // real-time to a human watching the window.
  static constexpr int kUpdateHz = 10;

  // ---- D2D ----
  ID2D1Factory* d2dFactory_ = nullptr;
  ID2D1HwndRenderTarget* renderTarget_ = nullptr;
  ID2D1SolidColorBrush* brushWhite_ = nullptr;
  ID2D1SolidColorBrush* brushGreen_ = nullptr;
  ID2D1SolidColorBrush* brushYellow_ = nullptr;
  ID2D1SolidColorBrush* brushRed_ = nullptr;
  ID2D1SolidColorBrush* brushBlack_ = nullptr;

  // ---- Threads / synchronization ----
  std::atomic<bool> running_{false};
  std::thread updateT_;
  std::mutex snapshotMu_;
  DebugSnapshot snapshot_;

  // ---- SPSC queues (producers = App's worker threads, consumer = update thread) ----
  // Frame: capacity 2 (one in-flight, one slot of slack).
  boost::lockfree::spsc_queue<Frame, boost::lockfree::capacity<2>> frameQ_;
  // Landmarks: capacity 4 (same as App's main landmarkQ_).
  boost::lockfree::spsc_queue<std::vector<HandLandmarks>,
                              boost::lockfree::capacity<4>>
      landmarkQ_;

  // ---- Camera-warmup indicator ----
  // Wall-clock time at which the user first saw the preview area
  // with no frame in it. Reset to zero once the first frame
  // arrives so the elapsed counter stops ticking. The render
  // function shows "Waiting for camera... (Ns)" so the user can
  // tell the app is alive during the Media Foundation warmup
  // window (which is typically 1-3 seconds; sometimes longer
  // for slower USB cameras).
  std::chrono::steady_clock::time_point waitingSince_{};
  bool hasShownFirstFrame_ = false;
};

}  // namespace vmosue
