@echo off
REM ============================================================================
REM build.bat - one-shot build for VMosue
REM ----------------------------------------------------------------------------
REM Run from the repo root:
REM     build.bat            (builds Release vmosue.exe + runs all tests)
REM     build.bat tests      (builds and runs only vmosue_tests)
REM     build.bat clean      (wipes build/ and reconfigures from scratch)
REM
REM Handles the three things that bit us before:
REM   1. CMake picks a VS instance that has the C++ ATL component
REM      (BuildTools often does NOT; CameraCapture.h needs <atlbase.h>).
REM   2. The vcpkg checkout is at the baseline pinned in vcpkg.json
REM      (commit 58613d0); without this, ports listed there fail to load.
REM   3. The hand_landmarker.task model is downloaded so CMake's resource
REM      sync step doesn't warn / produce a non-runnable bin tree.
REM ============================================================================
setlocal enabledelayedexpansion

set "REPO_ROOT=%~dp0"
if "%REPO_ROOT:~-1%"=="\" set "REPO_ROOT=%REPO_ROOT:~0,-1%"
cd /d "%REPO_ROOT%"

REM Reassign %ProgramFiles(x86)% to a name without parentheses. The
REM literal parentheses in the variable name break expansion when the
REM string is embedded inside a parenthesized `for ... do ( ... )` block
REM — the inner `)` terminates the `for` body prematurely. Capture it
REM into a paren-free name once and reference that everywhere below.
set "PF=%ProgramFiles%"
set "PFX86=%ProgramFiles(x86)%"

set "MODE=%~1"
if "%MODE%"=="" set "MODE=all"

REM ---------------------------------------------------------------------------
REM 1. Locate a VS instance that has C++ ATL.
REM ---------------------------------------------------------------------------
echo [1/6] Locating Visual Studio with C++ ATL...

REM Walk every candidate VS root and look for an MSVC toolset that
REM ships ATL. CMD's `if exist` cannot wildcard a path component, so
REM we use `for /d` to enumerate the MSVC version subdirectories.
REM Last-write-wins: we don't short-circuit on the first match (the
REM `if not defined` short-circuit interacts badly with delayed
REM expansion inside nested for); instead we let the loop run and
REM rely on every match overwriting with the same value.
set "VS_INSTANCE="
for %%R in ("%PF%\Microsoft Visual Studio\2022" "%PFX86%\Microsoft Visual Studio\2022") do (
  for %%E in (Community Professional Enterprise) do (
    if exist "%%~R\%%E\VC\Tools\MSVC" (
      for /d %%T in ("%%~R\%%E\VC\Tools\MSVC\*") do (
        if exist "%%T\atlmfc\include\atlbase.h" set "VS_INSTANCE=%%~R\%%E"
      )
    )
  )
)

if not defined VS_INSTANCE goto :no_atl
echo     Using: %VS_INSTANCE%
goto :have_atl
:no_atl
echo.
echo ERROR: No VS2022 instance with C++ ATL found.
echo.
echo The capture module includes ^<atlbase.h^>, which ships with the
echo "C++ ATL for latest v143 build tools" component. Open the Visual
echo Studio Installer, modify your VS2022 install Community/Pro/Ent,
echo and add that component. BuildTools alone is not enough.
exit /b 2
:have_atl

REM ---------------------------------------------------------------------------
REM 2. Ensure VCPKG_ROOT exists and is at the project's baseline commit.
REM ---------------------------------------------------------------------------
echo [2/6] Checking vcpkg...

if not defined VCPKG_ROOT set "VCPKG_ROOT=%USERPROFILE%\vcpkg"

if not exist "%VCPKG_ROOT%\vcpkg.exe" (
  if not exist "%VCPKG_ROOT%\.git" (
    echo     vcpkg not found at %VCPKG_ROOT%; cloning...
    git clone https://github.com/microsoft/vcpkg "%VCPKG_ROOT%"
    if errorlevel 1 (
      echo ERROR: vcpkg clone failed. Check your internet connection.
      exit /b 3
    )
    call "%VCPKG_ROOT%\bootstrap-vcpkg.bat" -disableMetrics
    if errorlevel 1 (
      echo ERROR: vcpkg bootstrap failed.
      exit /b 3
    )
  )
)

REM Read the baseline commit from vcpkg.json so this script does not
REM need to be edited when the project bumps it.
set "BASELINE="
for /f "usebackq tokens=2 delims=:," %%L in (`findstr /c:"builtin-baseline" vcpkg.json`) do (
  set "BASELINE=%%L"
)
if defined BASELINE (
  REM Strip surrounding spaces and quotes.
  set "BASELINE=!BASELINE: =!"
  set "BASELINE=!BASELINE:"=!"
)

if defined BASELINE (
  pushd "%VCPKG_ROOT%" >nul
  git rev-parse --quiet --verify "!BASELINE!^{commit}" >nul 2>&1
  if errorlevel 1 (
    echo     Fetching vcpkg baseline !BASELINE!...
    git fetch --quiet origin
  )
  for /f %%H in ('git rev-parse --short HEAD') do set "VCPKG_HEAD=%%H"
  if /i not "!VCPKG_HEAD!"=="!BASELINE:~0,7!" (
    echo     vcpkg currently at !VCPKG_HEAD!; checking out !BASELINE:~0,12!...
    git checkout --quiet "!BASELINE!"
    if errorlevel 1 (
      popd >nul
      echo ERROR: vcpkg checkout to baseline failed.
      exit /b 3
    )
  )
  popd >nul
)
echo     vcpkg ready at %VCPKG_ROOT%

REM ---------------------------------------------------------------------------
REM 3. Prepare runtime resources (model + python deps).
REM ---------------------------------------------------------------------------
echo [3/6] Preparing runtime resources...
where powershell >nul 2>&1
if errorlevel 1 goto :no_powershell
powershell -NoProfile -ExecutionPolicy Bypass -File "%REPO_ROOT%\scripts\prepare-resources.ps1" -SkipPython
REM Non-fatal: a missing model warns and continues.
goto :resources_done
:no_powershell
echo     WARNING: powershell not on PATH; skipping prepare-resources.ps1
echo     The build will still produce vmosue.exe but the model file
echo     resources/models/hand_landmarker.task may be missing at runtime.
:resources_done

REM ---------------------------------------------------------------------------
REM 4. Optional clean.
REM ---------------------------------------------------------------------------
if /i "%MODE%"=="clean" (
  echo [4/6] Cleaning build/ ...
  if exist build rmdir /s /q build
  REM Force a reconfigure on the next normal run by falling through.
  set "MODE=all"
)

REM ---------------------------------------------------------------------------
REM 5. Configure (if needed).
REM ---------------------------------------------------------------------------
if exist build\CMakeCache.txt goto :skip_configure
echo [5/6] Configuring with CMake (Visual Studio 17 2022 generator)...
cmake -B build -S . -G "Visual Studio 17 2022" -A x64 -DCMAKE_GENERATOR_INSTANCE="%VS_INSTANCE%" -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 (
  echo.
  echo ERROR: CMake configure failed. See output above.
  echo Common cause: vcpkg dependency resolution. Run "build.bat clean"
  echo and ensure your vcpkg checkout matches vcpkg.json's builtin-baseline.
  exit /b 4
)
goto :after_configure
:skip_configure
echo [5/6] Reusing existing build/ configuration.
:after_configure

REM ---------------------------------------------------------------------------
REM 6. Build + test.
REM ---------------------------------------------------------------------------
if /i "%MODE%"=="tests" (
  echo [6/6] Building vmosue_tests only...
  cmake --build build --config Release --target vmosue_tests --parallel
  if errorlevel 1 ( echo ERROR: tests build failed. & exit /b 5 )
  ctest --test-dir build -C Release --output-on-failure
  if errorlevel 1 ( echo ERROR: tests failed. & exit /b 6 )
  echo.
  echo BUILD OK ^(tests only^)
  exit /b 0
)

echo [6/6] Building vmosue.exe + vmosue_tests ...
cmake --build build --config Release --parallel
if errorlevel 1 (
  echo ERROR: build failed. See output above.
  exit /b 5
)

echo.
echo Running test suite...
ctest --test-dir build -C Release --output-on-failure
if errorlevel 1 (
  echo ERROR: one or more tests failed.
  exit /b 6
)

echo.
echo ============================================================================
echo BUILD OK
echo     vmosue.exe   : %REPO_ROOT%\build\bin\Release\vmosue.exe
echo     vmosue_tests : %REPO_ROOT%\build\bin\Release\vmosue_tests.exe
echo Run the app:           build\bin\Release\vmosue.exe
echo Build the installer:   package.bat
echo ============================================================================
exit /b 0
