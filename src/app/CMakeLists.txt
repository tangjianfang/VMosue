add_executable(vmosue WIN32
  main.cpp
  App.h
  App.cpp
  Watchdog.h
  Watchdog.cpp
)
target_link_libraries(vmosue PRIVATE
  vmosue_util
  vmosue_capture
  vmosue_inference
  vmosue_gesture
  vmosue_input
  vmosue_ui
  vmosue_platform
  vmosue_config
  vmosue_runtime_data
  Threads::Threads
  Boost::boost
)
