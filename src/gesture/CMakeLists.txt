# Gesture sub-detectors: cursor, click, scroll, pause, etc.
# Each detector consumes HandLandmarks and produces an intent (cursor delta,
# click event, ...). Detectors depend on the inference module for landmark
# types and on the util module for the logger + Result<T>.
add_library(vmosue_gesture STATIC
  CursorController.h
  CursorController.cpp
)
target_include_directories(vmosue_gesture PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(vmosue_gesture PUBLIC vmosue_util vmosue_inference)
