# Platform integration: global hotkeys, watchdog timers, multi-monitor
# helpers, and other thin wrappers over the Win32 / desktop session APIs.
# Each helper goes into this static library so the App can link them
# without dragging the entire GUI stack into non-app executables (e.g.
# CLI tools, tests).
add_library(vmosue_platform STATIC
  Hotkey.h
  Hotkey.cpp
  DisplayInfo.h
  DisplayInfo.cpp
  AutoStart.h
  AutoStart.cpp
)
target_include_directories(vmosue_platform PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(vmosue_platform PUBLIC vmosue_util)