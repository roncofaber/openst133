# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT

"""Regression tests for simulated-camera stream abort handling."""

import unittest

from openst133.camera import SimulatedCamera


class SimulatedStreamAbortTest(unittest.TestCase):
    """An abort between two stream frames must end the stream."""

    def test_abort_between_frames_ends_stream(self):
        camera = SimulatedCamera(sleep_scale=0.0, seed=1)
        camera.open()
        self.addCleanup(camera.close)
        camera.set_exposure(0.1)
        frames = camera.stream()
        self.addCleanup(frames.close)
        next(frames)
        camera.abort()
        with self.assertRaises(StopIteration, msg="the stream acquired another frame after abort()"):
            next(frames)


if __name__ == "__main__":
    unittest.main()
