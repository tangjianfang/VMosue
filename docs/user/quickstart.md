# Quickstart

This guide walks you from a fresh download to a working VMosue
installation. It should take about ten minutes.

## System requirements

| | |
|---|---|
| Operating system | Windows 10 (version 1903 or later) or Windows 11 |
| Architecture | x64 (Intel or AMD 64-bit) |
| Webcam | Any USB or built-in camera that Windows recognizes as a video capture device |
| Free disk space | About 200 MB |
| Lighting | A well-lit room — window light or a desk lamp facing you |
| Hand visibility | Short sleeves, no gloves, palms uncovered |

VMosue does not require a GPU. It runs on the CPU only and is designed
to leave the system responsive while you use it.

> **Note:** ARM-based Windows devices (Snapdragon X, etc.) are not
> currently supported. If you have an ARM Windows device, please open
> an issue.

## Install

1. Download the latest installer from the
   [GitHub Releases page](https://github.com/.../releases). The file is
   named `VMosue-Setup-1.0.0.exe` (the version number will change with
   each release).
2. Run the installer.
3. If Windows asks **"Do you want to allow this app to make changes?"**,
   click **Yes**. This is the standard User Account Control prompt.
4. Choose an install location. The default is
   `C:\Program Files\VMosue` — leave it unchanged unless you have a
   reason to change it.
5. Click **Install** and wait for the progress bar to finish.
6. Click **Finish**. VMosue launches automatically.

You can also launch VMosue later from the **Start Menu** (search for
"VMosue") or the **Desktop** shortcut.

## First-time setup

When VMosue starts for the first time:

1. A tray icon appears in the system tray (the area near the clock).
   Look for a small VMosue icon. If you do not see it, click the
   **^** arrow to expand the hidden tray icons.
2. **Right-click** the tray icon to open the menu.
3. Click **Settings**.
4. In the **Camera** dropdown, select the webcam you want to use. If
   the dropdown is empty, see
   [Troubleshooting — Camera not detected](troubleshooting.md#camera-not-detected).
5. (Optional but recommended.) Click **Run calibration**. Follow the
   on-screen instructions — it takes about a minute and improves
   tracking accuracy for your environment.
6. Adjust the **Sensitivity** slider to taste. Higher values move the
   cursor faster; lower values are smoother.
7. Click **Save**.
8. Try the tutorial: tray icon → **Tutorial**. It walks you through
   every gesture.

```
   +---------------------------------+
   |  VMosue v1.0.0                  |
   +---------------------------------+
   |                                 |
   |   Camera:    [ Integrated Webcam v]
   |   Sensitivity: [====o----] 0.5   |
   |                                 |
   |   [ Run calibration ]           |
   |                                 |
   |   [ Save ]   [ Cancel ]         |
   +---------------------------------+
```

## Camera positioning

Good camera positioning is the single biggest factor in tracking
quality. Follow these guidelines:

- **Top of the webcam at eye level.** If your laptop's built-in
  camera is below the screen, raise the laptop on a stand or stack of
  books.
- **Arm's length from your hand.** Sit at your normal distance from
  the screen, then rest your dominant hand on the desk in front of
  you. The camera should see your full hand and wrist.
- **Avoid back-lighting.** A window or bright lamp *behind* you
  turns your hand into a silhouette. Face the window instead, or
  close the blinds.
- **Plain background helps.** A cluttered background makes it
  harder for the tracker to isolate your hand. A wall, monitor, or
  closed curtain works well.

```
    screen
     +---+
     |   |
     +---+
       ^
       |   arm's length
       v
       ___        ___
      |   |      |cam|   <-- top of camera at eye level
      |   |      |___|
      | hand|
      |_____|
       user
```

## Where to find logs

If something goes wrong, the log file is the first place to look.

- **Folder:** `%LOCALAPPDATA%\VMosue\logs\`
  (for example `C:\Users\<you>\AppData\Local\VMosue\logs\`)
- **Latest log:** `vmosue.log` — plain text, rotated daily.
- **Live view:** **Settings → Debug → Live log viewer** shows the last
  few hundred log lines without leaving the app.
- **Settings file:** `%LOCALAPPDATA%\VMosue\config.json` — your saved
  preferences. Delete it to reset VMosue to defaults.

When reporting a bug, attach the most recent `vmosue.log` — see
[Troubleshooting — Where to report bugs](troubleshooting.md#where-to-report-bugs).