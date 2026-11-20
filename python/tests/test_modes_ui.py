# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT

import os
import pathlib
import shutil
import subprocess
import sys
import unittest
from unittest import mock

from openst133.camera import (
    CameraUnavailable,
    RealCamera,
    SimulatedCamera,
)
from openst133.device import NativeCamera, NativeError, OST_ERR_UNSUPPORTED, Readout
from openst133.viewer import AcquisitionController, ViewerApp


SOURCE = str(pathlib.Path(__file__).resolve().parents[1] / "src")


def cli_path():
    configured = os.environ.get("OPENST133_CLI")
    if configured:
        return configured
    library = os.environ.get("OPENST133_LIBRARY")
    if not library:
        return None
    candidate = pathlib.Path(library).resolve().parents[1] / "ost"
    return str(candidate) if candidate.exists() else None


OST = cli_path()


class RealCameraModesTest(unittest.TestCase):
    def setUp(self):
        self.camera = RealCamera()
        self.native = object()
        self.camera._native = self.native
        self.readout = Readout(100, 20, 200, 40, 2, 1)

    def test_roi_retains_binning_and_forwards_the_readout(self):
        with (
            mock.patch.object(NativeCamera, "get_readout", return_value=self.readout) as get,
            mock.patch.object(NativeCamera, "set_readout") as set_readout,
        ):
            self.camera.set_roi(1, 2, 300, 50)
        get.assert_called_once_with(self.native)
        set_readout.assert_called_once_with(self.native, Readout(1, 2, 300, 50, 2, 1))

    def test_binning_retains_roi_and_forwards_the_readout(self):
        with (
            mock.patch.object(NativeCamera, "get_readout", return_value=self.readout) as get,
            mock.patch.object(NativeCamera, "set_readout") as set_readout,
        ):
            self.camera.set_binning(4, 2)
        get.assert_called_once_with(self.native)
        set_readout.assert_called_once_with(self.native, Readout(100, 20, 200, 40, 4, 2))

    def test_readout_forwards_region_and_binning_in_one_call(self):
        with (
            mock.patch.object(NativeCamera, "get_readout") as get,
            mock.patch.object(NativeCamera, "set_readout") as set_readout,
        ):
            self.camera.set_readout(200, 0, 800, 100, 2, 100)
        get.assert_not_called()
        set_readout.assert_called_once_with(self.native, Readout(200, 0, 800, 100, 2, 100))

    def test_fvb_and_rate_map_to_their_native_calls(self):
        with mock.patch.object(NativeCamera, "set_full_vertical_binning") as fvb:
            self.camera.set_full_vertical_binning()
        fvb.assert_called_once_with(self.native)
        with mock.patch.object(NativeCamera, "set_readout_rate") as rate:
            self.camera.set_readout_rate(100000)
        rate.assert_called_once_with(self.native, 100000)

    def test_native_refusal_text_is_unchanged(self):
        refusal = "readout rate unsupported: choose 1000000 Hz or 100000 Hz"
        with mock.patch.object(
            NativeCamera,
            "set_readout_rate",
            side_effect=NativeError(OST_ERR_UNSUPPORTED, refusal),
        ):
            with self.assertRaises(CameraUnavailable) as raised:
                self.camera.set_readout_rate(100000)
        self.assertEqual(str(raised.exception), refusal)

    def test_acquisition_buffer_follows_native_readout(self):
        readout = Readout(100, 20, 200, 40, 2, 2)
        with (
            mock.patch.object(NativeCamera, "get_readout", return_value=readout),
            mock.patch.object(NativeCamera, "acquire_into", return_value=4000) as acquire,
        ):
            frame = self.camera.acquire()
        self.assertEqual(frame.shape, (20, 100))
        acquire.assert_called_once()
        self.assertIs(acquire.call_args.args[1], frame)

    def test_stream_buffer_follows_native_readout(self):
        readout = Readout(0, 0, 8, 6, 1, 6)
        stats = mock.Mock(frames_delivered=1, partial_bytes_discarded=0, host_stalls=0)

        def stream(_count, on_frame):
            on_frame(0, bytes(range(16)))
            return stats

        self.native = mock.Mock()
        self.native.get_readout.return_value = readout
        self.camera._native = self.native
        with mock.patch.object(self.native, "stream", side_effect=stream):
            frames = list(self.camera.stream(frames=1))
        self.assertEqual(len(frames), 1)
        self.assertEqual(frames[0].shape, (1, 8))


class TextVariable:
    def __init__(self, value):
        self.value = value

    def get(self):
        return self.value

    def set(self, value):
        self.value = value


class ViewerSettingsTest(unittest.TestCase):
    def setUp(self):
        self.camera = SimulatedCamera(width=8, height=6, sleep_scale=0.0, seed=1)
        self.camera.open()
        self.addCleanup(self.camera.close)
        self.app = ViewerApp.__new__(ViewerApp)
        self.app.camera = self.camera
        self.app.controller = AcquisitionController(self.camera)
        self.app.status_text = TextVariable("idle")
        self.app.shutter_text = TextVariable("normal")
        self.app.setpoint_text = TextVariable("")
        self.app.rate_text = TextVariable("1M")
        self.app.region_text = TextVariable("full")
        self.app.fvb = TextVariable(False)
        self.app.roi_text = [TextVariable(str(value)) for value in (0, 0, 8, 6)]
        self.app.binning_text = [TextVariable("1"), TextVariable("1")]

    def readout(self, region="full", roi=(0, 0, 8, 6), binning=(1, 1), fvb=False):
        self.app.region_text.set(region)
        for variable, value in zip(self.app.roi_text, roi):
            variable.set(str(value))
        for variable, value in zip(self.app.binning_text, binning):
            variable.set(str(value))
        self.app.fvb.set(fvb)
        return self.app.apply_readout()

    def test_controls_combine_region_binning_and_fvb(self):
        self.assertTrue(self.readout())
        self.assertEqual((self.camera.roi, self.camera.binning), ((0, 0, 8, 6), (1, 1)))
        self.assertTrue(self.readout(fvb=True))
        self.assertEqual((self.camera.roi, self.camera.binning), ((0, 0, 8, 6), (1, 6)))
        self.assertTrue(self.readout("custom", (2, 0, 4, 6), (2, 99), fvb=True))
        self.assertEqual((self.camera.roi, self.camera.binning), ((2, 0, 4, 6), (2, 6)))
        self.assertEqual(self.camera.acquire().shape, (1, 2))
        self.assertEqual(self.app.status_text.get(), "readout: 2 x 1 spectrum")
        self.assertTrue(self.readout("custom", (1, 1, 4, 2), (2, 2)))
        self.assertEqual(self.camera.acquire().shape, (1, 2))
        self.assertTrue(self.readout(binning=(2, 3)))
        self.assertEqual((self.camera.roi, self.camera.binning), ((0, 0, 8, 6), (2, 3)))

    def test_invalid_combination_is_reported_and_not_sent(self):
        self.assertTrue(self.readout("custom", (1, 1, 4, 2)))
        cases = (
            (("custom", (0, 0, 9, 6), (1, 1)), "x + w = 9 exceeds the sensor width 8"),
            (("custom", (0, 0, 6, 6), (4, 1)), "w = 6 is not divisible by bin x = 4"),
            (("full", (0, 0, 8, 6), (1, 4)), "h = 6 is not divisible by bin y = 4"),
            (("custom", ("a", 0, 8, 6), (1, 1)), "region x, y, w and h must be whole numbers"),
            (("full", (0, 0, 8, 6), (0, 1)), "bin x and y must be at least 1"),
        )
        for arguments, message in cases:
            with self.subTest(message=message):
                with mock.patch.object(self.camera, "set_readout") as set_readout:
                    self.assertFalse(self.readout(*arguments))
                set_readout.assert_not_called()
                self.assertEqual(self.app.status_text.get(), message)
        self.assertEqual(self.camera.roi, (1, 1, 4, 2))

    def test_slow_rate_requires_the_full_unbinned_frame(self):
        self.assertTrue(self.readout())
        self.app.rate_text.set("100k")
        self.assertTrue(self.app.apply_rate())
        self.assertEqual(self.camera.readout_rate, 100000)
        with mock.patch.object(self.camera, "set_readout") as set_readout:
            self.assertFalse(self.readout(fvb=True))
        set_readout.assert_not_called()
        self.assertIn("100 kHz", self.app.status_text.get())
        self.app.rate_text.set("1M")
        self.assertTrue(self.app.apply_rate())
        self.assertTrue(self.readout(fvb=True))
        self.app.rate_text.set("100k")
        self.assertFalse(self.app.apply_rate())
        self.assertEqual(self.camera.readout_rate, 1000000)

    def test_controls_select_other_settings(self):
        self.app.shutter_text.set("closed")
        self.assertTrue(self.app.apply_shutter())
        self.app.setpoint_text.set("-100")
        self.assertTrue(self.app.apply_setpoint())

    def test_every_setting_control_shows_refusal_verbatim(self):
        refusal = "native refusal: requested combination is unsupported"
        actions = (
            ("set_shutter", self.app.apply_shutter),
            ("set_temperature", self.app.apply_setpoint),
            ("set_readout_rate", self.app.apply_rate),
            ("set_readout", self.app.apply_readout),
        )
        self.app.setpoint_text.set("-100")
        self.app.fvb.set(True)
        for method, action in actions:
            with self.subTest(method=method):
                with mock.patch.object(self.camera, method, side_effect=CameraUnavailable(refusal)):
                    self.assertFalse(action())
                self.assertEqual(self.app.status_text.get(), refusal)

    def test_full_frame_restores_roi_and_binning(self):
        self.camera.set_roi(1, 1, 4, 2)
        self.camera.set_binning(2, 2)
        self.assertTrue(self.app.apply_readout())
        self.assertEqual(self.camera.roi, (0, 0, 8, 6))
        self.assertEqual(self.camera.binning, (1, 1))


@unittest.skipIf(OST is None, "ost command not found")
class CommandLineModesTest(unittest.TestCase):
    def run_ost(self, *arguments):
        return subprocess.run(
            [OST, "acquire", *arguments],
            capture_output=True,
            text=True,
            timeout=30,
        )

    def test_help_lists_mode_flags(self):
        result = self.run_ost("--help")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("--policy", result.stdout)
        for option in ("--setpoint", "--fvb", "--roi", "--bin", "--rate"):
            self.assertIn(option, result.stdout)

    def test_invalid_mode_syntax_stops_before_usb(self):
        cases = (
            (("--setpoint", "nan"), "--setpoint requires"),
            (("--roi", "1,2,3"), "--roi requires"),
            (("--bin", "2X2"), "--bin requires"),
            (("--rate", "100K"), "--rate requires"),
        )
        for arguments, message in cases:
            with self.subTest(arguments=arguments):
                result = self.run_ost(*arguments, "--replay", "/nonexistent/capture.pcap")
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(message, result.stderr)

    def test_real_camera_open_reaches_the_hardware_guard(self):
        result = subprocess.run(
            [OST, "acquire", "--exposure", "0.1", "--out", "/nonexistent/frame.spe"],
            capture_output=True,
            text=True,
            timeout=30,
            env={**os.environ, "OPENST133_NO_HARDWARE": "1"},
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("OPENST133_NO_HARDWARE=1 forbids opening real hardware", result.stderr)


@unittest.skipUnless(shutil.which("xvfb-run"), "xvfb-run not available")
class ViewerWindowModesTest(unittest.TestCase):
    def test_controls_acquire_and_render_fvb_under_xvfb(self):
        script = (
            "import tkinter\n"
            "from openst133.camera import SimulatedCamera\n"
            "from openst133.viewer import ViewerApp\n"
            "root = tkinter.Tk()\n"
            "camera = SimulatedCamera(width=8, height=6, sleep_scale=0.0, seed=1)\n"
            "camera.open()\n"
            "app = ViewerApp(root, camera)\n"
            "assert app.output_text.get() == 'Output 8 x 6 image', app.output_text.get()\n"
            "app.fvb.set(True)\n"
            "assert app.output_text.get() == 'Output 8 x 1 spectrum', app.output_text.get()\n"
            "assert app.rate_menu['menu'].entrycget('100 kHz', 'state') == 'disabled'\n"
            "assert app.apply_readout()\n"
            "app.shutter_text.set('closed')\n"
            "assert app.apply_shutter()\n"
            "app.setpoint_text.set('-100')\n"
            "assert app.apply_setpoint()\n"
            "app.acquire_once()\n"
            "app.controller.join(5)\n"
            "import time\n"
            "deadline = time.monotonic() + 5\n"
            "while app.frames_shown == 0 and time.monotonic() < deadline:\n"
            "    root.update()\n"
            "    time.sleep(0.01)\n"
            "assert app.last_display == 'spectrum', app.last_display\n"
            "assert app.controller.last_frame.shape == (1, 8)\n"
            "app.close()\n"
            "print('mode controls ok')\n"
        )
        result = subprocess.run(
            ["xvfb-run", "-a", sys.executable, "-c", script],
            env={**os.environ, "PYTHONPATH": SOURCE},
            capture_output=True,
            text=True,
            timeout=60,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("mode controls ok", result.stdout)


    def test_dragging_selects_a_region_under_xvfb(self):
        script = (
            "import numpy, tkinter\n"
            "from openst133.camera import SimulatedCamera\n"
            "from openst133.viewer import ViewerApp\n"
            "root = tkinter.Tk()\n"
            "camera = SimulatedCamera(width=1340, height=100, sleep_scale=0.0, seed=1)\n"
            "camera.open()\n"
            "app = ViewerApp(root, camera)\n"
            "root.update()\n"
            "def drag(x0, y0, x1, y1):\n"
            "    app.canvas.event_generate('<ButtonPress-1>', x=x0, y=y0)\n"
            "    app.canvas.event_generate('<B1-Motion>', x=x1, y=y1)\n"
            "    assert app.canvas.find_withtag('selection'), 'no selection rectangle while dragging'\n"
            "    app.canvas.event_generate('<ButtonRelease-1>', x=x1, y=y1)\n"
            "    root.update()\n"
            "m = app.VIEW_MARGIN\n"
            "app.canvas.config(width=670 + 2 * m)\n"
            "root.update()\n"
            "texts = lambda: [app.canvas.itemcget(i, 'text') for i in app.canvas.find_all() if app.canvas.type(i) == 'text']\n"
            "assert texts() == ['No frame available yet'], texts()\n"
            "app.region_text.set('custom')\n"
            "for v, n in zip(app.roi_text, (100, 10, 400, 40)):\n"
            "    v.set(str(n))\n"
            "assert app.apply_readout()\n"
            "assert texts() == ['No frame available yet'] and app.canvas.find_withtag('region'), texts()\n"
            "app.region_text.set('full')\n"
            "assert app.apply_readout()\n"
            "assert app._view_size() == (670, 320), app._view_size()\n"
            "assert root.nametowidget(root.cget('menu')).index('end') == 3, 'menu bar missing'\n"
            "def frame():\n"
            "    app.controller.last_frame_metadata = camera.spe_metadata()\n"
            "    app.controller.last_frame = camera.acquire()\n"
            "    app.show(app.controller.last_frame)\n"
            "    root.update()\n"
            "frame()\n"
            "assert not app.canvas.find_withtag('region'), 'the full-sensor window should be hidden'\n"
            "drag(m + 10, m + 128, m + 210, m + 192)\n"
            "assert (camera.roi, camera.binning) == ((20, 40, 400, 20), (1, 1)), (camera.roi, camera.binning)\n"
            "assert app.canvas.find_withtag('region'), 'the active window is not drawn'\n"
            "assert app.region_text.get() == 'custom'\n"
            "assert not app.canvas.find_withtag('selection')\n"
            "frame()\n"
            "drag(m + 100, m + 160, m + 150, m + 192)\n"
            "assert camera.roi == (120, 50, 400, 20), camera.roi\n"
            "frame()\n"
            "drag(m + 260, m + 200, m + 310, m + 200)\n"
            "assert camera.roi == (120, 50, 500, 20), camera.roi\n"
            "frame()\n"
            "drag(m + 200, m + 160, m + 200, m + 120)\n"
            "assert camera.roi == (120, 38, 500, 32), camera.roi\n"
            "frame()\n"
            "drag(2, m + 100, app.canvas.winfo_width() - 2, m + 200)\n"
            "assert camera.roi[0] == 0 and camera.roi[2] == 1340, camera.roi\n"
            "assert app.reset_region()\n"
            "frame()\n"
            "drag(m + 300, m + 100, -400, 900)\n"
            "assert camera.roi == (0, 31, 600, 69), camera.roi\n"
            "app.region_text.set('full')\n"
            "assert app.apply_readout()\n"
            "frame()\n"
            "shown = app.frames_shown\n"
            "app.fvb.set(True)\n"
            "assert app.apply_readout()\n"
            "assert app.last_display == 'spectrum' and app.status_text.get().startswith('preview'), app.status_text.get()\n"
            "assert app.frames_shown == shown, 'a preview is not a frame'\n"
            "assert app.controller.last_frame.shape == (100, 1340), 'a preview must not replace the saved frame'\n"
            "app.fvb.set(False)\n"
            "assert app.apply_readout()\n"
            "assert app.last_display == 'image'\n"
            "image = app._image\n"
            "app.colormap.set('viridis')\n"
            "assert app._image is not image, 'colormap change did not redraw'\n"
            "assert app.readout_fields['bin'][0].cget('values').split()[:4] == ['1', '2', '4', '5']\n"
            "app.fvb.set(True)\n"
            "assert app.apply_readout()\n"
            "frame()\n"
            "assert app.last_display == 'spectrum'\n"
            "assert not app.canvas.find_withtag('region')\n"
            "positions = app._display[2]\n"
            "drag(round(positions[100]), 10, round(positions[179]), 10)\n"
            "x, y, w, h = camera.roi\n"
            "assert abs(x - 100) <= 1 and abs(w - 80) <= 2 and (y, h) == (0, 100), camera.roi\n"
            "assert camera.binning == (1, 100), camera.binning\n"
            "for unused in range(2):\n"
            "    app.canvas.event_generate('<ButtonPress-1>', x=5, y=5)\n"
            "    app.canvas.event_generate('<ButtonRelease-1>', x=5, y=5)\n"
            "root.update()\n"
            "assert app.region_text.get() == 'full' and camera.roi == (0, 0, 1340, 100), camera.roi\n"
            "app.close()\n"
            "print('drag ok')\n"
        )
        result = subprocess.run(
            ["xvfb-run", "-a", sys.executable, "-c", script],
            env={**os.environ, "PYTHONPATH": SOURCE},
            capture_output=True,
            text=True,
            timeout=60,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("drag ok", result.stdout)

    def test_typed_values_apply_after_a_pause_under_xvfb(self):
        script = (
            "import time, tkinter\n"
            "from unittest import mock\n"
            "from openst133.camera import SimulatedCamera\n"
            "from openst133.viewer import ViewerApp\n"
            "root = tkinter.Tk()\n"
            "camera = SimulatedCamera(width=1340, height=100, sleep_scale=0.0, seed=1)\n"
            "camera.open()\n"
            "app = ViewerApp(root, camera)\n"
            "def wait(ms):\n"
            "    end = time.monotonic() + ms / 1000\n"
            "    while time.monotonic() < end:\n"
            "        root.update()\n"
            "        time.sleep(0.01)\n"
            "field = app.readout_fields['bin'][0]\n"
            "field.focus_force()\n"
            "root.update()\n"
            "field.delete(0, 'end')\n"
            "with mock.patch.object(camera, 'set_readout', wraps=camera.set_readout) as sent:\n"
            "    for key in '67':\n"
            "        field.insert('end', key)\n"
            "        field.event_generate('<KeyRelease>', keysym=key)\n"
            "        wait(100)\n"
            "    assert camera.binning == (1, 1), 'applied while still typing'\n"
            "    wait(app.TYPING_PAUSE_MS + 300)\n"
            "    assert camera.binning == (67, 1), camera.binning\n"
            "    assert sent.call_count == 1, sent.call_args_list\n"
            "app.close()\n"
            "print('typing ok')\n"
        )
        result = subprocess.run(
            ["xvfb-run", "-a", sys.executable, "-c", script],
            env={**os.environ, "PYTHONPATH": SOURCE},
            capture_output=True,
            text=True,
            timeout=60,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("typing ok", result.stdout)

if __name__ == "__main__":
    unittest.main()
