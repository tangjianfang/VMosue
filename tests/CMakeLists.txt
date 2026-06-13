add_subdirectory(unit)

add_executable(vmosue_tests
  unit/test_result.cpp
  unit/test_one_euro_filter.cpp
  unit/test_cursor_controller.cpp
  unit/test_click_detector.cpp
  unit/test_air_click_detector.cpp
  unit/test_input_injector.cpp
  unit/test_state_machine.cpp
)
target_link_libraries(vmosue_tests PRIVATE
  vmosue_util
  vmosue_gesture
  vmosue_input
  GTest::gtest_main
)
include(GoogleTest)
gtest_discover_tests(vmosue_tests)
