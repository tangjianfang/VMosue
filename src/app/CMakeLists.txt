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
  Threads::Threads
  Boost::boost
)

# NOTE: This list intentionally omits vmosue_config. It is a forward
# reference to Task 23 which has not landed yet. Add it to the link
# list when that task lands.
