# Inference module: hand detection (MediaPipe Tasks) + landmark smoothing.
#
# MediaPipe Tasks C++ is fetched by CMake's FetchContent. Note: as of 2026-06-14
# the project's development environment cannot reach github.com / Google Cloud
# Storage, so the FetchContent step will fail in offline builds. We guard it
# with a VMOSUE_FETCH_MEDIAPIPE option (default ON) so CI / dev machines with
# network access can still build, and offline builds can disable it.
#
# Alternative considered: vcpkg port "mediapipe" — not available upstream as
# of this writing, so FetchContent is the only feasible path. See
# docs/build-notes.md for the full rationale.
include(FetchContent)

option(VMOSUE_FETCH_MEDIAPIPE "Fetch MediaPipe Tasks C++ source via FetchContent" ON)

if(VMOSUE_FETCH_MEDIAPIPE)
  FetchContent_Declare(
    mediapipe_tasks
    GIT_REPOSITORY https://github.com/google-ai-edge/mediapipe-samples.git
    GIT_TAG        main
    SOURCE_SUBDIR  examples/gesture_recognizer/cpp
    GIT_SHALLOW    TRUE
  )
  set(MEDIAPIPE_TASKS_GPU_DELEGATE OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(mediapipe_tasks)
endif()

add_library(vmosue_inference STATIC
  HandDetector.h
  HandDetector.cpp
  LandmarkSmoother.h
)
target_include_directories(vmosue_inference PUBLIC
  ${CMAKE_SOURCE_DIR}/src
)
if(VMOSUE_FETCH_MEDIAPIPE)
  target_include_directories(vmosue_inference PUBLIC
    ${mediapipe_tasks_SOURCE_DIR}
  )
  target_link_libraries(vmosue_inference PUBLIC mediapipe_tasks)
endif()
target_link_libraries(vmosue_inference PUBLIC vmosue_util)
