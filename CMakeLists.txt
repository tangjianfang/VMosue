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