add_subdirectory(unit)

add_executable(vmosue_tests
  unit/test_result.cpp
  unit/test_one_euro_filter.cpp
)
target_link_libraries(vmosue_tests PRIVATE
  vmosue_util
  GTest::gtest_main
)
include(GoogleTest)
gtest_discover_tests(vmosue_tests)