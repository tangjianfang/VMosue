add_subdirectory(unit)

# Make the fixtures dir absolute so we can hand it to the integration
# test via a compile definition. The path is resolved relative to the
# current source dir (tests/), so it works for both in-source and
# out-of-source builds.
get_filename_component(VMOSUE_TEST_FIXTURES_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/fixtures" ABSOLUTE)

add_executable(vmosue_tests
  unit/test_result.cpp
  unit/test_one_euro_filter.cpp
  unit/test_cursor_controller.cpp
  unit/test_click_detector.cpp
  unit/test_air_click_detector.cpp
  unit/test_scroll_detector.cpp
  unit/test_pause_detector.cpp
  unit/test_input_injector.cpp
  unit/test_state_machine.cpp
  unit/test_calibration.cpp
  unit/test_config.cpp
  unit/test_watchdog.cpp
  unit/test_display_info.cpp
  unit/test_frame_resampler.cpp
  unit/test_profile_guard.cpp
  unit/test_frame_convert.cpp
  unit/test_hand_skeleton.cpp
  unit/test_overlay_geometry.cpp
  unit/test_adaptive.cpp
  integration/test_pipeline_e2e.cpp
  # Watchdog is a standalone utility compiled into the vmosue
  # executable (it lives in src/app/ alongside App.cpp). Since
  # vmosue_tests does not link to the vmosue executable, we compile
  # Watchdog.cpp into the test binary directly so the watchdog
  # unit tests can link against it.
  ${CMAKE_SOURCE_DIR}/src/app/Watchdog.cpp
)
target_link_libraries(vmosue_tests PRIVATE
  vmosue_util
  vmosue_gesture
  vmosue_input
  vmosue_platform
  vmosue_config
  GTest::gtest_main
)
# The integration test loads tests/fixtures/sample_landmarks.json at
# runtime. Hand the absolute path in via a compile definition so the
# test does not have to guess where it lives.
target_compile_definitions(vmosue_tests PRIVATE
  VMOSUE_TEST_FIXTURES_DIR="${VMOSUE_TEST_FIXTURES_DIR}")
# Make the fixtures dir reachable from the test binary's CWD (most
# build trees run tests from the binary directory).
target_include_directories(vmosue_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/integration)
include(GoogleTest)
gtest_discover_tests(vmosue_tests)