# Changelog

All notable changes to VMosue are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.6.2] - 2026-06-18

### Added

- **Anti-phantom, two-layer protection.** `HandStabilityFilter` drops any
  handedness whose score has not been above the adaptive floor for at
  least 5 consecutive frames (~167 ms at 30 fps); `DwellGate` further
  requires every one-shot action to be continuously held for `dwellMs`
  (production default 2500 ms) before the release event is allowed to
  fire. End-to-end: 20 synthetic phantom bursts → 0 clicks; real
  sustained pinch → 1 click on release (`PhantomE2E.*`).
- **Per-action dwell preview.** Top-center overlay renders
  `About to: Left click 1.2s` in 36 pt bold yellow text with a 520×14
  progress bar, plus a 200 ms white flash on commit. Cursor still moves
  during the dwell so the user has live feedback that the system sees
  their hand.
- **First-hand settle-in grace.** New 1500 ms window after the user's
  hand first appears in which the cursor moves but every button event
  is suppressed. Overlay shows `Calibrating... 1.2s` and flashes
  `✓ Ready` when the window ends.
- **Action-list help window** (`ActionListWindow`, F1 hotkey or tray
  menu `Action list`) lists the 7 gestures → actions mapping. Auto-shows
  ~2 s after launch when `Config::showActionListOnLaunch` is true.
- **Tutorial step 7** (action reference table). 7-step tutorial now ends
  with a summary of every gesture → action mapping.
- **`test_phantom_e2e.cpp`** — new end-to-end regression suite covering
  phantom bursts, dwell-met fires, gap-in-burst cancels dwell, and
  sustained-real-pinch produces exactly one click.
- **Tighter adaptive `MinHandScore`** (bias 0.05 → 0.16, floor 0.3) and
  adaptive pinch/release thresholds reduce cold-start false positives
  without hurting real-gesture acceptance (≥95 %, tested).

### Fixed

- **`DwellGate` now reads `*Held` signals, not release events.**
  Architectural bug from v0.6 rc1: `ClickDetector` only emits `LeftClick`
  on the release frame (one frame), so `DwellGate.Process` saw a
  `startMs = releaseFrameTs` and immediately disarmed on the next
  frame. The dwell could never elapse, so `cfg.dwellMs > 0` would have
  made every click in production silently never fire. The new model
  feeds the gate the *gesture-held* booleans (`leftPinchHeld`,
  `middlePinchHeld`, `rightPushHeld`) read from new
  `ClickDetector::IsLeftPinching()` / `IsMiddlePinching()` and
  `AirClickDetector::IsApproaching()` getters, and only commits on the
  release frame after the slot has been continuously held for
  `dwellMs`. Also: `OnLandmarks` no longer early-returns on no-hand
  frames without running `DwellGate.Process(empty, ts)`, so 30-frame
  no-hand gaps between 5-frame phantom bursts now correctly disarm the
  phantom-armed slot (previously 19/20 phantom bursts fired).

## [Unreleased]

### Added

- **Adaptive parameters (v0.5).** Every environment- or signal-derived
  tunable is now a deterministic function of observed signals instead
  of a hard-coded constant or a user slider. The phantom-hand filter
  uses a score-gap rule, the overlay colour tiers use rolling
  percentiles, the render cadence tracks the observed camera FPS, the
  1-Euro smoothing adapts to the measured noise floor and motion
  magnitude, the cursor dead-zone adapts to cursor stillness, and the
  pinch / scroll / air-click thresholds adapt to the observed motion
  range. Cold-start falls back to the v0.4 constants and blends in over
  the first ~2 s. See `docs/superpowers/specs/2026-06-17-adaptive-parameters.md`.
- **Middle click** — thumb-to-middle-finger pinch and release on the
  right hand emits a middle (mouse-wheel) click. Left click wins if
  both pinches are detected in the same instant.
- **Horizontal scrolling** — the two-finger scroll gesture on the left
  hand now sends horizontal-wheel (tilt) events for left/right hand
  motion in addition to the existing vertical wheel.
- **System-aligned double-click window** — the pinch double-click
  interval now follows the user's Windows mouse double-click speed
  (`GetDoubleClickTime`, clamped to 200–900 ms) instead of a hard-coded
  400 ms, so the gesture matches every other double-click on the
  machine.
- **World landmarks over the IPC bridge** — the Python detector now
  emits metric `world_landmarks` alongside the normalized landmarks.

### Changed

- **Settings window** dropped the sensitivity / threshold sliders in
  favour of live "Adaptive (auto)" readouts of the currently-derived
  values (v0.5 Wave 4).
- **Render hot path** — cut per-frame allocation and latency: the
  capture loop reuses a single `Frame` buffer across iterations instead
  of reallocating the ~3.6 MB pixel vector every frame; the IPC
  metadata line is hand-serialized instead of going through the general
  JSON serializer; and `ProfileGuard::P95Ms` uses `nth_element` (O(N))
  instead of a full sort.
- Cursor X is inverted to match the user-perceived hand direction
  (selfie-mirrored webcam convention).

### Fixed

- **Right-click gesture now works in production.** The push-toward-
  camera right click reads `world[8].z` (index-fingertip depth), but
  the IPC parser only ever populated the normalized `points` array and
  the Python server never sent world landmarks — so `world` was always
  all-zero and the gesture could never fire outside the unit tests.
  The server now emits world landmarks and the C++ parser populates
  `HandLandmarks::world`.
- **Hand-detector subprocess auto-recovers from a crash.** `Detect()`
  checks the child process is still alive and respawns it (bounded to
  three attempts per dead streak, reset on the next healthy frame)
  instead of writing into a broken pipe forever and freezing the
  cursor.
- **Python detector survives a bad frame.** Per-frame processing is
  wrapped so a transient MediaPipe error or malformed frame degrades to
  a "zero hands" response instead of crashing the whole subprocess. The
  metadata validator also rejects absurd frame dimensions (> ~35 MP)
  before any buffer is allocated.
- **Calibration profiles are written atomically** (temp file + rename),
  matching `Config::Save`, so a crash mid-write can no longer truncate
  an existing profile.
- **Cursor controller guards against non-finite landmarks and oversized
  virtual-desktop dimensions**, preventing undefined behaviour in the
  normalized-to-pixel cast.
- Right click is suppressed when a left or middle button event fires in
  the same frame, preventing a conflicting `leftUp + rightClick`
  injection during a pinch-drag that ends on a forward push.

### Removed

- Dead `AirClickDetector::Phase::Retreat` state and the unused
  `baseZ_` / `palmStableAtStart_` fields.

## [1.0.0] - 2026-06-14

The first stable release of VMosue — a Windows 10/11 native
gesture-controlled mouse application. Move the cursor and click with
hand gestures captured by your webcam; no gloves, no controllers, no
special hardware.

### Highlights

- **Real-time hand tracking** via the bundled MediaPipe Tasks model
  (no model download required at install time).
- **Eight supported gestures** covering cursor movement, all standard
  mouse buttons, drag, double-click, scrolling, pause/resume, and an
  emergency stop.
- **Multi-monitor support** that respects each display's virtual
  desktop rectangle and DPI scale.
- **CPU-only pipeline** — no GPU required, intentionally lightweight
  so it does not interfere with the foreground app.
- **Live overlay indicator** showing tracking state and the active
  gesture.
- **NSIS installer** that drops everything under `C:\Program Files\VMosue`
  with Start Menu and Desktop shortcuts, and an uninstaller that
  cleans up cleanly.
- **Trilingual-friendly UI** scaffolded with English (Simplified
  Chinese shipped, additional locales can be added by dropping a JSON
  file in `resources/i18n/`).

### Supported gestures

| Gesture             | Hand  | Motion                                         | Action                      |
|---------------------|-------|------------------------------------------------|-----------------------------|
| Cursor              | Right | Move hand                                      | Cursor follows index tip    |
| Left click          | Right | Thumb-to-index pinch + release                 | Single click                |
| Double click        | Right | Two quick pinches (< 300 ms apart)             | Double click                |
| Left drag           | Right | Pinch and hold + move                          | Drag                        |
| Right click         | Right | Push hand toward camera (or quick back-flick)  | Right click                 |
| Scroll              | Left  | Two fingers close + move up/down               | Mouse wheel events          |
| Pause / Resume      | Left  | Open left hand held still 1 s                  | Toggle pause (overlay yellow) |
| Emergency stop      | Either| Both hands open, or `Ctrl+Alt+G`, or `Esc` 1 s | Release everything, stop    |

See `docs/user/gestures.md` for diagrams and per-gesture configuration
tips.

### Features

#### Capture and inference
- Camera capture via Windows Media Foundation (`IMFSourceReader`), with
  device enumeration for the Settings camera dropdown.
- Hand detection via MediaPipe Tasks (`HandLandmarker`) running on CPU.
- Per-frame landmark smoothing using a 1-Euro filter (configurable).
- Configurable inference resolution with auto-downscaling when the
  tracker has been idle, plus ROI tracking to keep the detector focused
  on the hand.
- Profile guards around hot sections so accidental debug builds do not
  ship without optimisations.

#### Gestures
- Cursor control with edge-clamping to the active monitor and optional
  acceleration curve.
- Pinch-based click and double-click with a tunable threshold and a
  guard that suppresses the "first click of a double click" until the
  double-click window expires.
- Drag detector that holds the button down while the pinch is
  maintained.
- Right-click via depth-based "push toward camera" detection, with a
  fallback backward flick when the camera does not expose depth well.
- Scroll detector for vertical wheel events on the left hand.
- Pause detector on the left hand that toggles overlay-yellow state
  and ignores all gesture input until motion resumes.
- Emergency stop triggered by either both hands open, `Ctrl+Alt+G`, or
  holding `Esc` for 1 s.

#### Platform integration
- Input injection via `SendInput` (mouse move / button / wheel).
- Multi-monitor aware cursor movement using `MonitorFromPoint` and the
  virtual desktop rectangle.
- Global hotkeys (`Ctrl+Alt+G` for emergency stop, `Esc` hold for stop,
  auto-rearming via a per-hotkey latch).
- Auto-start toggle (HKCU `Run` registry entry) configurable from
  Settings.
- Camera device enumeration exposed in the Settings window.

#### UI
- Direct2D overlay showing tracking state, active gesture, and pause
  indicator.
- System tray icon with menu: Pause, Settings, Debug, Tutorial, Exit.
- Settings window: camera selection, sensitivity, pinch threshold,
  scroll speed, performance mode (Battery / Balanced / Performance),
  auto-start, language, calibration launcher.
- Debug window: live preview with landmark overlay, current state, FPS
  graph, and action log.
- 6-step interactive Tutorial window covering every supported gesture.
- i18n framework with `en` and `zh-CN` bundled; runtime language
  switching; auto-detect on first run.

#### Reliability and performance
- Watchdog thread that monitors the capture / inference / gesture
  pipeline and surfaces stuck-thread errors in the log.
- Daily-rotated plain-text log under `%LOCALAPPDATA%\VMosue\logs\`.
- Per-frame profiling in Debug builds; Release builds run with
  ProfileGuards stripped.
- Configurable performance modes (Battery / Balanced / Performance) that
  change capture and inference FPS targets.

#### Configuration and persistence
- JSON config at `%LOCALAPPDATA%\VMosue\config.json` with safe defaults
  and corruption recovery (a malformed file is renamed to
  `config.json.bak` and defaults are re-applied).
- Calibration: guided flow that records lighting, hand size, and
  per-gesture thresholds for the current camera and saves them to the
  user profile.

#### Packaging
- NSIS 3.x installer (`VMosue-Setup-1.0.0.exe`) per-user, no UAC
  elevation required, registers uninstall metadata under HKCU.
- Uninstaller removes the install directory, the desktop shortcut, the
  Start Menu shortcut, and the uninstall registry key.

#### Testing and CI
- GoogleTest-based unit test suite (camera, gesture, config, util,
  platform) plus an end-to-end pipeline test driven by a recorded
  landmark fixture.
- MSVC parse-check script that builds every translation unit against
  stub headers to catch syntactic drift without a full link.
- GitHub Actions CI on push and pull request (`.github/workflows/ci.yml`).
- GitHub Actions release workflow that builds the installer on every
  push and, on `v*.*.*` tags, attaches it to a GitHub Release.

### Documentation

- `README.md` — top-level overview, install, build, links.
- `docs/user/quickstart.md` — install, first-time setup, camera
  positioning, logs.
- `docs/user/gestures.md` — every gesture with diagrams.
- `docs/user/troubleshooting.md` — most common issues and fixes.
- `docs/user/tutorial.md` — the in-app 6-step tutorial transcript.
- `docs/build-notes.md` — building from source, MSVC parse-check,
  bootstrap script.
- `docs/acceptance/` — manual acceptance test protocol for v1.0.
- `docs/superpowers/specs/` — design specs.
- `docs/superpowers/plans/` — implementation plan.

### Known limitations

- **Windows 10/11 x64 only.** ARM-based Windows devices (Snapdragon X,
  etc.) are not supported.
- **Right-click via depth** requires a camera that exposes depth well
  (most built-in laptop cameras do not). On those cameras, right-click
  falls back to a quick backward flick and may fire accidentally.
- **No GPU acceleration.** Inference runs on the CPU. Very high
  resolution webcams may benefit from `Settings -> Performance ->
  Battery` mode.
- **Single user, single profile.** Calibration and settings are stored
  per-Windows-user; there is no per-camera automatic switching.
- **English and Simplified Chinese only.** The i18n framework ships with
  these two; other locales can be added by dropping a JSON file in
  `resources/i18n/`.
- **Installer uses the legacy NSIS UI.** There is no MUI2 welcome /
  finish page yet — see `installer/README.md` for the upgrade path.
- **No `vc_redist` bootstrap.** The Visual C++ runtime is expected to
  be present on Windows 10/11.
- **Pre-release quality.** Despite the 1.0.0 version number, this is the
  first public release. Expect rough edges, especially around unusual
  lighting and unusual cameras — please file issues with log files.

### Upgrade notes

There is no previous released version. Installing 1.0.0 alongside a
development build is safe; the two will not interfere because the
installed copy registers itself under HKCU and does not auto-start by
default.

To uninstall, use **Settings -> Apps -> Installed apps -> VMosue ->
Uninstall**, or run the `uninstall.exe` left in the install directory.

### Credits

- MediaPipe Tasks for the hand landmarker.
- spdlog, nlohmann/json, Boost.Lockfree, GoogleTest (vcpkg
  dependencies).
- NSIS for the installer.

## Pre-1.0 development

The project was developed as a single continuous stream from initial
skeleton to the v1.0.0 release described above. Prior internal
checkpoints (skeleton, CI, core types, capture, inference, gestures,
platform integration, UI, settings, i18n, packaging) are reflected in
the git history rather than as separate changelog entries.

[Unreleased]: https://github.com/.../compare/v1.0.0...HEAD
[1.0.0]: https://github.com/.../releases/tag/v1.0.0