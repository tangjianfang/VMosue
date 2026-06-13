# Build notes

## MediaPipe Tasks C++ integration (Task 8)

### Approach chosen: CMake `FetchContent`

`src/inference/CMakeLists.txt` fetches the MediaPipe Tasks C++ sample from
the `mediapipe-samples` GitHub repository (subdirectory
`examples/gesture_recognizer/cpp`) using `FetchContent_Declare` +
`FetchContent_MakeAvailable`, matching the Task 8 spec.

We considered the vcpkg alternative. As of 2026-06-14 there is no first-party
`mediapipe` port in the vcpkg catalog — only community overlays exist, and
none of them ship a stable, Windows-ABI-compatible build of the Tasks C++
runtime. Switching to vcpkg would have required a custom port and a
precompiled binary, which adds maintenance burden without solving the
underlying network-fetch problem (vcpkg also fetches source from GitHub).

Therefore: **FetchContent of `mediapipe-samples/examples/gesture_recognizer/cpp`**
is the chosen approach. It tracks upstream and matches the structure
referenced in the design spec.

The FetchContent step is gated by the `VMOSUE_FETCH_MEDIAPIPE` option
(default ON). Set `-DVMOSUE_FETCH_MEDIAPIPE=OFF` to skip the fetch on
offline machines — the inference library will still build (it has a stub
implementation), but the real hand detection will not work.

### Environmental limitation

The build environment used for VMosue development (2026-06-14) cannot reach
`github.com` or `storage.googleapis.com` from inside the sandbox. As a
result:

1. `FetchContent` of `mediapipe-samples` will fail with a network error.
   The full `cmake --build` of `vmosue_inference` therefore cannot run to
   completion in this environment. The library compiles only when the
   `VMOSUE_FETCH_MEDIAPIPE=OFF` option is used (in which case the
   `mediapipe_tasks` target is not defined and the inference source
   files use a no-op stub).
2. The hand landmark model `hand_landmarker.task` cannot be downloaded
   automatically. The `resources/models/` directory is created with a
   `.gitkeep` and a `README.txt` containing the manual download command.
   When the file is present, `HandDetector::Init` will load it. The stub
   `HandLandmarkerWrapper` does not actually load the model, so the file
   does not need to exist for the library to compile.

### What to do when network is available

1. Download the model file:
   ```
   mkdir -p resources/models
   curl -L -o resources/models/hand_landmarker.task \
     https://storage.googleapis.com/mediapipe-models/hand_landmarker/hand_landmarker/float16/latest/hand_landmarker.task
   ```
2. Re-run cmake configure so FetchContent can populate
   `build/_deps/mediapipe_tasks-src/`.
3. Replace the stub `HandLandmarkerWrapper` body in
   `src/inference/HandDetector.cpp` with the real
   `HandLandmarker_Create` / `HandLandmarker_DetectImage` / `HandLandmarker_Close`
   calls, gated on the presence of the MediaPipe header.

### Verification performed for Task 8

- Standalone MSVC parse-check (`cl /EHa /std:c++20 /I src /I src/util /I src/capture ...`)
  on the three new translation units to confirm they compile in isolation.
  See the commit message and the "Standalone parse-check" subsection of
  the task report for the exact command and observed output.
