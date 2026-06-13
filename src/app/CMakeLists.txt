add_executable(vmosue WIN32 main.cpp)
target_link_libraries(vmosue PRIVATE
  vmosue_util
  vmosue_capture
  vmosue_inference
  vmosue_gesture
  vmosue_input
  vmosue_ui
  vmosue_config
  vmosue_platform
  Threads::Threads
)