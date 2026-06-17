#!/usr/bin/env python3
"""Unit tests for the pure-logic helpers in hand_detector_server.py.

These cover the parts of the server that do NOT require MediaPipe to be
installed: metadata validation (including the size-bound guard added in
v0.5) and response construction (including the world-landmark payload).

Run with:  python -m unittest tests.python.test_hand_detector_server
or:        python tests/python/test_hand_detector_server.py
"""

import os
import sys
import unittest

# Make scripts/ importable regardless of CWD.
_HERE = os.path.dirname(os.path.abspath(__file__))
_SCRIPTS = os.path.join(os.path.dirname(_HERE), "..", "scripts")
sys.path.insert(0, os.path.abspath(_SCRIPTS))

import hand_detector_server as srv  # noqa: E402


class ValidateMetaTests(unittest.TestCase):
    def test_accepts_well_formed_meta(self):
        w, h, length = srv.validate_meta({"width": 640, "height": 480,
                                          "length": 640 * 480 * 4})
        self.assertEqual((w, h, length), (640, 480, 640 * 480 * 4))

    def test_rejects_length_mismatch(self):
        with self.assertRaises(srv.ProtocolError):
            srv.validate_meta({"width": 640, "height": 480, "length": 123})

    def test_rejects_missing_field(self):
        with self.assertRaises(srv.ProtocolError):
            srv.validate_meta({"width": 640, "height": 480})

    def test_rejects_non_positive_dimensions(self):
        for meta in ({"width": 0, "height": 480, "length": 0},
                     {"width": 640, "height": -1, "length": 0}):
            with self.assertRaises(srv.ProtocolError):
                srv.validate_meta(meta)

    def test_rejects_oversize_frame(self):
        # 100000 x 100000 x 4 ~= 40 GB; must be rejected before any
        # allocation is attempted (DoS / OOM guard).
        big = 100000
        with self.assertRaises(srv.ProtocolError):
            srv.validate_meta({"width": big, "height": big,
                               "length": big * big * 4})

    def test_accepts_4k_frame(self):
        # A legitimately large but sane frame must still pass.
        w, h, length = srv.validate_meta(
            {"width": 3840, "height": 2160, "length": 3840 * 2160 * 4})
        self.assertEqual((w, h), (3840, 2160))


class _FakeLandmark:
    def __init__(self, x, y, z):
        self.x, self.y, self.z = x, y, z


class _FakeCategory:
    def __init__(self, name, score):
        self.display_name = name
        self.category_name = name
        self.score = score


class _FakeResult:
    """Mimics the shape of mediapipe HandLandmarkerResult."""
    def __init__(self, hand_landmarks, world_landmarks, handedness):
        self.hand_landmarks = hand_landmarks
        self.hand_world_landmarks = world_landmarks
        self.handedness = handedness


class BuildResponseTests(unittest.TestCase):
    def _one_hand_result(self):
        norm = [_FakeLandmark(0.1 * i, 0.2 * i, -0.01 * i) for i in range(21)]
        world = [_FakeLandmark(0.5 * i, 0.6 * i, -0.02 * i) for i in range(21)]
        hand = [[_FakeCategory("Right", 0.97)]]
        return _FakeResult([norm], [world], hand)

    def test_includes_world_landmarks(self):
        # Regression: the C++ AirClickDetector reads right.world[8].z for
        # the push-to-right-click gesture. If the server never emits
        # world landmarks, that gesture can never fire in production.
        resp = srv.build_response(self._one_hand_result(), 640, 480)
        self.assertEqual(resp["hand_count"], 1)
        hand = resp["hands"][0]
        self.assertIn("world_landmarks", hand)
        self.assertEqual(len(hand["world_landmarks"]), 21)
        # world[8] z must round-trip the fake value (-0.02 * 8).
        self.assertAlmostEqual(hand["world_landmarks"][8][2], -0.16, places=5)

    def test_normalized_landmarks_preserved(self):
        resp = srv.build_response(self._one_hand_result(), 640, 480)
        hand = resp["hands"][0]
        self.assertEqual(len(hand["landmarks"]), 21)
        self.assertAlmostEqual(hand["landmarks"][8][0], 0.8, places=5)

    def test_handedness_and_score(self):
        resp = srv.build_response(self._one_hand_result(), 640, 480)
        hand = resp["hands"][0]
        self.assertEqual(hand["handedness"], "Right")
        self.assertAlmostEqual(hand["score"], 0.97, places=5)

    def test_handles_missing_world_landmarks_gracefully(self):
        # If a model build omits world landmarks, the response must still
        # be well-formed (empty world list), not crash.
        norm = [_FakeLandmark(0.1, 0.2, 0.0) for _ in range(21)]
        result = _FakeResult([norm], [], [[_FakeCategory("Left", 0.9)]])
        resp = srv.build_response(result, 640, 480)
        self.assertEqual(resp["hands"][0]["world_landmarks"], [])

    def test_empty_result(self):
        resp = srv.build_response(_FakeResult([], [], []), 320, 240)
        self.assertEqual(resp["hand_count"], 0)
        self.assertEqual(resp["hands"], [])


if __name__ == "__main__":
    unittest.main()
