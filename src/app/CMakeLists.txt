add_executable(vmosue WIN32
  main.cpp
  App.h
  App.cpp
)
target_link_libraries(vmosue PRIVATE
  vmosue_util
  vmosue_capture
  vmosue_inference
  vmosue_gesture
  vmosue_input
  Threads::Threads
  Boost::boost
)

# NOTE: This list intentionally omits vmosue_ui, vmosue_config, and
# vmosue_platform. They are forward references to Tasks 15, 19, 24, 25,
# etc. which have not landed yet. Add them to the link list as those
# tasks land.
