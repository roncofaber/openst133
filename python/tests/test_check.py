# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT

import json
import os
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
from types import SimpleNamespace
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from openst133.check import _frame_summary, _pixel_consistency, _repeat_consistency, inspect_frames, qemu_conflicts, run_driver_checks


ROOT = Path(__file__).resolve().parents[2]
MODULE = "openst133.check"



def library_available():
    sys.path.insert(0, str(ROOT / "python" / "src"))
    try:
        from openst133._library import load
        load()
    except (ImportError, OSError):
        return False
    finally:
        sys.path.pop(0)
    return True

class HardwareSmokeTests(unittest.TestCase):
    def test_real_run_requires_camera_acknowledgment_before_open(self):
        with tempfile.TemporaryDirectory() as temporary:
            completed = subprocess.run(
                [sys.executable, "-m", MODULE, "--output", temporary],
                cwd=ROOT,
                env={**os.environ, "PYTHONPATH": os.pathsep.join(filter(None, [str(ROOT / "python" / "src"), os.environ.get("PYTHONPATH")]))},
                text=True,
                capture_output=True,
                check=False,
            )
        self.assertNotEqual(completed.returncode, 0)
        self.assertIn("--hardware", completed.stderr)

    def test_qemu_detection_requires_the_camera_or_direct_usb_device(self):
        listing = "\n".join(
            (
                "101 qemu-system-x86_64 -device usb-host,vendorid=0x0bd7,productid=0xa010",
                "102 qemu-system-x86_64 -display none",
                "103 qemu-system-x86_64 -device usb-host,hostdevice=/dev/bus/usb/001/004",
            )
        )
        self.assertEqual(qemu_conflicts(listing), ["101", "103"])

    def test_failed_acquisition_closes_camera(self):
        class FailingCamera:
            width = 1340
            height = 100
            identity = SimpleNamespace(width=1340, height=100)
            closed = False

            def open(self):
                pass

            def temperature(self):
                return -25.0

            def set_exposure(self, exposure):
                pass

            def set_shutter(self, closed):
                pass

            def acquire(self):
                raise RuntimeError("simulated acquisition failure")

            def close(self):
                self.closed = True

        camera = FailingCamera()
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaisesRegex(RuntimeError, "simulated acquisition failure"):
                run_driver_checks(camera, Path(temporary))
        self.assertTrue(camera.closed)

    def test_simulated_driver_checks_cover_the_settings(self):
        from openst133.camera import SimulatedCamera

        with tempfile.TemporaryDirectory() as temporary:
            with mock.patch("openst133.check._spe_checks", return_value={}):
                report = run_driver_checks(SimulatedCamera(sleep_scale=0.0, seed=3), Path(temporary), simulated=True)
        self.assertTrue(report["passed"], report)
        generated = report["settings"]
        checks = generated["checks"]
        self.assertEqual(
            set(checks),
            {"gain", "shutter", "fvb", "binned_roi", "fvb_stream", "accumulations", "setpoint_persistence"},
        )
        self.assertTrue(all(check["passed"] for check in checks.values()), checks)
        self.assertEqual([item["selected_gain"] for item in checks["gain"]["frames"]], [1, 2, 3])
        self.assertEqual(checks["fvb"]["frame"]["shape"], [1, 1340])
        self.assertEqual(checks["binned_roi"]["frame"]["shape"], [20, 100])
        self.assertEqual(checks["binned_roi"]["roi"], [0, 40, 400, 20])
        self.assertEqual(checks["binned_roi"]["binning"], [4, 1])
        self.assertEqual(checks["fvb_stream"]["received_frames"], 20)
        self.assertGreater(checks["fvb_stream"]["frames_per_second"], 0.0)
        self.assertEqual(checks["accumulations"]["count"], 3)
        single_frame = checks["accumulations"]["single"]["frame"]
        accumulated_frame = checks["accumulations"]["accumulated"]["frame"]
        self.assertEqual(np.dtype(accumulated_frame["dtype"]), np.dtype(np.int32))
        self.assertIn("saturated_uint16_pixels", single_frame)
        self.assertNotIn("saturated_uint16_pixels", accumulated_frame)
        self.assertEqual(accumulated_frame["summed_output_maximum_dn"], accumulated_frame["maximum_dn"])
        self.assertEqual(accumulated_frame["theoretical_maximum_dn"], 3 * 65535)
        self.assertAlmostEqual(checks["accumulations"]["accumulated_to_single_mean_ratio"], 3.0, delta=0.1)
        self.assertEqual(checks["setpoint_persistence"]["requested_setpoint_celsius"], -120.0)
        self.assertEqual(checks["setpoint_persistence"]["setpoint_after_reopen_celsius"], -120.0)

    def test_frame_inspection_rejects_stuck_rows_and_columns(self):
        frames = np.random.default_rng(4).integers(
            100, 1000, size=(3, 100, 1340), dtype=np.uint16
        )
        frames[1, 17, :] = 400
        frames[2, :, 23] = 500
        result = inspect_frames(frames)
        self.assertFalse(result["passed"])
        self.assertEqual(result["stuck_rows"], [[1, 17]])
        self.assertEqual(result["stuck_columns"], [[2, 23]])

    def test_frame_inspection_rejects_zero_and_saturated_frames(self):
        frames = np.full((2, 100, 1340), 100, dtype=np.uint16)
        frames[0] = 0
        frames[1, 0, 0] = 65535
        result = inspect_frames(frames)
        self.assertEqual(result["all_zero_frames"], [0])
        self.assertEqual(result["saturated_frames"], [1])
        self.assertFalse(result["passed"])

    def test_accumulated_maximum_does_not_imply_constituent_frame_saturation(self):
        frame = np.array([[65534, 65535]], dtype=np.int32)
        result = _frame_summary(frame, shape=(1, 2), dtype=np.int32, accumulations=3)
        self.assertTrue(result["passed"], result)
        self.assertEqual(result["summed_output_maximum_dn"], 65535)
        self.assertEqual(result["theoretical_maximum_dn"], 3 * 65535)
        self.assertNotIn("saturated_uint16_pixels", result)

    @unittest.skipUnless(library_available(), "openst133 shared library not found; set OPENST133_LIBRARY")
    def test_simulated_cli_writes_passing_json_report(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "run"
            completed = subprocess.run(
                [
                    sys.executable,
                    "-m", MODULE,
                    "--simulate",
                    "--output",
                    str(output),
                ],
                cwd=ROOT,
                env={**os.environ, "PYTHONPATH": os.pathsep.join(filter(None, [str(ROOT / "python" / "src"), os.environ.get("PYTHONPATH")]))},
                text=True,
                capture_output=True,
                check=False,
                timeout=120,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)
            report = json.loads((output / "report.json").read_text())
            self.assertTrue(report["passed"])
            self.assertEqual(report["mode"], "simulate")
            self.assertEqual(report["driver"]["shape"], [100, 1340])
            self.assertTrue(report["driver"]["spe_header"]["passed"])
            generated = report["driver"]["settings"]
            self.assertTrue(generated["passed"])
            for name in ("gain", "shutter", "fvb", "binned_roi", "fvb_stream", "accumulations", "setpoint_persistence"):
                self.assertTrue(generated["checks"][name]["passed"], name)
            self.assertGreater(report["duration_seconds"], 0.0)
            for name in ("offset", "read_noise", "dark_current", "linearity", "gain"):
                quantity = report["characterization"]["quantities"][name]
                self.assertTrue(quantity["method"])
                self.assertTrue(quantity["source"].startswith("https://"))



class ConsistencyTests(unittest.TestCase):
    def frames(self, count, level=0.0, seed=0):
        rng = np.random.default_rng(seed)
        pattern = 3000.0 + 500.0 * rng.random((100, 1340))
        return np.stack([pattern + level * (i + 1) + rng.normal(0.0, 30.0, pattern.shape) for i in range(count)]), pattern

    def test_level_drift_passes_and_is_reported(self):
        frames, _ = self.frames(5, level=5.0)
        result = _pixel_consistency(frames[:2], frames[2:])
        self.assertTrue(result["passed"], result)
        self.assertGreater(abs(result["frames"][-1]["level_offset_dn"]), 5.0)
        self.assertTrue(_repeat_consistency(frames[:2], frames[3:])["passed"])

    def test_shifted_or_swapped_pixels_fail(self):
        frames, _ = self.frames(4)
        shifted = np.roll(frames[2:], 1, axis=2)
        self.assertFalse(_pixel_consistency(frames[:2], shifted)["passed"])
        swapped = frames[2:].astype(np.uint16).byteswap().astype(np.float64)
        self.assertFalse(_pixel_consistency(frames[:2], swapped)["passed"])
        self.assertFalse(_repeat_consistency(np.stack([frames[0], np.roll(frames[1], 1, axis=0)]), frames[2:])["passed"])

if __name__ == "__main__":
    unittest.main()
