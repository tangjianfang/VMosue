; installer/nsi/variables.nsh
;
; Shared NSIS preprocessor variables for the VMosue installer.
; Sourced by both VMosue.nsi (the installer itself) and any helpers
; that need to reuse the same constants (e.g. localized message files).
;
; Keep this file in sync with:
;   - vcpkg.json                  (package version)
;   - CMakeLists.txt              (CMake project version, if added)
;   - README.md                   (release notes)
;
; Bumping APP_VERSION should be the only edit required to roll a new
; release; the GitHub Actions release workflow reads these indirectly
; via the OutFile pattern "VMosue-Setup-${APP_VERSION}.exe".

!define APP_NAME      "VMosue"
!define APP_PUBLISHER "VMosue"
!define APP_VERSION   "1.0.0"

; Windows registry keys (uninstall metadata + auto-start) all live
; under HKCU so the installer does not require admin elevation.
!define APP_UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}"

; OutFile is computed from APP_VERSION so a single .nsi drives all
; patch releases without edits to the main script.
!define OUTPUT "VMosue-Setup-${APP_VERSION}.exe"