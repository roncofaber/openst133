# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT

import contextlib
import importlib.util
import io
import logging
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

import numpy

import openst133
from openst133.__main__ import main as module_main
from openst133.camera import CameraError, RealCamera, SimulatedCamera
from openst133.device import NativeCamera, NativeError
from openst133.viewer import acquire_to_file


ROOT = Path(__file__).resolve().parents[2]


def load_smoke():
    from openst133 import check

    return check


class LibraryLoggingTest(unittest.TestCase):
    def test_package_installs_only_a_null_handler(self):
        handlers = logging.getLogger("openst133").handlers
        self.assertTrue(handlers)
        self.assertTrue(all(type(handler) is logging.NullHandler for handler in handlers))

    def test_simulated_acquisition_logs_lifecycle_settings_and_frame_summary(self):
        camera = SimulatedCamera(width=6, height=4, sleep_scale=0.0, seed=1)
        with self.assertLogs("openst133.camera", level="INFO") as captured:
            camera.open()
            camera.set_exposure(0.25)
            frame = camera.acquire()
            camera.close()
        messages = [record.getMessage() for record in captured.records]
        self.assertTrue(all(record.name == "openst133.camera" for record in captured.records))
        self.assertTrue(any("camera opened" in message for message in messages))
        self.assertTrue(any("setting applied" in message and "exposure_s=0.25" in message for message in messages))
        self.assertTrue(any("camera closed" in message for message in messages))
        summary = next(message for message in messages if "acquisition complete" in message)
        for field in (
            "exposure_s=0.25",
            "shutter=normal",
            "frame_count=1",
            "duration_s=",
            f"frame_mean={float(numpy.mean(frame)):.6f}",
            f"frame_min={int(numpy.min(frame))}",
            f"frame_max={int(numpy.max(frame))}",
        ):
            self.assertIn(field, summary)
        self.assertNotIn("serial", "\n".join(messages).lower())

    def test_debug_includes_acquisition_step_detail(self):
        camera = SimulatedCamera(width=6, height=4, sleep_scale=0.0, seed=1)
        camera.open()
        with self.assertLogs("openst133.camera", level="DEBUG") as captured:
            camera.acquire()
        camera.close()
        self.assertTrue(any(
            record.levelno == logging.DEBUG and "acquisition started" in record.getMessage()
            for record in captured.records
        ))

    def test_native_error_log_preserves_last_error_text(self):
        camera = RealCamera()
        camera._native = object()
        error = NativeError(123, "sensor read failed: native last-error detail")
        with mock.patch.object(NativeCamera, "get_temperature", side_effect=error):
            with self.assertLogs("openst133.camera", level="ERROR") as captured:
                with self.assertRaisesRegex(CameraError, "native last-error detail"):
                    camera.temperature()
        camera._native = None
        self.assertIn("sensor read failed: native last-error detail", captured.output[0])

    def test_viewer_emits_per_step_debug_records(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "frame.spe"
            with mock.patch("openst133.export.write_spe"):
                with self.assertLogs("openst133.viewer", level="DEBUG") as captured:
                    acquire_to_file(SimulatedCamera(width=6, height=4, sleep_scale=0.0), 0.01, path)
        self.assertTrue(any(
            record.levelno == logging.DEBUG and "acquisition started" in record.getMessage()
            for record in captured.records
        ))


class CommandLoggingTest(unittest.TestCase):
    def setUp(self):
        self.root_level = logging.root.level

    def tearDown(self):
        for handler in logging.root.handlers[:]:
            logging.root.removeHandler(handler)
            handler.close()
        logging.root.setLevel(self.root_level)

    def test_acquire_log_file_receives_info_records_without_stderr(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "frames.spe"
            log = Path(temporary) / "acquire.log"
            stdout = io.StringIO()
            stderr = io.StringIO()
            with mock.patch("openst133.export.write_spe"):
                with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                    result = module_main([
                        "acquire", "--simulate", "--exposure", "0.01", "--out", str(output),
                        "--log-level", "INFO", "--log-file", str(log),
                    ])
            self.assertEqual(result, 0, stderr.getvalue())
            self.assertEqual(stderr.getvalue(), "")
            records = log.read_text(encoding="utf-8")
        self.assertIn("openst133.camera", records)
        self.assertIn("camera opened", records)
        self.assertIn("setting applied", records)
        self.assertIn("acquisition complete", records)
        self.assertIn("camera closed", records)
        self.assertNotIn("serial", records.lower())

    def test_smoke_log_file_receives_info_records_without_stderr(self):
        smoke = load_smoke()
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "run"
            log = Path(temporary) / "smoke.log"
            stdout = io.StringIO()
            stderr = io.StringIO()
            with mock.patch.object(smoke, "_spe_checks", return_value={}):
                with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                    result = smoke.main([
                        "--simulate", "--output", str(output),
                        "--log-level", "INFO", "--log-file", str(log),
                    ])
            self.assertEqual(result, 0, stderr.getvalue())
            self.assertEqual(stderr.getvalue(), "")
            records = log.read_text(encoding="utf-8")
        self.assertIn("camera opened", records)
        self.assertIn("acquisition complete", records)
        self.assertIn("camera closed", records)


if __name__ == "__main__":
    unittest.main()
