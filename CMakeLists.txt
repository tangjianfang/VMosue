cmake_minimum_required(VERSION 3.20)
project(vmosue LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Use wide-character (UTF-16) Windows APIs. The code passes wide string
# literals (L"...") to APIs like CreateWindowEx / RegisterClassEx and
# stores class names in wchar_t* constants; without these defines the
# W-suffixed APIs are not active and the compiler errors on
# wchar_t* -> LPCSTR conversions.
if(WIN32)
  add_compile_definitions(UNICODE _UNICODE)
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Output to single directory for easy packaging
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

option(VMOSUE_BUILD_TESTS "Build unit tests" ON)
option(VMOSUE_COVERAGE "Enable coverage instrumentation (gcov on GCC/Clang)" OFF)

# Coverage instrumentation. The spec snippet uses --coverage (GCC/Clang)
# which links against libgcov and emits .gcno/.gcda files for gcovr to
# roll up. MSVC has no direct equivalent; instead we recommend using
# OpenCppCoverage (https://github.com/OpenCppCoverage/OpenCppCoverage)
# on Windows. The helper script lives at scripts/run-coverage.sh.
if(VMOSUE_COVERAGE)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    add_compile_options(--coverage)
    add_link_options(--coverage)
  elseif(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    message(WARNING
      "VMOSUE_COVERAGE=ON with MSVC: --coverage has no MSVC equivalent. "
      "Use OpenCppCoverage to collect coverage data on Windows; the "
      "--coverage flags will be silently ignored for the MSVC toolchain.")
  endif()
endif()

# Threads
find_package(Threads REQUIRED)

# vcpkg dependencies
find_package(spdlog REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(GTest CONFIG REQUIRED)
find_package(Boost REQUIRED COMPONENTS lockfree)

# Core library
add_subdirectory(src)

# Tests
if(VMOSUE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# v0.5: keep build/bin/scripts and build/bin/resources in sync with
# the source tree on every build. The runtime HandDetector spawns
# `python scripts/hand_detector_server.py` with the working
# directory inherited from vmosue.exe, so the script + model
# directories must sit next to the binary. An earlier v0.3 setup
# relied on a one-off manual copy that went stale when the python
# script gained new CLI arguments (the python child then failed
# with "unrecognized arguments" even though the source-tree copy
# was up to date). add_custom_command(OUTPUT <sentinel> POST_BUILD)
# is the standard CMake idiom: the sentinel .stamp file is the
# "what got built" token, and depends on the source files we want
# copied. Whenever any of those source files are newer than the
# stamp, CMake re-runs the copy and refreshes the stamp. We add
# the stamp as a dependency of the vmosue executable so a stale
# copy can never ship.
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/bin/scripts_and_resources.stamp
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_SOURCE_DIR}/scripts
          ${CMAKE_BINARY_DIR}/bin/scripts
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_SOURCE_DIR}/resources
          ${CMAKE_BINARY_DIR}/bin/resources
  COMMAND ${CMAKE_COMMAND} -E touch
          ${CMAKE_BINARY_DIR}/bin/scripts_and_resources.stamp
  DEPENDS
    ${CMAKE_SOURCE_DIR}/scripts/hand_detector_server.py
    ${CMAKE_SOURCE_DIR}/resources/models/hand_landmarker.task
  COMMENT "Syncing scripts/ and resources/ into build/bin"
  VERBATIM
)
add_custom_target(sync_runtime_data ALL
  DEPENDS ${CMAKE_BINARY_DIR}/bin/scripts_and_resources.stamp)
# Bind the sync target to the main executable's build dependency
# so incremental builds always re-check the stamp. We can't add
# it as a direct link-time dependency (it's a custom target, not
# a library) so we wire it through a small INTERFACE library.
add_library(vmosue_runtime_data INTERFACE)
add_dependencies(vmosue_runtime_data sync_runtime_data)