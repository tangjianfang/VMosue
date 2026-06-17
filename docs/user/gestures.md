# Gestures

This page lists every gesture VMosue recognises. Each gesture has a
small text diagram — visual aids (screenshots / video) will be added
in a future release.

> **Developers / QA:** for the precise landmark-and-threshold
> definition of each gesture and its mapped action, see
> [gesture-action-map.md](gesture-action-map.md).

## Quick reference

| Gesture | Hand | Motion | Action |
|---|---|---|---|
| Cursor | Right | Move hand | Cursor follows index fingertip |
| Left click | Right | Thumb to index pinch + release | Single click |
| Double click | Right | Two quick pinches | Double click |
| Left drag | Right | Pinch and hold + move | Drag |
| Middle click | Right | Thumb to **middle** finger pinch + release | Middle (wheel) click |
| Right click | Right | Move hand toward camera | Right click |
| Scroll | Left | Two fingers close + move up/down/left/right | Wheel (vertical + horizontal) |
| Pause / Resume | Left | Open hand held 1 s | Toggle pause |
| Emergency stop | Either | Both hands open, or `Ctrl+Alt+G`, or hold `Esc` 1 s | Stop all input |

## Cursor

**Hand:** right
**Motion:** any hand movement
**Action:** the cursor follows the **tip of your index finger**.

```
       Thumb
         \
          \
  Index ->  *  <-- cursor point
          /
         /
       Middle
```

Move your hand right to move the cursor right, and so on. Keep your
hand flat and relaxed for the smoothest tracking.

## Left click

**Hand:** right
**Motion:** bring the **thumb** and **index finger** together
("pinch"), then release.
**Action:** single mouse click.

```
  Thumb --\
            \  -> pinch (click)
  Index  --/

  Quick pinch + release  =  single click
```

The pinch detector measures the distance between the thumb tip and the
index finger tip. When the distance drops below the **pinch threshold**,
a click fires. As of v0.5 the threshold is **adaptive**: VMosue observes
the open/closed range of your pinch over a rolling window and derives
the cut-off automatically — there is no slider to tune. The Settings
window shows the currently-derived value as a live readout.

## Double click

**Hand:** right
**Motion:** two quick pinches in a row.
**Action:** double mouse click.

```
  pinch   pinch
    v       v
  ___     ___
 /   \   /   \
/_   _\ /_   _\
  | |     | |
  ~ ~     ~ ~
   t = 0   t < 300 ms
```

Both pinches must happen within your **Windows double-click speed** of
each other (Settings → Bluetooth & devices → Mouse → Additional mouse
settings). VMosue reads this system setting so the gesture matches
every other double-click on your machine. If the gap is too long, the
pinches register as two single clicks.

## Left drag

**Hand:** right
**Motion:** pinch and **hold**, then move your hand.
**Action:** the mouse button stays held while you move — i.e. a drag.

```
  Thumb -----  Index      (no click)

  Thumb --\
            \              (pinch held)
  Index  --/

       \
        \___________________________   drag
                                      v
```

Release the pinch to drop whatever you are dragging.

## Middle click

**Hand:** right
**Motion:** bring the **thumb** and **middle finger** together
("pinch"), then release.
**Action:** middle (mouse-wheel) click.

```
  Thumb ---\
            \  -> pinch the MIDDLE finger (middle click)
  Middle --/

  Index stays out of the way
```

Middle click uses the same pinch mechanic as left click but targets
the middle finger instead of the index finger. Left click always wins
if both pinches are detected in the same instant, so a deliberate
thumb-middle pinch is read as a middle click only when the index
finger is not also pinched.

## Right click

**Hand:** right
**Motion:** move your hand **toward the camera** (push forward, away
from you).
**Action:** right mouse click.

```
                camera
                  ^
                  |  <-- hand moves toward camera
                  |
                 ___
                |   |
                |   |
                |___|

         (Z axis:  + = away from camera
                    - = toward camera
                    depth threshold ~ 5 cm )
```

This is the only gesture that uses depth instead of finger pose. If
the camera does not expose depth well (most built-in laptop cameras
do not), the right-click also fires on a quick backward flick.

## Scroll

**Hand:** **left**
**Motion:** bring the **index** and **middle** fingers close together
(like a peace sign without the spread), then move the hand **up,
down, left, or right**.
**Action:** mouse wheel events — vertical motion sends wheel up/down,
horizontal motion sends the horizontal wheel (tilt) left/right.

```
  Left hand:

    Index  \
            \   <-- two fingers close + move up = scroll up
    Middle /

    Index  /
            \   <-- two fingers close + move down = scroll down
    Middle \
```

The fingers do not need to stay touching — they just need to be close
together. Move your hand vertically to send wheel events in that
direction. Move slowly for fine scrolling; flick for fast scrolling.

## Pause / Resume

**Hand:** **left**
**Motion:** hold an **open left hand** still for **1 second**.
**Action:** toggle pause. The overlay indicator turns yellow and all
gesture input is ignored until you move your hand again.

```
  Left hand open, held 1 s  =  pause

    +-----+         +-----+
    |     |   -->   |     |   (yellow overlay = paused)
    |     |         |     |
    +-----+         +-----+
       0 s           1 s
```

Pause is useful when you want to type on a real keyboard, eat lunch,
or reach for something without VMosue interpreting the motion as a
gesture.

## Emergency stop

**Hand:** **either** (or use a hotkey)
**Motion:** open **both hands** at the same time.
**Action:** release every held key and mouse button, stop processing
gestures, and unlock the cursor if it was locked.

```
  Both hands open  =  emergency stop

    +-----+   +-----+
    |     |   |     |
    |     |   |     |
    +-----+   +-----+
```

The same stop also fires on:

- **`Ctrl+Alt+G`** — press all three at once.
- **`Esc` held for 1 second** — easier than the keyboard chord if your
  hands are already on the mouse.

Use emergency stop if the cursor is stuck, a click is being held
down, or the app is doing something you did not intend. The app
keeps running — you can resume by closing both hands and moving
normally.