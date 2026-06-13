# VMosue Installer

NSIS-based Windows installer for VMosue. Produces a single
self-contained `VMosue-Setup-1.0.0.exe` that drops a `vmosue.exe`
plus runtime assets into `C:\Program Files\VMosue` and registers
uninstall metadata under `HKCU`.

## Files

```
installer/
  nsi/
    VMosue.nsi       ; main installer script
    variables.nsh    ; shared NSIS preprocessor defines
```

## Local build

Requires **NSIS 3.x** (`makensis` on PATH).

```powershell
# 1. Build the application tree (Release).
cmake --build build --config Release

# 2. Compile the installer.
makensis /WX installer/nsi/VMosue.nsi
```

`/WX` promotes warnings to errors so syntactic drift gets caught at
build time. The installer output is `VMosue-Setup-1.0.0.exe` in the
repository root (matching the `${OUTPUT}` define in
`variables.nsh`).

## CI build

`.github/workflows/release.yml` runs the same commands on
`windows-2022` runners:

1. Configure & build with CMake (Release).
2. `makensis /WX installer/nsi/VMosue.nsi`.
3. Verify `VMosue-Setup-*.exe` exists.
4. Upload as the `vmosue-installer` artifact.

Pushing a tag matching `v*.*.*` additionally publishes the
installer as a GitHub Release asset via
`softprops/action-gh-release@v2`.

## Uninstall

The installed `uninstall.exe` removes the install directory, the
uninstall registry key, and the desktop / Start Menu shortcuts.

## Upgrading the version

Bump `APP_VERSION` in `installer/nsi/variables.nsh`. The installer
file name and uninstall registry `DisplayVersion` both derive from
this single constant — no other edits required.

## Known limitations

- Uses the legacy NSIS UI (no MUI2 welcome / finish page). Swap to
  `MUI2.nsh` once a LICENSE file is bundled with the installer.
- Installs per-user only (`RequestExecutionLevel user`). All
  registry writes go to `HKCU` so no admin elevation is required.
- No `vc_redist` bootstrap; the runtime is expected to be present
  on Windows 10/11.