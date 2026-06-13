; installer/nsi/VMosue.nsi
;
; NSIS 3.x installer for VMosue (Windows 10/11 gesture mouse).
;
; Inputs
; ------
;   - installer/nsi/variables.nsh   : APP_NAME, APP_VERSION, OUTPUT, APP_UNINST_KEY
;   - build/bin/Release/*           : compiled application tree (vmosue.exe
;                                     and any runtime resources / DLLs copied
;                                     by CMake's install rules or post-build
;                                     steps). Must exist before makensis runs.
;
; Build
; -----
;   makensis /WX installer/nsi/VMosue.nsi
;
; The /WX flag promotes warnings to errors so syntactic drift gets caught
; in CI rather than at install time on a customer machine.
;
; This script uses the old-style NSIS UI on purpose: it has zero external
; MUI dependencies (no MUI2.nsh / nshsplash / etc.), which keeps the build
; hermetic across CI runners that do not ship the NSIS Modern UI contrib.
; Swap to MUI2.nsh later if/when we add a real license file and want a
; branded welcome/finish page.

!include "variables.nsh"

Name "${APP_NAME} ${APP_VERSION}"
OutFile "${OUTPUT}"
InstallDir "$PROGRAMFILES64\${APP_NAME}"
RequestExecutionLevel user
ShowInstDetails show
ShowUninstDetails show

; Footer text shown at the bottom of every installer page.
BrandingText "${APP_NAME} v${APP_VERSION}"

; ----------------------------------------------------------------------------
; Install
; ----------------------------------------------------------------------------
Section "Install"
  SectionIn RO  ; always selected, cannot be unchecked

  SetOutPath "$INSTDIR"

  ; The release workflow is responsible for having populated
  ; build/bin/Release with vmosue.exe + runtime resources. We copy the
  ; whole tree so non-exe assets (e.g. *.tflite, *.pak) come along too.
  File /r "build\bin\Release\*"

  ; Drop the uninstaller so Add/Remove Programs and the Start Menu
  ; entry have something to invoke.
  WriteUninstaller "$INSTDIR\uninstall.exe"

  ; Uninstall metadata. HKCU keeps us non-elevated; the spec mandates
  ; RequestExecutionLevel user, so writing to HKLM here would silently
  ; fail in a non-admin shell.
  WriteRegStr HKCU "${APP_UNINST_KEY}" "DisplayName"     "${APP_NAME}"
  WriteRegStr HKCU "${APP_UNINST_KEY}" "DisplayVersion"  "${APP_VERSION}"
  WriteRegStr HKCU "${APP_UNINST_KEY}" "Publisher"       "${APP_PUBLISHER}"
  WriteRegStr HKCU "${APP_UNINST_KEY}" "UninstallString" "$INSTDIR\uninstall.exe"
  WriteRegStr HKCU "${APP_UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${APP_UNINST_KEY}" "DisplayIcon"     "$INSTDIR\vmosue.exe,0"
  WriteRegDWORD HKCU "${APP_UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${APP_UNINST_KEY}" "NoRepair" 1

  ; Desktop + Start Menu shortcuts. Both may fail silently in a locked-
  ; down corporate environment; we ignore the return code so the install
  ; still completes.
  CreateShortcut "$DESKTOP\${APP_NAME}.lnk" "$INSTDIR\vmosue.exe"
  CreateDirectory "$SMPROGRAMS\${APP_NAME}"
  CreateShortcut "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk" "$INSTDIR\vmosue.exe"
SectionEnd

; ----------------------------------------------------------------------------
; Uninstall
; ----------------------------------------------------------------------------
Section "Uninstall"
  ; Remove the install directory. /r recurses; $INSTDIR only contains
  ; files we created in the Install section, so nuking it is safe.
  RMDir /r "$INSTDIR"

  DeleteRegKey HKCU "${APP_UNINST_KEY}"

  Delete "$DESKTOP\${APP_NAME}.lnk"
  RMDir /r "$SMPROGRAMS\${APP_NAME}"
SectionEnd