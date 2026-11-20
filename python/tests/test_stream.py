# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT

import threading
import time
import unittest

import numpy

from openst133.camera import CameraError, CameraUnavailable, RealCamera, SimulatedCamera
from openst133.device import OST_ERR_UNSUPPORTED, NativeError, Readout, StreamStats
from openst133.viewer import AcquisitionController

FRAME = 1340 * 100 * 2


def stats(delivered, stopped):
    return StreamStats(delivered, 0, 0, 0, delivered * FRAME, 0, 0, stopped)


class StubNative:
    """Delivers numbered frames like the C stream and ends on abort() or on_frame returning True."""

    def __init__(self, limit=None, error=None, delay=0.0):
        self.aborted = threading.Event()
        self.limit = limit
        self.error = error
        self.delay = delay
        self.aborts = 0

    def abort(self):
        self.aborts += 1
        self.aborted.set()

    def get_readout(self):
        return Readout(0, 0, 1340, 100, 1, 1)

    def stream(self, frame_count, on_frame):
        if self.error is not None:
            raise self.error
        index = 0
        while not self.aborted.is_set() and (frame_count == 0 or index < frame_count):
            if self.limit is not None and index >= self.limit:
                break
            data = numpy.full(1340 * 100, index, dtype=numpy.uint16).tobytes()
            if on_frame(index, data):
                return stats(index + 1, True)
            index += 1
            time.sleep(self.delay)
        return stats(index, self.aborted.is_set())


def camera_with(native):
    camera = RealCamera.__new__(RealCamera)
    camera._native = native
    camera.width, camera.height = 1340, 100
    return camera


class RealStreamTests(unittest.TestCase):
    def test_finite_stream_yields_every_frame_in_order(self):
        camera = camera_with(StubNative())
        frames = list(camera.stream(5, queue_size=16))
        self.assertEqual([int(f[0, 0]) for f in frames], [0, 1, 2, 3, 4])
        self.assertEqual(frames[0].shape, (100, 1340))
        self.assertEqual(camera.stream_stats.frames_delivered, 5)
        self.assertEqual(camera.stream_dropped, 0)

    def test_closing_the_iterator_stops_the_camera(self):
        native = StubNative(delay=0.001)
        camera = camera_with(native)
        iterator = camera.stream()
        next(iterator)
        next(iterator)
        iterator.close()
        self.assertTrue(native.aborted.is_set())
        self.assertTrue(camera.stream_stats.stopped)

    def test_slow_consumer_drops_are_counted(self):
        camera = camera_with(StubNative(limit=40))
        seen = []
        for frame in camera.stream(queue_size=2):
            time.sleep(0.01)
            seen.append(int(frame[0, 0]))
        self.assertEqual(len(seen) + camera.stream_dropped, 40)
        self.assertGreater(camera.stream_dropped, 0)
        self.assertEqual(seen, sorted(seen))

    def test_errors_surface(self):
        camera = camera_with(StubNative(error=NativeError(OST_ERR_UNSUPPORTED, "no")))
        with self.assertRaises(CameraUnavailable):
            list(camera.stream(1))
        camera = camera_with(StubNative(error=NativeError(-3, "io")))
        with self.assertRaises(CameraError):
            list(camera.stream(1))


class ControllerTests(unittest.TestCase):
    def test_continuous_mode_uses_stream_and_stops(self):
        camera = SimulatedCamera(sleep_scale=0.0)
        camera.open()
        controller = AcquisitionController(camera)
        controller.set_exposure(0.001)
        controller.start(True)
        deadline = time.time() + 5
        frames = 0
        while frames < 3 and time.time() < deadline:
            frames += sum(1 for kind, _ in controller.drain() if kind == "frame")
            time.sleep(0.01)
        controller.stop()
        controller.join(5)
        self.assertFalse(controller.running)
        self.assertGreaterEqual(frames, 3)


if __name__ == "__main__":
    unittest.main()
