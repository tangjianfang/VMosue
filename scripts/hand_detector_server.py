#!/usr/bin/env python3
"""Hand detector subprocess server for vmosue.

v0.3 (Python IPC bridge): the C++ HandDetector spawns this script as a
subprocess and talks to it over stdin/stdout. Per-frame wire protocol:

  C++ -> Python  (per-frame request):
      One JSON metadata line, LF-terminated:
          {"width": int, "height": int, "length": int}
      Then `length` raw bytes of BGRA pixel data
      (B, G, R, A bytes; row-major; pitch == width * 4; no padding).

  Python -> C++  (per-frame response, one JSON line, LF-terminated):
      {"hand_count": int,
       "image_width": int,
       "image_height": int,
       "hands": [{"handedness": "Left"|"Right",
                  "score": float,
                  "landmarks":       [[x, y, z], ... 21 points ...],
                  "world_landmarks": [[x, y, z], ... 21 points ...]}, ...]}

Coordinates in `landmarks` are normalized [0, 1] over the input image:
x is left-to-right, y is top-to-bottom, z is depth (negative = closer
to camera). 21 points in MediaPipe Hands canonical order (wrist, then
thumb..pinky, 4 points per finger).

Coordinates in `world_landmarks` are metric (meters), with the origin
at the hand's approximate geometric center. The C++ AirClickDetector
reads `world_landmarks[8].z` (index fingertip depth) to detect the
push-toward-camera right-click gesture, so this array MUST be present
for that gesture to work. It may be an empty list if the active model
build does not produce world landmarks.

The server boots once, prints `{"status": "ready", "model": "..."}` and
flushes, then loops until EOF on stdin (which signals the C++ parent
shutting down). Errors are emitted as
`{"status": "error", "message": "..."}` lines and the process exits
with non-zero code so the parent can detect failure.

Why a subprocess? The C++ project cannot link MediaPipe Tasks C++ (the
upstream repo's examples were restructured and the desktop C++ example
no longer exists). Running the official Python `mediapipe` package via
a child process gives the C++ parent real, commercial-grade hand
detection without any in-process Python embedding.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time

import numpy as np


def log(msg: str) -> None:
    """Diagnostic lines go to stderr so they don't pollute the JSON
    protocol stream on stdout."""
    print(msg, file=sys.stderr, flush=True)


# Upper bound on a single frame's pixel count. A corrupt or malicious
# parent could send `width`/`height` that multiply out to tens of GB;
# `os.read(0, length)` would then try to assemble a giant buffer and
# OOM-kill the process. 33 megapixels comfortably covers 8K (7680x4320
# = 33.2 MP rounds just under) plus normal webcam and 4K resolutions,
# while rejecting absurd values long before any allocation.
MAX_FRAME_PIXELS = 8192 * 4320  # ~35.4 MP
MAX_FRAME_BYTES = MAX_FRAME_PIXELS * 4  # BGRA


class ProtocolError(Exception):
    """Raised when a metadata line violates the wire protocol. The
    server turns this into a `{"status": "error", ...}` line and exits
    with a non-zero code so the C++ parent can detect the failure and
    respawn the subprocess."""


def validate_meta(meta: dict) -> tuple[int, int, int]:
    """Validate a decoded metadata dict and return (width, height,
    length). Raises ProtocolError on any violation.

    Guards, in order:
      * required keys present and integer-convertible,
      * width/height strictly positive,
      * total pixels within MAX_FRAME_PIXELS (DoS / OOM guard),
      * declared byte length equals width*height*4 (BGRA).
    """
    try:
        width = int(meta["width"])
        height = int(meta["height"])
        length = int(meta["length"])
    except (KeyError, TypeError, ValueError) as exc:
        raise ProtocolError(f"bad/missing metadata field: {exc!r}") from exc
    if width <= 0 or height <= 0:
        raise ProtocolError(f"non-positive dimensions: {width}x{height}")
    if width * height > MAX_FRAME_PIXELS:
        raise ProtocolError(
            f"frame too large: {width}x{height} "
            f"({width * height} px > {MAX_FRAME_PIXELS} px cap)")
    expected = width * height * 4  # BGRA = 4 bytes/pixel
    if length != expected:
        raise ProtocolError(f"length {length} != w*h*4 {expected}")
    return width, height, length


def build_response(result, width: int, height: int) -> dict:
    """Build the per-frame JSON response dict from a MediaPipe
    HandLandmarkerResult (or any object exposing the same fields).

    Emits BOTH normalized `landmarks` (image space, [0,1]) and
    `world_landmarks` (metric, origin at the hand's geometric center).
    The world landmarks are required by the C++ AirClickDetector, which
    reads `world[8].z` (index fingertip depth) for the push-to-
    right-click gesture; without them that gesture can never fire.
    """
    hand_landmarks = getattr(result, "hand_landmarks", []) or []
    world_landmarks = getattr(result, "hand_world_landmarks", []) or []
    handedness = getattr(result, "handedness", []) or []

    response = {
        "hand_count": len(hand_landmarks),
        "image_width": width,
        "image_height": height,
        "hands": [],
    }
    for i, hand_lms in enumerate(hand_landmarks):
        score = 1.0
        handedness_label = "Right"
        if i < len(handedness):
            cat = handedness[i]
            # `category` is a Category proto; .category_name and
            # .score are the public fields. The label is "Left" /
            # "Right" (MediaPipe models mirror the actual hand).
            handedness_label = cat[0].display_name or cat[0].category_name
            score = float(cat[0].score)
        landmarks_xy = [
            [float(lm.x), float(lm.y), float(lm.z)] for lm in hand_lms
        ]
        world_xy = []
        if i < len(world_landmarks):
            world_xy = [
                [float(lm.x), float(lm.y), float(lm.z)]
                for lm in world_landmarks[i]
            ]
        response["hands"].append({
            "handedness": handedness_label,
            "score": score,
            "landmarks": landmarks_xy,
            "world_landmarks": world_xy,
        })
    return response


def main() -> int:
    parser = argparse.ArgumentParser(description="vmosue hand detector server")
    parser.add_argument(
        "--model",
        default=os.path.join(
            os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
            "resources", "models", "hand_landmarker.task",
        ),
        help="Path to hand_landmarker.task (MediaPipe model file).",
    )
    parser.add_argument(
        "--num-hands", type=int, default=2,
        help="Maximum number of hands to detect (default 2).",
    )
    parser.add_argument(
        "--min-hand-confidence", type=float, default=0.6,
        help=(
            "Confidence floor for MediaPipe's hand detection and "
            "presence checks (default 0.6). Higher values reject "
            "phantom second-hand detections when only one real hand "
            "is visible to the camera. Mirrored to both "
            "min_hand_detection_confidence and "
            "min_hand_presence_confidence."
        ),
    )
    args = parser.parse_args()

    if not os.path.exists(args.model):
        print(json.dumps({
            "status": "error",
            "message": f"model not found: {args.model}",
        }), flush=True)
        return 2

    # Import MediaPipe lazily so the error path above can fire even if
    # mediapipe isn't installed (e.g. on a machine that has only the
    # future C++ ONNX-runtime path enabled).
    try:
        from mediapipe.tasks import python as mp_python
        from mediapipe.tasks.python import vision as mp_vision
    except ImportError as exc:
        print(json.dumps({
            "status": "error",
            "message": f"mediapipe import failed: {exc!r}. "
                       f"Install with: pip install mediapipe",
        }), flush=True)
        return 3

    log(f"[hand_detector_server] loading model: {args.model}")
    base_opts = mp_python.BaseOptions(model_asset_path=args.model)
    # running_mode=VIDEO drives timestamp-based internal smoothing.
    # hand_landmarker is the same model as IMAGE but the C++ parent
    # sends frames at fixed cadence so the model gets consistent
    # timestamps, which reduces jitter in landmark positions.
    landmarker_opts = mp_vision.HandLandmarkerOptions(
        base_options=base_opts,
        num_hands=args.num_hands,
        running_mode=mp_vision.RunningMode.VIDEO,
        # Raise the default 0.5 floor to suppress phantom second
        # hands when only one real hand is visible — MediaPipe with
        # num_hands=2 will otherwise emit a low-confidence second
        # entry alongside the real one, which then shows up in the
        # DebugWindow preview as an extra set of dots.
        min_hand_detection_confidence=args.min_hand_confidence,
        min_hand_presence_confidence=args.min_hand_confidence,
    )
    landmarker = mp_vision.HandLandmarker.create_from_options(landmarker_opts)
    log("[hand_detector_server] model loaded")

    # Announce readiness. The C++ parent blocks on this line before
    # sending any frames.
    print(json.dumps({"status": "ready", "model": args.model}), flush=True)

    from mediapipe import Image, ImageFormat

    frame_count = 0
    t_start = time.monotonic()
    # VIDEO mode requires a monotonically increasing timestamp in ms.
    timestamp_ms = 0

    while True:
        # ---- Read metadata line (raw fd, 1 byte at a time) ----
        # Use os.read on fd 0 directly to bypass any TextIOWrapper
        # buffering weirdness on Windows when the parent process is
        # a GUI-subsystem binary (which is what vmosue.exe is).
        meta_bytes = bytearray()
        while True:
            ch = os.read(0, 1)
            if not ch:
                log(f"[hand_detector_server] EOF after {frame_count} "
                    f"frames; elapsed={time.monotonic()-t_start:.1f}s")
                return 0
            if ch == b"\n":
                break
            meta_bytes += ch
            # Guard against runaway metadata lines (a corrupt parent
            # shouldn't pin a CPU at 100% parsing).
            if len(meta_bytes) > 4096:
                log("[hand_detector_server] metadata line too long")
                return 7
        try:
            meta = json.loads(meta_bytes.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            log(f"[hand_detector_server] bad metadata: {exc!r}")
            print(json.dumps({
                "status": "error",
                "message": f"bad metadata line: {exc!r}",
            }), flush=True)
            return 8

        try:
            width, height, length = validate_meta(meta)
        except ProtocolError as exc:
            log(f"[hand_detector_server] {exc}")
            print(json.dumps({
                "status": "error",
                "message": str(exc),
            }), flush=True)
            return 9

        # ---- Read the BGRA frame bytes ----
        payload = b""
        while len(payload) < length:
            chunk = os.read(0, length - len(payload))
            if not chunk:
                log("[hand_detector_server] EOF mid-frame")
                return 10
            payload += chunk

        # Everything from pixel reshape through response construction is
        # wrapped so a single bad frame (corrupt pixels, a transient
        # MediaPipe graph error, an unexpected result shape) degrades to
        # a "zero hands" response instead of taking down the whole
        # server. A crashed server freezes the cursor until the C++
        # parent respawns it; a per-frame skip is invisible to the user.
        try:
            # BGRA -> RGB numpy. CameraCapture delivers BGRA with row
            # pitch == width * 4 (no padding); reshape works directly.
            # ascontiguousarray: MediaPipe's Image requires a contiguous
            # buffer, and fancy-indexing the channel order yields a view.
            bgra = np.frombuffer(payload, dtype=np.uint8).reshape(
                (height, width, 4))
            rgb = np.ascontiguousarray(bgra[:, :, [2, 1, 0]])  # BGRA -> RGB

            # ---- Run HandLandmarker ----
            mp_img = Image(image_format=ImageFormat.SRGB, data=rgb)
            result = landmarker.detect_for_video(mp_img, timestamp_ms)
            timestamp_ms += 33  # ~30 fps cadence hint to the model

            # ---- Build response ----
            response = build_response(result, width, height)
        except Exception as exc:  # noqa: BLE001 - intentional catch-all
            # Advance the timestamp on the skipped frame too so the
            # model's VIDEO-mode smoothing doesn't see a time stall.
            timestamp_ms += 33
            log(f"[hand_detector_server] frame {frame_count} skipped: "
                f"{exc!r}")
            response = {
                "hand_count": 0,
                "image_width": width,
                "image_height": height,
                "hands": [],
            }

        # Write one JSON line. Do NOT pretty-print — every byte costs
        # ~30 fps of parsing on the C++ side.
        sys.stdout.write(json.dumps(response, separators=(",", ":")))
        sys.stdout.write("\n")
        sys.stdout.flush()

        frame_count += 1
        if frame_count % 30 == 0:
            elapsed = time.monotonic() - t_start
            log(f"[hand_detector_server] {frame_count} frames in "
                f"{elapsed:.1f}s ({frame_count/elapsed:.1f} fps)")


if __name__ == "__main__":
    sys.exit(main())
