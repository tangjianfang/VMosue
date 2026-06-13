# VMosue Tutorial

This is the 6-step interactive tutorial you see the first time VMosue
launches. The same content is shown verbatim in the in-app **Tutorial**
window (open it any time from the tray menu). The tutorial is here in
the user manual so you can read it without launching the app, share a
link, or copy the diagrams into documentation.

> **Tip:** The tutorial window can be opened any time from the system
> tray menu. To skip the auto-show on launch, set
> `showTutorialOnLaunch = false` in `%LOCALAPPDATA%\VMosue\config.json`.

---

## Step 1 / 6 — Welcome

Welcome to VMosue. This tutorial will guide you through setting up
and using gesture control.

**System requirements**

- Windows 10 or later (x64)
- A webcam (built-in or USB)
- About 200 MB free disk space
- A well-lit room (your hand should be clearly visible to the camera)

```
+-------------------+
|   VMosue v1.0     |
|   Gesture Mouse   |
|                   |
|   (o) webcam      |
|     |             |
|     v             |
|   cursor          |
+-------------------+
```

Click **Next** to continue, or **Skip** to dismiss.

---

## Step 2 / 6 — Camera positioning

Position your camera so your dominant hand is clearly visible at arm's
length. Top of camera at eye level works well.

**Tips**

- Sit at your normal distance from the screen.
- Make sure the camera can see your full hand and wrist.
- Avoid back-lighting (a window behind you makes the camera see a
  silhouette of your hand).
- The **Settings** window lets you change the camera later.

```
  screen
   +---+
   |   |
   +---+
     ^
     |  arm's length
     v
    ___        ___
   |   |      |cam| <-- top of camera at eye level
   |   |      |___|
   | hand|
   |_____|
    user
```

---

## Step 3 / 6 — Hand visibility check

Hold your hand in front of the camera, palm facing the camera. You
should see your hand highlighted in the cursor ring overlay.

**If you don't see the highlight**

- Move your hand closer to the camera.
- Improve the lighting (turn on a desk lamp).
- Use the **Settings** window to verify the right camera is selected.
- Open the **Debug** window to see what the camera sees.

```
  +-----------+
  |  camera   |
  |   view    |
  |           |
  |   /\      |
  |  /  \     |   <- your hand should appear
  |  \  /     |      inside the ring
  |   \/      |
  |   ()      |   <- cursor ring overlay
  +-----------+
```

---

## Step 4 / 6 — Practice cursor movement

Move your hand to control the cursor. The **index finger tip** is the
cursor point. Keep your hand flat for smooth movement.

The cursor follows your index fingertip. Moving your hand to the right
moves the cursor to the right, and so on. If the movement feels too
slow or jumpy, adjust **Cursor sensitivity** in the **Settings** window.

```
   Hand   ---->   Cursor moves right

       Thumb
         \
          \
  Index ->  *  <- cursor point
          /
         /
       Middle
```

---

## Step 5 / 6 — Practice pinch click

Touch your **thumb** to your **index finger** to click.

- **Quick pinch + release** = single click
- **Hold pinch** = drag (the mouse button stays held)

The pinch detector looks at the distance between the thumb tip and the
index finger tip. When the distance drops below a small threshold, the
click fires. Releasing your fingers sends a mouse-up. Holding the pinch
keeps the mouse button held (drag).

```
  Thumb --\
            \  -> pinch (click)
  Index  --/

  Quick pinch + release  =  single click

  Thumb -----  Index      (no click)
  Thumb --\
            \
  Index  --/   = single click

  Hold the pinch = drag
```

---

## Step 6 / 6 — Scroll, drag, pause, emergency stop

**Use your LEFT hand** for scroll and pause:

- **Scroll:** bring the index and middle fingers of your left hand close
  together (like a "peace" sign without the spread), then move the
  hand up or down. The system wheel events follow.
- **Pause:** hold an open left hand still for **1 second**. The
  overlay indicator turns yellow and gesture input is ignored. Move
  your hand again to resume.
- **Drag:** hold a pinch (thumb + index together) and move your hand.
  The cursor moves with the held mouse button — release to drop.

**Emergency stop:** open **BOTH** hands at the same time. The system
releases all held keys and mouse buttons, and stops processing
gestures. You can also press **Ctrl+Alt+G** or hold **Esc for 1
second**.

```
  Left hand:
    Index  \
            \   <- two fingers close + move up = scroll up
    Middle /

  Left hand open, held 1s = pause

  Both hands open  =  emergency stop

  Ctrl+Alt+G  =  emergency stop (hotkey)
  Hold Esc for 1s  =  emergency stop (hotkey)
```

---

## Where to go next

- **Settings** (tray menu) — change camera, sensitivity, performance
  mode, or auto-start.
- **Debug** (tray menu) — see the live camera feed, detected hand
  landmarks, state machine state, and action log.
- **Exit** (tray menu) — close the app.
- See also: [User manual index](README.md) for the full feature list.
