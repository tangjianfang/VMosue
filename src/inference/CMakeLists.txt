# Inference module: hand detection (MediaPipe Hands via Python IPC) + landmark smoothing.
#
# v0.3: the C++ HandDetector spawns `python scripts/hand_detector_server.py`
# as a subprocess and exchanges frames / JSON landmarks over stdin/stdout
# pipes. This gives the C++ parent real, commercial-grade MediaPipe Hands
# detection (the upstream `google-ai-edge/mediapipe-samples` repo no
# longer ships a desktop C++ example, and the main `google/mediapipe`
# repo has no Windows prebuilts for the C++ Tasks runtime — building
# from source is multi-hour and pulls tens of GB of dependencies).
#
# The Python side is a pure dependency-light script that imports the
# official `mediapipe` PyPI package and drives HandLandmarker. The
# hand_landmarker.task model lives at resources/models/hand_landmarker.task
# (downloaded from storage.googleapis.com during environment setup).
#
# Alternatives considered and rejected (see docs/build-notes.md):
#   - CMake FetchContent of mediapipe-samples: target path was deleted
#     upstream; doesn't exist anymore.
#   - vcpkg "mediapipe" port: not in the catalog as of 2026-06.
#   - Direct build of google/mediapipe C++: needs Bazel + 30+ system
#     deps; multi-hour build; not viable on Windows MSVC.
#   - ONNX Runtime + MediaPipe-as-ONNX: requires a hand landmark ONNX
#     model that doesn't ship in the official MediaPipe model zoo
#     (only TFLite .task files are provided). Deferrable to v0.4.
#
# Requirement: the deployed vmosue.exe must find `python` on PATH and
# the scripts\ + resources\models\ directories alongside the exe.
# The build system copies these next to the binary via the standard
# install rules in the root CMakeLists.

add_library(vmosue_inference STATIC
  HandDetector.h
  HandDetector.cpp
  LandmarkSmoother.h
)
target_include_directories(vmosue_inference PUBLIC
  ${CMAKE_SOURCE_DIR}/src
)
# nlohmann_json is used by HandDetector.cpp to serialize the per-frame
# JSON metadata line and parse the Python subprocess's JSON response.
find_package(nlohmann_json CONFIG REQUIRED)
target_link_libraries(vmosue_inference PUBLIC vmosue_util nlohmann_json::nlohmann_json)