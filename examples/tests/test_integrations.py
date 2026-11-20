# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT

import ctypes
import os
from pathlib import Path
import sys
import tempfile
import threading
import types
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "python" / "src"))
sys.path.insert(0, str(ROOT / "examples"))
os.environ["OPENST133_NO_HARDWARE"] = "1"

import numpy
import acquire
import bluesky_detector
import ctypes_spe
import pymodaq_detector
from openst133.camera import SimulatedCamera


class FakeStatus:
    def __init__(self, device, timeout=None):
        self.device = device
        self.timeout = timeout
        self.done = threading.Event()
        self.error = None
        self.finished = False

    def set_finished(self):
        self.finished = True
        self.done.set()

    def set_exception(self, error):
        self.error = error
        self.done.set()

    def wait(self, timeout=None):
        if not self.done.wait(timeout):
            raise TimeoutError("acquisition did not finish")
        if self.error is not None:
            raise self.error


class FakeFunction:
    def __init__(self, status=0):
        self.status = status
        self.calls = []

    def __call__(self, *arguments):
        self.calls.append((arguments, ctypes.string_at(arguments[5], arguments[6])))
        return self.status


class FakeParameter:
    def __init__(self, name, value):
        self._name = name
        self._value = value

    def name(self):
        return self._name

    def value(self):
        return self._value


class FakeSettings:
    def __init__(self):
        self.parameters = {"exposure_s": FakeParameter("exposure_s", 0.01), "shutter": FakeParameter("shutter", "normal")}

    def child(self, name):
        return self.parameters[name]


class FakeSignal:
    def __init__(self):
        self.values = []

    def emit(self, value):
        self.values.append(value)


class IntegrationExamplesTest(unittest.TestCase):
    def simulated_camera(self):
        return SimulatedCamera(width=32, height=16, sleep_scale=0.0, seed=7)

    def test_plain_acquisition_writes_npy(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "frame.npy"
            camera = self.simulated_camera()
            camera.open()
            frame = acquire.acquire_to_file(output, camera)
            numpy.testing.assert_array_equal(numpy.load(output), frame)
            self.assertEqual(frame.shape, (16, 32))
            self.assertEqual(frame.dtype, numpy.dtype("uint16"))
            camera.close()

    def test_plain_spe_path_uses_the_public_writer(self):
        with tempfile.TemporaryDirectory() as directory, self.simulated_camera() as camera:
            output = Path(directory) / "frame.spe"
            with mock.patch("openst133.spe.create") as create:
                frame = acquire.acquire_to_file(output, camera)
            arguments = create.call_args.args
            self.assertEqual(arguments[0], str(output))
            numpy.testing.assert_array_equal(arguments[1], [frame])
            self.assertEqual(arguments[2], 0.01)

    def test_bluesky_detector_protocol_with_mock_ophyd(self):
        previous = bluesky_detector.DeviceStatus
        bluesky_detector.DeviceStatus = FakeStatus
        self.addCleanup(setattr, bluesky_detector, "DeviceStatus", previous)
        with bluesky_detector.OpenST133Detector(camera=self.simulated_camera()) as detector:
            status = detector.trigger()
            status.wait(2)
            self.assertTrue(status.finished)
            key = detector.image_name
            reading = detector.read()[key]
            descriptor = detector.describe()[key]
            self.assertEqual(reading["value"].shape, (16, 32))
            self.assertEqual(descriptor["shape"], [16, 32])
            self.assertEqual(descriptor["dtype"], "array")
            self.assertEqual(descriptor["dtype_numpy"], reading["value"].dtype.str)
            self.assertIsInstance(reading["timestamp"], float)

    def test_pymodaq_detector_openst133_side_with_mock_framework(self):
        previous = pymodaq_detector.DataFromPlugins, pymodaq_detector.DataToExport
        pymodaq_detector.DataFromPlugins = lambda **fields: types.SimpleNamespace(**fields)
        pymodaq_detector.DataToExport = lambda name, data: types.SimpleNamespace(name=name, data=data)
        self.addCleanup(setattr, pymodaq_detector, "DataFromPlugins", previous[0])
        self.addCleanup(setattr, pymodaq_detector, "DataToExport", previous[1])
        detector = pymodaq_detector.DAQ_2DViewer_OpenST133()
        detector.is_master = True
        detector.camera_factory = self.simulated_camera
        detector.settings = FakeSettings()
        detector.dte_signal = FakeSignal()
        detector.ini_attributes()
        message, initialized = detector.ini_detector()
        self.assertTrue(initialized, message)
        detector.commit_settings(FakeParameter("exposure_s", 0.02))
        detector.commit_settings(FakeParameter("shutter", "closed"))
        self.assertEqual(detector.controller.exposure, 0.02)
        self.assertEqual(detector.controller.shutter, "closed")
        detector.grab_data(Naverage=2)
        self.assertEqual(len(detector.dte_signal.values), 1)
        exported = detector.dte_signal.values[0]
        self.assertEqual(exported.name, "openst133")
        self.assertEqual(exported.data[0].dim, "Data2D")
        self.assertEqual(exported.data[0].data[0].shape, (16, 32))
        self.assertEqual(exported.data[0].data[0].dtype, numpy.dtype("float64"))
        detector.stop()
        detector.close()
        self.assertIsNone(detector.controller)

    def test_pymodaq_slave_does_not_close_shared_camera(self):
        camera = self.simulated_camera()
        camera.open()
        self.addCleanup(camera.close)
        detector = pymodaq_detector.DAQ_2DViewer_OpenST133()
        detector.ini_attributes()
        detector.is_master = False
        detector.controller = camera
        detector.close()
        self.assertIsNone(detector.controller)
        self.assertIsInstance(camera.temperature(), float)

    def test_ctypes_spe_writer_with_mock_native_library(self):
        native = types.SimpleNamespace(ost_spe_create=FakeFunction())
        ctypes_spe.bind_spe_writer(native)
        with self.simulated_camera() as camera:
            frame = camera.acquire()
        ctypes_spe.write_uint16_spe(native, "frame.spe", frame, 0.01)
        arguments, pixel_bytes = native.ost_spe_create.calls[0]
        self.assertEqual(arguments[0], b"frame.spe")
        self.assertEqual(arguments[1:5], (32, 16, ctypes_spe.OST_SPE_UINT16, 1))
        self.assertEqual(arguments[6], frame.nbytes)
        self.assertEqual(arguments[7], 0.01)
        self.assertIsNone(arguments[8])
        self.assertEqual(pixel_bytes, frame.astype("<u2", copy=False).tobytes())
        self.assertEqual(native.ost_spe_create.restype, ctypes.c_int)

    def test_ctypes_spe_writer_raises_native_status(self):
        native = types.SimpleNamespace(ost_spe_create=FakeFunction(status=-3))
        ctypes_spe.bind_spe_writer(native)
        frame = numpy.zeros((2, 3), dtype=numpy.uint16)
        with self.assertRaisesRegex(OSError, "ost_spe_create failed"):
            ctypes_spe.write_uint16_spe(native, "frame.spe", frame, 0.01)


if __name__ == "__main__":
    unittest.main()
