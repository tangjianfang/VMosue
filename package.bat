@echo off
REM ============================================================================
REM package.bat - produce a distributable VMosue artifact
REM ----------------------------------------------------------------------------
REM Run from the repo root. Produces (in dist/):
REM   - VMosue-<version>-win-x64.zip          (always)
REM   - VMosue-Setup-<version>.exe            (if NSIS is installed)
REM
REM Both contain the full runtime tree: vmosue.exe + scripts/ + resources/ +
REM the spdlog/fmt DLLs already deployed by CMake into build\bin\Release.
REM
REM Pre-requisite: build.bat must have produced build\bin\Release\vmosue.exe.
REM This script will run build.bat for you if the binary is missing.
REM ============================================================================
setlocal enabledelayedexpansion

set "REPO_ROOT=%~dp0"
if "%REPO_ROOT:~-1%"=="\" set "REPO_ROOT=%REPO_ROOT:~0,-1%"
cd /d "%REPO_ROOT%"

REM Capture %ProgramFiles(x86)% into a paren-free name; embedding
REM the literal `(x86)` inside parenthesized blocks below would break
REM cmd's parser.
set "PF=%ProgramFiles%"
set "PFX86=%ProgramFiles(x86)%"

set "BIN_DIR=%REPO_ROOT%\build\bin\Release"
set "DIST_DIR=%REPO_ROOT%\dist"
set "STAGE_DIR=%DIST_DIR%\stage"

REM ---------------------------------------------------------------------------
REM 1. Ensure the binary exists; build it if not.
REM ---------------------------------------------------------------------------
if not exist "%BIN_DIR%\vmosue.exe" (
  echo [1/5] vmosue.exe not found; running build.bat first...
  call "%REPO_ROOT%\build.bat"
  if errorlevel 1 (
    echo ERROR: build.bat failed; cannot package.
    exit /b 1
  )
) else (
  echo [1/5] Reusing existing vmosue.exe at %BIN_DIR%\vmosue.exe
)

REM ---------------------------------------------------------------------------
REM 2. Read APP_VERSION from installer/nsi/variables.nsh so a single edit
REM    drives both the zip name and the NSIS output name.
REM ---------------------------------------------------------------------------
set "APP_VERSION="
for /f "usebackq tokens=3" %%V in (`findstr /c:"!define APP_VERSION" installer\nsi\variables.nsh`) do (
  set "APP_VERSION=%%V"
)
if defined APP_VERSION (
  set "APP_VERSION=!APP_VERSION:"=!"
)
if not defined APP_VERSION (
  echo ERROR: could not parse APP_VERSION from installer\nsi\variables.nsh
  exit /b 2
)
echo [2/5] APP_VERSION = !APP_VERSION!

REM ---------------------------------------------------------------------------
REM 3. Stage the runtime tree.
REM ---------------------------------------------------------------------------
echo [3/5] Staging runtime tree to %STAGE_DIR% ...
if exist "%STAGE_DIR%" rmdir /s /q "%STAGE_DIR%"
mkdir "%STAGE_DIR%" >nul

REM CMake puts the compiled binaries under build\bin\Release\ but the
REM custom resource-sync target writes scripts\ and resources\ into
REM build\bin\ directly (one level up). We need both: copy the Release
REM tree (excluding the test binary) AND mirror scripts\ + resources\
REM from %REPO_ROOT% so a fresh clone with no resource sync still
REM produces a runnable bundle.
REM
REM robocopy returns 0..7 for success (with various "what was copied"
REM bits). Only exit codes >= 8 mean a real failure.
robocopy "%BIN_DIR%" "%STAGE_DIR%" /e /xf vmosue_tests.exe gtest.dll gtest_main.dll /njh /njs /ndl /nfl /nc /ns >nul
if errorlevel 8 (
  echo ERROR: staging copy of build\bin\Release failed.
  exit /b 3
)
robocopy "%REPO_ROOT%\scripts" "%STAGE_DIR%\scripts" /e /njh /njs /ndl /nfl /nc /ns >nul
if errorlevel 8 (
  echo ERROR: staging copy of scripts\ failed.
  exit /b 3
)
robocopy "%REPO_ROOT%\resources" "%STAGE_DIR%\resources" /e /xf .gitkeep README.txt /njh /njs /ndl /nfl /nc /ns >nul
if errorlevel 8 (
  echo ERROR: staging copy of resources\ failed.
  exit /b 3
)

REM Drop a one-line README at the root of the stage so a user who
REM unzips the archive knows what to do.
> "%STAGE_DIR%\README.txt" echo VMosue !APP_VERSION!  -  run vmosue.exe to start.
>> "%STAGE_DIR%\README.txt" echo Logs are written to %%LOCALAPPDATA%%\VMosue\logs\
>> "%STAGE_DIR%\README.txt" echo See https://github.com/tangjianfang/VMosue for documentation.

REM ---------------------------------------------------------------------------
REM 4. Make the portable ZIP.
REM ---------------------------------------------------------------------------
set "ZIP_NAME=VMosue-!APP_VERSION!-win-x64.zip"
set "ZIP_PATH=%DIST_DIR%\%ZIP_NAME%"
echo [4/5] Building portable zip: %ZIP_NAME% ...
if exist "%ZIP_PATH%" del "%ZIP_PATH%" >nul

REM Use PowerShell's Compress-Archive: ships in every Windows 10/11 box,
REM no choco / 7z dependency. -Force overwrites, -CompressionLevel Optimal
REM trades a few extra seconds for a noticeably smaller artifact.
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "Compress-Archive -Path '%STAGE_DIR%\*' -DestinationPath '%ZIP_PATH%' -CompressionLevel Optimal -Force"
if errorlevel 1 (
  echo ERROR: zip creation failed.
  exit /b 4
)
for %%S in ("%ZIP_PATH%") do set "ZIP_BYTES=%%~zS"
echo     wrote %ZIP_PATH% ^(!ZIP_BYTES! bytes^)

REM ---------------------------------------------------------------------------
REM 5. Build the NSIS installer if makensis is on PATH.
REM ---------------------------------------------------------------------------
echo [5/5] Looking for NSIS makensis ...
set "MAKENSIS="
where makensis >nul 2>&1 && set "MAKENSIS=makensis"
if not defined MAKENSIS if exist "%PFX86%\NSIS\makensis.exe" set "MAKENSIS=%PFX86%\NSIS\makensis.exe"
if not defined MAKENSIS if exist "%PF%\NSIS\makensis.exe" set "MAKENSIS=%PF%\NSIS\makensis.exe"

if not defined MAKENSIS (
  echo     NSIS not found. Skipping installer build.
  echo     To produce VMosue-Setup-!APP_VERSION!.exe, install NSIS 3.x from
  echo     https://nsis.sourceforge.io/Download and re-run package.bat
  echo     ^(or use the portable zip at %ZIP_PATH%^).
  goto :summary
)

echo     using %MAKENSIS%
"%MAKENSIS%" /WX installer\nsi\VMosue.nsi
if errorlevel 1 (
  echo ERROR: NSIS build failed.
  exit /b 5
)

REM The .nsi writes to the repo root; move it under dist/.
set "NSIS_OUT=%REPO_ROOT%\VMosue-Setup-!APP_VERSION!.exe"
if exist "%NSIS_OUT%" (
  move /y "%NSIS_OUT%" "%DIST_DIR%\" >nul
)

:summary
echo.
echo ============================================================================
echo PACKAGE OK
echo     %DIST_DIR%\%ZIP_NAME%
if defined MAKENSIS (
  echo     %DIST_DIR%\VMosue-Setup-!APP_VERSION!.exe
)
echo ============================================================================
exit /b 0
