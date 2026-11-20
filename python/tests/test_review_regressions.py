# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT

"""Regression tests for simulated cameras, native wrappers, viewers, saved data and integration stubs."""

import contextlib
import ctypes
import importlib.util
import io
import logging
import math
import pathlib
import sys
import tempfile
import threading
import types
import unittest
from unittest import mock

import numpy

from openst133 import __main__ as cli
from openst133 import spe
from openst133.camera import CameraUnavailable, RealCamera, SimulatedCamera
from openst133.device import Identity, NativeCamera, Readout
from openst133.viewer import AcquisitionController, ViewerApp

PLUGIN = pathlib.Path(__file__).resolve().parents[2] / "integrations" / "scopefoundry" / "ScopeFoundryHW" / "openst133"


class TextVariable:
    def __init__(self, value):
        self.value = value

    def get(self):
        return self.value

    def set(self, value):
        self.value = value


def _load_plugin():
    """Import the plugin modules against stand-ins for ScopeFoundry and pyqtgraph, which the CTest Python lacks."""
    scopefoundry = types.ModuleType("ScopeFoundry")
    scopefoundry.HardwareComponent = type("HardwareComponent", (), {})
    scopefoundry.Measurement = type("Measurement", (), {})
    modules = {}
    with mock.patch.dict(sys.modules, {"ScopeFoundry": scopefoundry, "pyqtgraph": mock.Mock()}):
        for name in ("openst133_hw", "openst133_measure"):
            spec = importlib.util.spec_from_file_location(f"_test_{name}", PLUGIN / f"{name}.py")
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            modules[name] = module
    return modules["openst133_hw"], modules["openst133_measure"]


def _plugin_hardware(hw_module, camera):
    hw = hw_module.OpenST133HW.__new__(hw_module.OpenST133HW)
    hw.camera = camera
    hw.io_lock = threading.Lock()
    hw._stopping = threading.Event()
    hw._acquiring = threading.Event()
    hw.unsupported = {}
    hw._readout_applied = None
    hw._pending = set()
    hw.log = logging.getLogger("openst133-tests")
    hw.settings = {
        "exposure": 0.01,
        "temperature_setpoint": -120.0,
        "shutter": "normal",
        "gain": 2,
        "accumulations": 1,
        "readout_rate": "1 MHz",
        "full_vertical_binning": False,
        "roi_x0": 0,
        "roi_x1": camera.width,
        "roi_y0": 0,
        "roi_y1": camera.height,
        "bin_x": 1,
        "bin_y": 1,
    }
    return hw


class ViewerRegressionTest(unittest.TestCase):
    def setUp(self):
        self.camera = SimulatedCamera(width=8, height=6, sleep_scale=0.0, seed=3)
        self.camera.open()
        self.addCleanup(self.camera.close)

    def test_saved_exposure_is_the_exposure_of_the_saved_frame(self):
        controller = AcquisitionController(self.camera)
        controller.set_exposure(0.1)
        controller.start(False)
        controller.join(5.0)
        controller.set_exposure(2.0)
        with mock.patch("openst133.export.write_spe") as write:
            controller.save("frame.spe")
        write.assert_called_once()
        self.assertEqual(write.call_args.args[2], 0.1)

    def test_full_frame_after_a_binning_that_does_not_divide_the_sensor(self):
        app = ViewerApp.__new__(ViewerApp)
        app.camera = self.camera
        app.controller = AcquisitionController(self.camera)
        app.status_text = TextVariable("idle")
        app.rate_text = TextVariable("1 MHz")
        app.region_text = TextVariable("full")
        app.fvb = TextVariable(False)
        app.roi_text = [TextVariable(str(value)) for value in (0, 0, 8, 6)]
        app.binning_text = [TextVariable("1"), TextVariable("1")]
        self.camera.set_roi(0, 0, 6, 6)
        self.camera.set_binning(3, 1)
        self.assertTrue(app.apply_readout(), app.status_text.get())
        self.assertEqual(self.camera.roi, (0, 0, 8, 6))
        self.assertEqual(self.camera.binning, (1, 1))


class NativeArgumentRangeTest(unittest.TestCase):
    """ctypes truncates Python integers to uint32_t silently, so 2**32 + 100 ms used to reach the camera as 100 ms."""

    def setUp(self):
        self.native = NativeCamera.__new__(NativeCamera)
        self.native._handle = ctypes.c_void_p(1)
        self.native._library = mock.Mock()

    def test_out_of_range_arguments_are_refused_before_the_call(self):
        calls = (
            (self.native.set_exposure_ms, (2**32 + 100,), "ost_camera_set_exposure"),
            (self.native.set_exposure_ms, (-1,), "ost_camera_set_exposure"),
            (self.native.set_readout_rate, (2**32 + 100000,), "ost_camera_set_readout_rate"),
            (self.native.set_readout, (Readout(2**32 + 10, 0, 10, 10, 1, 1),), "ost_camera_set_readout"),
            (self.native.stream, (2**32, lambda index, data: False), "ost_camera_stream"),
        )
        for method, arguments, function in calls:
            with self.subTest(method=method.__name__, arguments=arguments):
                with self.assertRaises(ValueError):
                    method(*arguments)
                getattr(self.native._library, function).assert_not_called()


class CommandLineRegressionTest(unittest.TestCase):
    def test_acquire_refuses_an_infinite_exposure(self):
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as raised:
                cli.main(["acquire", "--simulate", "--exposure", "inf", "--out", str(pathlib.Path(directory) / "x.spe")])
        self.assertEqual(raised.exception.code, 2)

    def test_list_reports_a_missing_library_without_a_traceback(self):
        stderr = io.StringIO()
        with mock.patch("openst133.device.list_devices", side_effect=OSError("openst133 shared library not found")):
            with contextlib.redirect_stderr(stderr):
                self.assertEqual(cli.main(["list"]), 2)
        self.assertIn("openst133 shared library not found", stderr.getvalue())


class SpeWriterRegressionTest(unittest.TestCase):
    def test_floating_pixels_outside_uint16_are_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            for index, value in enumerate((70000.0, -1.0, math.nan, math.inf)):
                path = pathlib.Path(directory) / f"frame{index}.spe"
                with self.subTest(value=value):
                    with self.assertRaises(ValueError):
                        spe.create(path, [numpy.array([[value, 1.0]])], 0.1, data_type="uint16")
                    self.assertFalse(path.exists())


@unittest.skipUnless(PLUGIN.is_dir(), "ScopeFoundry plugin source not present")
class ScopeFoundryPluginRegressionTest(unittest.TestCase):
    def setUp(self):
        self.hw_module, self.measure_module = _load_plugin()
        self.camera = SimulatedCamera(width=8, height=6, sleep_scale=0.0, seed=5)
        self.camera.open()
        self.addCleanup(self.camera.close)
        self.hw = _plugin_hardware(self.hw_module, self.camera)

    def _measure(self, camera=None):
        measure = self.measure_module.OpenST133Measure.__new__(self.measure_module.OpenST133Measure)
        if camera is not None:
            self.hw.camera = camera
        measure.app = types.SimpleNamespace(hardware={"openst133": self.hw})
        measure.settings = {
            "mode": "live",
            "n_frames": 1,
            "save_h5": False,
            "save_spe": True,
            "wavelength_start": 0.0,
            "wavelength_step": 0.0,
            "last_saved": "",
        }
        measure.log = logging.getLogger("openst133-tests")
        measure.new_dataset_metadata = lambda: types.SimpleNamespace(get_file_path=lambda suffix: f"unused{suffix}")
        measure.exposure_used = 0.1
        measure.last_frame = None
        measure.last_temperature = float("nan")
        return measure

    def test_identity_property_is_read_not_called(self):
        identity = Identity(4, 98, "EEV 100x1340B", 8, 6, None)
        camera = RealCamera()
        camera._native = types.SimpleNamespace(target_source=0)
        readout = Readout(0, 0, 8, 6, 1, 1)
        with (
            mock.patch.object(NativeCamera, "get_readout", return_value=readout),
            mock.patch.object(RealCamera, "identity", new_callable=mock.PropertyMock, return_value=identity),
        ):
            metadata = camera.spe_metadata()
        self.assertEqual((metadata["detector_id"], metadata["detector_name"]), (98, "EEV 100x1340B"))
        failing = mock.PropertyMock(side_effect=CameraUnavailable("identification is unavailable"))
        with (
            mock.patch.object(NativeCamera, "get_readout", return_value=readout),
            mock.patch.object(RealCamera, "identity", failing),
        ):
            metadata = camera.spe_metadata()
        self.assertNotIn("detector_id", metadata)

    def test_settings_write_during_an_acquisition_does_not_wait_for_it(self):
        self.assertTrue(self.hw.io_lock.acquire(blocking=False))
        self.hw._acquiring.set()
        writer = threading.Thread(target=self.hw._write, args=("exposure", 0.5), daemon=True)
        try:
            writer.start()
            writer.join(1.0)
            self.assertFalse(writer.is_alive(), "the exposure write waited for the running acquisition")
            self.assertEqual(self.camera.exposure, 0.1)
        finally:
            self.hw._acquiring.clear()
            self.hw.io_lock.release()
            writer.join(5.0)
        self.hw.settings["exposure"] = 0.5
        self.hw.acquire()
        self.assertEqual(self.camera.exposure, 0.5)

    def test_save_last_frame_records_the_settings_the_frame_was_taken_with(self):
        measure = self._measure()
        measure.interrupt_measurement_called = True
        measure.run()
        self.assertIsNotNone(measure.last_frame)
        self.hw.settings["exposure"] = 2.0
        self.hw.settings["roi_x1"], self.hw.settings["bin_x"] = 4, 2
        exported = {}
        measure.export_spe = lambda path, frames, acquisition: exported.update(acquisition) or path
        measure.save_last_frame()
        self.assertEqual(exported["exposure_seconds"], 0.01)
        self.assertEqual(tuple(exported["roi"]), (0, 0, 8, 6))
        self.assertEqual(tuple(exported["binning"]), (1, 1))


if __name__ == "__main__":
    unittest.main()
