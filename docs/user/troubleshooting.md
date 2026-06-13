# Troubleshooting

This page covers the most common issues with VMosue and how to fix
them. If your problem is not listed here, see
[Where to report bugs](#where-to-report-bugs) below.

## Camera not detected

**Symptom:** the **Camera** dropdown in **Settings** is empty (or
shows "No cameras").

**Fix:**

1. Open **Settings → Camera**. Confirm the dropdown is empty.
2. Close every other app that might be using the camera:
   - Zoom, Microsoft Teams, Skype, Discord, OBS, browser tabs with
     video calls, etc.
   - Windows Camera app.
3. Unplug the webcam, wait five seconds, and plug it back in
   (built-in cameras: skip this step).
4. Right-click the VMosue tray icon → **Exit**, then relaunch
   VMosue.
5. Re-open **Settings → Camera**. The dropdown should now show your
   camera.

If the dropdown is still empty, the camera driver may not be
installed correctly. Open **Windows Settings → Bluetooth & devices →
Cameras** and verify the camera appears there. If it does not, the
issue is with the driver, not with VMosue.

## Cursor jittery / too slow

**Symptom:** the cursor shakes when your hand is still, or moves much
less than your hand does.

**Fix:**

1. **Lighting.** A dim or unevenly lit scene is the most common
   cause of jitter. Turn on a desk lamp or move so a window lights
   your hand from the front.
2. **Performance mode.** Open **Settings → Performance** and try
   **Battery** mode. It runs at a lower frame rate and uses a heavier
   smoothing filter, which trades a little latency for stability.
3. **Sensitivity.** Open **Settings → Input** and lower the
   **Sensitivity** slider. A lower value means each centimetre of
   hand movement moves the cursor fewer pixels — smoother, but
   slower.
4. **Calibration.** Run **Run calibration** in **Settings**. It
   records your room's lighting and camera angle and tunes the
   tracker for that environment. Re-run calibration if you move the
   camera, change lighting, or switch cameras.
5. **Background.** A cluttered background (shelves, posters, busy
   wallpaper) can confuse the hand tracker. A plain wall or a
   closed curtain behind you is best.

If the cursor is still jittery after all of the above, attach a log
file when you report the bug (see below).

## Click triggers when not intended

**Symptom:** clicks fire while you are just moving your hand around,
or while your fingers are close together but you did not mean to
click.

**Fix:**

1. **Pinch threshold.** Open **Settings → Input** and raise the
   **Pinch threshold** slider. A higher threshold means the fingers
   must be closer together before a click fires — less sensitive,
   fewer accidental clicks.
2. **Calibration.** Run **Run calibration**. The calibration step
   records what a "pinch" looks like for your hand and camera, which
   reduces false positives.
3. **Hand posture.** Try to keep your fingers in a relaxed, slightly
   curved pose between clicks. Fully straight or fully curled fingers
   both work better than a half-curl, which the tracker can confuse
   with a pinch.

If you find yourself raising the threshold so high that intentional
clicks become hard, the camera may be too far from your hand. Move
the camera closer (or move your hand closer to the camera).

## App not responding

**Symptom:** the cursor stops moving, the tray menu does not open,
or VMosue is using a lot of CPU.

**Fix:**

1. **Try the emergency stop first.** Press and hold `Esc` for one
   second, or press `Ctrl+Alt+G`. If this works, the app is still
   alive — it just had a stuck gesture. Move your hand normally and
   see if it resumes.
2. **Right-click the tray icon → Exit.** Then relaunch VMosue from
   the Start Menu.
3. **If it keeps happening**, check the log file:
   `%LOCALAPPDATA%\VMosue\logs\vmosue.log`. The most recent lines
   usually explain what went wrong (camera disconnect, GPU error,
   etc.).
4. **Reset to defaults.** Quit VMosue and delete
   `%LOCALAPPDATA%\VMosue\config.json`. The next launch will be a
   clean install.
5. **Reboot.** Some camera drivers do not release cleanly after a
   crash. A reboot is the fastest way to recover.

If the problem recurs every time, please file a bug — see below.

## Where to report bugs

Open an issue on the
[GitHub issue tracker](https://github.com/.../issues). Please include:

- **Windows version.** Open **Settings → System → About** and copy
  the "Windows specifications" section (Edition, Version, OS Build).
- **VMosue version.** **Help → About** in the app, or the file name
  of the installer you downloaded.
- **Log file.** The most recent `%LOCALAPPDATA%\VMosue\logs\vmosue.log`.
  Drag it into the issue as an attachment, or paste the last ~50
  lines into the issue body.
- **Steps to reproduce.** A short numbered list: "1. Open Settings.
  2. Switch to Camera B. 3. Click Run calibration. 4. Cursor jumps
  to top-left."
- **What you expected to happen** and **what actually happened**.
- **Screenshots or screen recording** if it is a visual bug. The
  **Debug** window in the app is a good way to capture what the
  camera sees.

The issue tracker is also where to request features, ask questions,
and check whether a bug you are seeing has already been reported.