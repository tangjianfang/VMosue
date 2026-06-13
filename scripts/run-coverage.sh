#!/usr/bin/env bash
# Run the test suite with gcov coverage instrumentation and roll the
# results up into coverage.xml via gcovr. Requires a GCC or Clang
# toolchain (--coverage is not supported on MSVC -- see the
# VMOSUE_COVERAGE option in the root CMakeLists.txt for the Windows
# alternative).
#
# Usage:
#   scripts/run-coverage.sh
#
# Output:
#   build/coverage.xml  -- gcovr Cobertura XML, ready for CI upload
#   build/coverage.html -- human-readable HTML report
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"

cmake -B "$BUILD" -S "$ROOT" \
  -DVMOSUE_COVERAGE=ON \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER="${CXX:-g++}"

cmake --build "$BUILD" --config Debug -j

"$BUILD/bin/vmosue_tests"  # gtest_discover_tests registers, but run the binary too

if ! command -v gcovr >/dev/null 2>&1; then
  echo "gcovr not found. Install with: pip install gcovr" >&2
  exit 1
fi

gcovr --filter src/ --xml "$BUILD/coverage.xml"
gcovr --filter src/ --html-details "$BUILD/coverage.html"

echo
echo "Coverage report written to:"
echo "  $BUILD/coverage.xml  (CI / Cobertura)"
echo "  $BUILD/coverage.html (human-readable)"