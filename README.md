# VMosue

A Windows 10/11 native gesture-controlled mouse application. Move the
cursor and click with hand gestures captured by your webcam — no
gloves, no controllers, no special hardware.

```
   screen
    +---+
    |   |
    +---+
      ^
      |  arm's length
      v
      ___        ___
     |   |      |cam|
     |   |      |___|
     | hand|
     |_____|
      user
```

> **Status:** v1.0.0 released (2026-06-14); active development continues
> on `main` (see the [Changelog](CHANGELOG.md) `[Unreleased]` section).
> Despite the 1.0 version number this is still early software — expect
> rough edges, especially in unusual lighting or with unusual cameras.
> See [docs/superpowers/specs/](docs/superpowers/specs/) for the design
> and [docs/superpowers/plans/](docs/superpowers/plans/) for the
> implementation plan.

## Installation

1. Download the latest installer from the
   [GitHub Releases page](https://github.com/.../releases). The file
   is named `VMosue-Setup-1.0.0.exe`.
2. Run the installer, accept the User Account Control prompt, and
   click **Install**.
3. Launch from the Start Menu or Desktop shortcut.

For full install instructions (system requirements, first-time setup,
camera positioning, log locations), see the
[**Quickstart**](docs/user/quickstart.md).

> **Screenshots coming soon.** In the meantime, the
> [Tutorial](docs/user/tutorial.md) page shows the in-app first-run
> walkthrough with text diagrams.

## Documentation

### User manual

- [Quickstart](docs/user/quickstart.md) — install, first-time setup,
  camera positioning, logs.
- [Gestures](docs/user/gestures.md) — every supported gesture with
  diagrams.
- [Troubleshooting](docs/user/troubleshooting.md) — fixes for the most
  common issues.
- [Tutorial](docs/user/tutorial.md) — the in-app 6-step tutorial.

See the [user manual index](docs/user/README.md) for the full
contents.

### Developer docs

- [Build notes](docs/build-notes.md) — how to bootstrap and build from
  source.
- [Specs](docs/superpowers/specs/) — design documents.
- [Plans](docs/superpowers/plans/) — implementation plans.

## Build

```powershell
.\scripts\bootstrap.ps1            # one-time: clone + bootstrap vcpkg
.\scripts\prepare-resources.ps1    # download the ML model + install Python deps
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release
```

`prepare-resources.ps1` downloads `hand_landmarker.task` into
`resources/models/`, installs the Python dependencies
(`mediapipe`, `numpy`) listed in `requirements.txt`, and verifies the
app's runtime resources are in place. Run it before the first build so
the model is present. See [build notes](docs/build-notes.md) for the
ATL / Visual Studio component requirement.

## License

Apache-2.0. See [LICENSE](LICENSE).