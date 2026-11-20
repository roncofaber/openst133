# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT

import time
import unittest
from unittest import mock

from openst133.camera import SimulatedCamera
from openst133.viewer import AcquisitionController, ViewerApp



class TextVariable:
    def __init__(self, value):
        self.value = value

    def get(self):
        return self.value

    def set(self, value):
        self.value = value


class ViewerStreamGuardTest(unittest.TestCase):
    """While a stream runs, the Tk thread makes no native call that would wait for it; Stop returns at once."""

    def setUp(self):
        self.camera = SimulatedCamera(width=8, height=6, sleep_scale=1.0, seed=4)
        self.camera.open()
        self.addCleanup(self.camera.close)
        app = ViewerApp.__new__(ViewerApp)
        app.camera = self.camera
        app.controller = AcquisitionController(self.camera)
        app.root = mock.Mock()
        app.status_text = TextVariable("idle")
        app.temperature_text = TextVariable("-")
        app.exposure_text = TextVariable("0.5")
        app.shutter_text = TextVariable("closed")
        app.setpoint_text = TextVariable("-100")
        app.rate_text = TextVariable("100k")
        app.region_text = TextVariable("full")
        app.fvb = TextVariable(True)
        app.roi_text = [TextVariable(str(value)) for value in (0, 0, 8, 6)]
        app.binning_text = [TextVariable("1"), TextVariable("1")]
        self.app = app
        self.addCleanup(self._stop)

    def _stop(self):
        self.app.controller.stop()
        self.app.controller.join(5.0)

    def _wait_for_frame(self):
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            if any(kind == "frame" for kind, _ in self.app.controller.drain()):
                return
            time.sleep(0.005)
        self.fail("the stream delivered no frame")

    def test_stream_guard(self):
        self.app.controller.set_exposure(0.01)
        self.app.controller.start(True)
        self._wait_for_frame()
        self.assertTrue(self.app.controller.running)

        with mock.patch.object(self.camera, "temperature", wraps=self.camera.temperature) as temperature:
            self.app._poll_temperature()
            temperature.assert_not_called()
        self.app.root.after.assert_called_once()
        self.assertEqual(self.app.temperature_text.get(), "-")

        self.app._continuous = True
        self.app.rate_text.set("1 MHz")
        for action in (
            self.app.apply_exposure,
            self.app.apply_shutter,
            self.app.apply_setpoint,
            self.app.apply_rate,
            self.app.apply_readout,
        ):
            with self.subTest(action=action.__name__):
                started = time.monotonic()
                self.assertTrue(action())
                self.assertLess(time.monotonic() - started, 0.5)
                self.assertTrue(self.app.status_text.get().startswith("applying "))
        self.assertEqual(self.camera.exposure, 0.01)
        self.assertEqual(self.camera.binning, (1, 1))

        self.app.controller.join(5.0)
        self.assertFalse(self.app.controller.running)
        self.app.controller.drain()
        self.app._apply_pending()
        self.assertEqual(self.camera.exposure, 0.5)
        self.assertEqual(self.camera.shutter, "closed")
        self.assertTrue(self.app.controller.running, "the stream resumes after the settings are applied")
        self._wait_for_frame()

        self.app.stop()
        self.app.controller.join(5.0)
        self.assertFalse(self.app.controller.running)

        with mock.patch.object(self.camera, "temperature", wraps=self.camera.temperature) as temperature:
            self.app._poll_temperature()
            temperature.assert_called_once_with()
        self.assertTrue(self.app.temperature_text.get().endswith(" C"))
        self.app.fvb.set(False)
        self.assertTrue(self.app.apply_readout())
        self.app.rate_text.set("100k")
        self.assertTrue(self.app.apply_rate())
        self.assertEqual(self.camera.readout_rate, 100000)


if __name__ == "__main__":
    unittest.main()
