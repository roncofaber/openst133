# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT

import pathlib
import struct
import tempfile
import unittest

import numpy

from openst133 import spe
from openst133.camera import CameraUnavailable, SimulatedCamera
from openst133.device import SHUTTER_MODES, shutter_mode_name
from openst133.viewer import AcquisitionController


class GainShutterAccumulationTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.directory.name)
        self.camera = SimulatedCamera(sleep_scale=0.0, seed=1)
        self.camera.open()

    def tearDown(self):
        self.camera.close()
        self.directory.cleanup()

    def test_shutter_names_and_bool_compatibility(self):
        self.assertEqual(SHUTTER_MODES, {"normal": 1, "closed": 0, "opened": 2, "preopen": 3})
        self.assertEqual(shutter_mode_name(True), "closed")
        self.assertEqual(shutter_mode_name(False), "normal")
        with self.assertRaises(ValueError):
            shutter_mode_name("open")

    def test_int32_frames_write_a_winspec_long_spe(self):
        frame = numpy.array([[16195, 0], [70000, 3]], dtype=numpy.int32)
        path = self.root / "long.spe"
        spe.create(path, [frame], 0.1, metadata={"controller_gain": 3, "accumulations": 3, "shutter": "opened"})
        header = path.read_bytes()[:4100]
        self.assertEqual(struct.unpack_from("<h", header, 108)[0], spe.SPE_INT32)
        self.assertAlmostEqual(struct.unpack_from("<f", header, 1992)[0], 2.2, places=5)
        with spe.SpeFile(path) as written:
            self.assertEqual(written.info.data_type, spe.SPE_INT32)
            self.assertEqual(written.metadata.controller_gain, 3)
            self.assertEqual(written.metadata.accumulations, 3)
            self.assertEqual(written.metadata.shutter, "opened")
        self.assertEqual(path.stat().st_size, 4100 + frame.nbytes)

    def test_simulated_camera_accumulates_into_int32_and_refuses_pre_open(self):
        self.camera.set_exposure(0.01)
        self.camera.set_gain(3)
        self.camera.set_shutter("opened")
        self.camera.set_accumulations(3)
        frame = self.camera.acquire()
        self.assertEqual(frame.dtype, numpy.dtype("<i4"))
        self.assertEqual(frame.shape, (self.camera.height, self.camera.width))
        with self.assertRaises(CameraUnavailable):
            self.camera.set_shutter("preopen")
        with self.assertRaises(CameraUnavailable):
            self.camera.set_gain(4)
        with self.assertRaises(CameraUnavailable):
            self.camera.set_accumulations(2)
        self.assertEqual((self.camera.gain, self.camera.accumulations, self.camera.shutter), (3, 3, "opened"))

    def test_viewer_save_records_gain_shutter_and_accumulations(self):
        self.camera.set_gain(1)
        self.camera.set_shutter("closed")
        self.camera.set_accumulations(3)
        controller = AcquisitionController(self.camera)
        controller.set_exposure(0.01)
        controller.start(False)
        controller.join(10)
        path = self.root / "viewer.spe"
        controller.save(path)
        with spe.SpeFile(path) as written:
            self.assertEqual(written.info.data_type, spe.SPE_INT32)
            self.assertEqual(written.metadata.controller_gain, 1)
            self.assertEqual(written.metadata.accumulations, 3)
            self.assertEqual(written.metadata.shutter, "closed")


if __name__ == "__main__":
    unittest.main()
