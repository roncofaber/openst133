# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT

import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import Mock, call

import numpy

from openst133 import spe
from openst133.camera import (
    AcquisitionAborted,
    CameraError,
    CameraUnavailable,
    RealCamera,
    SimulatedCamera,
    make_camera,
)
from openst133.viewer import (
    AcquisitionController,
    ViewerApp,
    acquire_to_file,
    adjust_box,
    COLORMAPS,
    box_handle,
    divisors,
    display_stride,
    format_temperature,
    frame_geometry,
    moved_region,
    region_box,
    resized_region,
    parse_exposure,
    preview_spectrum,
    selection_region,
    sensor_view,
    spectrum_positions,
    spectrum_selection,
    spectrum_x_values,
    spectrum_axis,
    to_ppm,
    to_rgb,
    write_spe,
)

NO_LIBRARY = {"OPENST133_LIBRARY": "/nonexistent/libopenst133.so"}
SOURCE = str(pathlib.Path(__file__).resolve().parents[1] / "src")


def library_available():
    try:
        from openst133._library import load

        load()
    except OSError:
        return False
    return True


class FakeClock:
    def __init__(self):
        self.now = 0.0

    def __call__(self):
        return self.now


class SimulatedCameraTest(unittest.TestCase):
    def test_frame_is_100b_sized_uint16_with_noise(self):
        with SimulatedCamera(sleep_scale=0.0, seed=1) as camera:
            first = camera.acquire()
            second = camera.acquire()
        self.assertEqual(first.shape, (100, 1340))
        self.assertEqual(first.dtype, numpy.uint16)
        self.assertFalse(numpy.array_equal(first, second))

    def test_fvb_returns_one_summed_spectrum(self):
        with SimulatedCamera(sleep_scale=0.0, seed=1) as camera:
            camera.set_full_vertical_binning()
            frame = camera.acquire()
            expected = numpy.clip(camera._last_sensor_frame.sum(axis=0, dtype=numpy.uint32), 0, 65535)
            self.assertEqual(frame.shape, (1, 1340))
            self.assertTrue(numpy.array_equal(frame[0], expected))
            self.assertEqual(camera.roi, (0, 0, 1340, 100))
            self.assertEqual(camera.binning, (1, 100))

    def test_roi_and_binning_sum_selected_pixels(self):
        camera = SimulatedCamera(width=6, height=4, sleep_scale=0.0)
        camera.set_roi(1, 1, 4, 2)
        camera.set_binning(2, 2)
        sensor = numpy.arange(24, dtype=numpy.uint16).reshape(4, 6)
        expected = numpy.array([[42, 50]], dtype=numpy.uint16)
        self.assertTrue(numpy.array_equal(camera._readout(sensor), expected))

    def test_readout_validation_preserves_state(self):
        camera = SimulatedCamera(width=8, height=6, sleep_scale=0.0)
        camera.set_roi(1, 1, 4, 4)
        camera.set_binning(2, 2)
        previous = camera.roi, camera.binning
        for roi in ((-1, 0, 4, 4), (0, 0, 0, 4), (0, 0, 9, 4), (0, 0, 4, 4.5)):
            with self.assertRaises(ValueError):
                camera.set_roi(*roi)
        for binning in ((0, 1), (3, 2), (2, 3), (1.5, 1)):
            with self.assertRaises(ValueError):
                camera.set_binning(*binning)
        self.assertEqual((camera.roi, camera.binning), previous)

    def test_temperature_cools_towards_target(self):
        clock = FakeClock()
        camera = SimulatedCamera(clock=clock, start_temperature=20.0, target_temperature=-100.0, cooling_time_constant=10.0)
        camera.open()
        self.assertAlmostEqual(camera.temperature(), 20.0)
        clock.now = 10.0
        middle = camera.temperature()
        clock.now = 1000.0
        self.assertTrue(-100.0 < middle < 20.0)
        self.assertAlmostEqual(camera.temperature(), -100.0, places=3)

    def test_exposure_validation_and_closed_camera(self):
        camera = SimulatedCamera(sleep_scale=0.0)
        with self.assertRaises(CameraError):
            camera.acquire()
        camera.open()
        for bad in (0, -1.0):
            with self.assertRaises(ValueError):
                camera.set_exposure(bad)
        camera.set_exposure(0.5)
        self.assertEqual(camera.exposure, 0.5)

    def test_abort_interrupts_acquire(self):
        camera = SimulatedCamera()
        camera.open()
        camera.set_exposure(30.0)
        errors = []
        import threading

        def run():
            try:
                camera.acquire()
            except AcquisitionAborted as error:
                errors.append(error)

        thread = threading.Thread(target=run)
        thread.start()
        camera.abort()
        thread.join(5.0)
        self.assertFalse(thread.is_alive())
        self.assertEqual(len(errors), 1)

    def test_real_camera_reports_not_implemented(self):
        self.assertIsInstance(make_camera(False), RealCamera)
        self.assertIsInstance(make_camera(True), SimulatedCamera)
        with self.assertRaises(CameraUnavailable) as context:
            RealCamera(library_path="/nonexistent/libopenst133.so").open()
        self.assertIn("cannot use the openst133 library", str(context.exception))


class ControllerTest(unittest.TestCase):
    def make(self):
        camera = SimulatedCamera(sleep_scale=0.0, seed=2)
        camera.open()
        return camera, AcquisitionController(camera)

    def test_single_acquisition_emits_one_frame_then_done(self):
        camera, controller = self.make()
        controller.set_exposure(0.1)
        controller.start(False)
        controller.join(5.0)
        kinds = [kind for kind, _ in controller.drain()]
        self.assertEqual(kinds, ["frame", "done"])
        self.assertEqual(controller.last_frame.shape, (100, 1340))

    def test_continuous_runs_until_stopped(self):
        camera = SimulatedCamera(sleep_scale=1.0, seed=3)
        camera.open()
        controller = AcquisitionController(camera)
        controller.set_exposure(0.01)
        controller.start(True)
        with self.assertRaises(RuntimeError):
            controller.start(False)
        import time

        deadline = time.monotonic() + 5.0
        frames = 0
        while frames < 3 and time.monotonic() < deadline:
            frames += sum(1 for kind, _ in controller.drain() if kind == "frame")
            time.sleep(0.01)
        controller.stop()
        controller.join(5.0)
        self.assertGreaterEqual(frames, 3)
        self.assertFalse(controller.running)
        self.assertEqual(controller.drain()[-1][0], "done")

    def test_error_is_reported_not_raised(self):
        camera = SimulatedCamera(sleep_scale=0.0)
        controller = AcquisitionController(camera)
        controller.start(False)
        controller.join(5.0)
        events = controller.drain()
        self.assertEqual(events[0][0], "error")
        self.assertEqual(events[-1][0], "done")

    def test_save_requires_a_frame(self):
        _, controller = self.make()
        with self.assertRaises(ValueError):
            controller.save("unused.spe")


class ViewerShutdownTest(unittest.TestCase):
    def test_close_waits_for_worker_before_closing_camera(self):
        app = object.__new__(ViewerApp)
        app.controller = Mock()
        app.camera = Mock()
        app.root = Mock()
        app.close()
        app.controller.assert_has_calls([call.stop(), call.join()])
        app.controller.join.assert_called_once_with()
        app.camera.close.assert_called_once_with()
        app.root.destroy.assert_called_once_with()


class DisplayTest(unittest.TestCase):
    def test_rgb_scaling_and_ppm(self):
        frame = numpy.array([[0, 500], [1000, 1000]], dtype=numpy.uint16)
        for colormap in ("gray", "heat"):
            rgb = to_rgb(frame, 0, 1000, colormap)
            self.assertEqual(rgb.shape, (2, 2, 3))
            self.assertEqual(rgb.dtype, numpy.uint8)
        gray = to_rgb(frame, 0, 1000)
        self.assertEqual(gray[0, 0].tolist(), [0, 0, 0])
        self.assertEqual(gray[1, 0].tolist(), [255, 255, 255])
        ppm = to_ppm(gray)
        self.assertTrue(ppm.startswith(b"P6 2 2 255\n"))
        self.assertEqual(len(ppm), len(b"P6 2 2 255\n") + 12)

    def test_sensor_view_places_the_readout_on_the_sensor(self):
        rgb = numpy.zeros((2, 3, 3), dtype=numpy.uint8)
        rgb[..., 0] = numpy.arange(6).reshape(2, 3) + 1
        view = sensor_view(rgb, (2, 2, 6, 2, 2, 1), 10, 5, 10, 5)
        self.assertEqual(view.shape, (5, 10, 3))
        self.assertEqual(view[2, :, 0].tolist(), [32, 32, 1, 1, 2, 2, 3, 3, 32, 32])
        self.assertEqual(view[3, :, 0].tolist(), [32, 32, 4, 4, 5, 5, 6, 6, 32, 32])
        self.assertTrue((view[[0, 1, 4]] == 32).all())
        stretched = sensor_view(rgb[:1, :2], (0, 0, 10, 5, 5, 5), 10, 5, 4, 2)
        self.assertEqual(stretched[..., 0].tolist(), [[1, 1, 2, 2], [1, 1, 2, 2]])

    def test_frame_geometry_uses_matching_metadata_only(self):
        frame = numpy.zeros((10, 200), dtype=numpy.uint16)
        metadata = {"roi": (0, 40, 400, 20), "binning": (2, 2)}
        self.assertEqual(frame_geometry(frame, metadata, 1340, 100), (0, 40, 400, 20, 2, 2))
        stale = {"roi": (0, 0, 1340, 100), "binning": (1, 1)}
        self.assertEqual(frame_geometry(frame, stale, 1340, 100), (0, 0, 1340, 100, 6, 10))
        self.assertEqual(frame_geometry(frame, None, 1340, 100), (0, 0, 1340, 100, 6, 10))

    def test_selection_region_maps_a_drag_to_whole_bins_inside_the_sensor(self):
        view, sensor = (670, 320), (1340, 100)
        self.assertEqual(selection_region((0, 128), (200, 192), view, sensor, (1, 1), False), (0, 40, 400, 20))
        self.assertEqual(selection_region((200, 192), (0, 128), view, sensor, (1, 1), False), (0, 40, 400, 20))
        self.assertEqual(selection_region((0, 128), (200, 192), view, sensor, (3, 3), False), (0, 40, 402, 21))
        self.assertEqual(selection_region((600, 300), (700, 340), view, sensor, (4, 4), False), (1200, 92, 140, 8))
        self.assertEqual(selection_region((10, 10), (11, 12), view, sensor, (1, 1), False), None)
        self.assertEqual(selection_region((0, 0), (670, 7), view, sensor, (1, 7), True), (0, 0, 1340, 3))

    def test_window_handles_move_and_resize_in_sensor_pixels(self):
        view, sensor = (670, 320), (1340, 100)
        region = (120, 50, 400, 20)
        box = region_box(region, view, sensor)
        self.assertEqual(box, (60.0, 160.0, 260.0, 224.0))
        self.assertEqual(box_handle((150, 190), box, view), "move")
        self.assertEqual(box_handle((62, 190), box, view), "w")
        self.assertEqual(box_handle((258, 222), box, view), "se")
        self.assertEqual(box_handle((10, 10), box, view), None)
        self.assertEqual(box_handle((300, 150), (0, 0, 670, 320), view), None)
        self.assertEqual(box_handle((1, 150), (0, 0, 670, 320), view), "w")
        self.assertEqual(adjust_box(box, "move", 1000, -1000, view), (470.0, 0.0, 670.0, 64.0))
        self.assertEqual(adjust_box(box, "e", -500, 0, view), (60.0, 160.0, 61.0, 224.0))
        self.assertEqual(moved_region(region, 50, 32, view, sensor), (220, 60, 400, 20))
        self.assertEqual(moved_region(region, 5000, 5000, view, sensor), (940, 80, 400, 20))
        self.assertEqual(resized_region(region, "e", (60, 160, 310, 224), view, sensor, (1, 1), False), (120, 50, 500, 20))
        self.assertEqual(resized_region(region, "w", (40, 160, 260, 224), view, sensor, (3, 1), False), (79, 50, 441, 20))
        self.assertEqual(resized_region(region, "n", (60, 120, 260, 224), view, sensor, (1, 4), False), (120, 38, 400, 32))
        self.assertEqual(resized_region(region, "s", (60, 160, 260, 400), view, sensor, (1, 7), True), (120, 50, 400, 50))

    def test_spectrum_selection_maps_columns_through_the_readout(self):
        positions = spectrum_positions(400, None, 640)
        self.assertEqual(positions[0], 64)
        self.assertEqual(positions[-1], 620)
        region = spectrum_selection(positions[10], positions[109], positions, (200, 0, 800, 100, 2, 100))
        self.assertEqual(region, (220, 0, 200, 100))
        self.assertIsNone(spectrum_selection(100, 101, positions, (200, 0, 800, 100, 2, 100)))
        columns = 200 + numpy.arange(400) * 2
        values, unit = spectrum_x_values(400, None, columns)
        self.assertEqual((values[0], values[-1], unit), (200.0, 998.0, "pixel"))
        self.assertTrue(numpy.allclose(spectrum_positions(400, None, 640, columns), positions))
        self.assertEqual(spectrum_x_values(3, [500.0, 501.0, 502.0], [0, 1, 2])[1], "nm")
        wavelengths = numpy.linspace(400.0, 800.0, 400) ** 1.1
        curved = spectrum_positions(400, wavelengths, 640)
        self.assertEqual(spectrum_selection(curved[50], curved[60], curved, (0, 0, 400, 100, 1, 100)), (50, 0, 11, 100))

    def test_every_colormap_maps_levels_to_rgb(self):
        frame = numpy.arange(12, dtype=numpy.uint16).reshape(3, 4)
        for name in COLORMAPS:
            with self.subTest(name=name):
                rgb = to_rgb(frame, colormap=name)
                self.assertEqual((rgb.shape, rgb.dtype), ((3, 4, 3), numpy.uint8))
        self.assertEqual(to_rgb(numpy.array([[0, 1]]), 0, 1, "viridis").tolist(), [[[68, 1, 84], [253, 231, 37]]])

    def test_bin_choices_are_divisors(self):
        self.assertEqual(divisors(1340)[:8], [1, 2, 4, 5, 10, 20, 67, 134])
        self.assertEqual(divisors(100)[-1], 100)

    def test_preview_spectrum_sums_rows_of_a_covering_image(self):
        frame = numpy.arange(40, dtype=numpy.uint16).reshape(4, 10)
        preview = preview_spectrum(frame, (0, 0, 10, 4, 1, 1), (2, 1, 4, 2, 2, 2))
        expected = frame[1:3, 2:6].sum(axis=0).reshape(2, 2).sum(axis=1)
        self.assertEqual(preview.tolist(), [expected.tolist()])
        binned = frame[:, ::2] + frame[:, 1::2]
        self.assertEqual(preview_spectrum(binned, (0, 0, 10, 4, 2, 1), (0, 0, 10, 4, 2, 4)).tolist(), [binned.sum(axis=0).tolist()])
        self.assertIsNone(preview_spectrum(frame, (0, 0, 10, 4, 1, 1), (8, 0, 4, 4, 1, 4)))
        self.assertIsNone(preview_spectrum(binned, (0, 0, 10, 4, 2, 1), (1, 0, 4, 4, 1, 4)))

    def test_constant_frame_does_not_divide_by_zero(self):
        rgb = to_rgb(numpy.full((4, 4), 7, dtype=numpy.uint16))
        self.assertEqual(rgb.shape, (4, 4, 3))

    def test_helpers(self):
        self.assertEqual(display_stride(1340), 2)
        self.assertEqual(display_stride(800), 1)
        self.assertEqual(parse_exposure("0.25"), 0.25)
        for bad in ("abc", "0", "-1", "inf", ""):
            with self.assertRaises(ValueError):
                parse_exposure(bad)
        self.assertEqual(format_temperature(-119.96), "-120.0 C")


@unittest.skipUnless(library_available(), "openst133 shared library not found")
class SpeSaveTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="ost-viewer-")
        self.addCleanup(self.directory.cleanup)

    def path(self, name):
        return os.path.join(self.directory.name, name)

    def test_write_round_trip_multiple_shapes(self):
        for shape in ((100, 1340), (3, 5), (1, 1)):
            frames = [numpy.arange(shape[0] * shape[1], dtype=numpy.uint16).reshape(shape) + index for index in range(2)]
            path = self.path(f"{shape[0]}x{shape[1]}.spe")
            write_spe(path, frames, 0.25)
            info, data = spe.read(path)
            self.assertEqual((info.height, info.width, info.frame_count), (shape[0], shape[1], 2))
            self.assertEqual(info.data_type, 3)
            self.assertAlmostEqual(info.exposure_seconds, 0.25)
            for written, read_back in zip(frames, data):
                self.assertTrue(numpy.array_equal(written, read_back))

    def test_spectrum_axis_reads_generated_spe_wavelength_mapping(self):
        path = self.path("calibrated.spe")
        write_spe(path, [numpy.zeros((1, 3), dtype=numpy.uint16)], 0.1)
        with spe.SpeFile(path) as original:
            footer = original.xml_footer
        calibrated_footer = footer.replace(
            'type="Frame"', 'type="Frame" calibrations="spectrum-calibration"', 1
        ).replace(
            "</SpeFormat>",
            '<Calibrations><WavelengthMapping id="spectrum-calibration">'
            '<Wavelength>400.0,401.5,403.0</Wavelength>'
            '<WavelengthError>400.0 0.1,401.5 0.1,403.0 0.2</WavelengthError>'
            "</WavelengthMapping></Calibrations></SpeFormat>",
        )
        self.assertNotEqual(calibrated_footer, footer)
        raw = pathlib.Path(path).read_bytes()
        footer_bytes = footer.encode("utf-8")
        self.assertEqual(raw.count(footer_bytes), 1)
        pathlib.Path(path).write_bytes(raw.replace(footer_bytes, calibrated_footer.encode("utf-8")))
        axis, unit = spectrum_axis(path)
        self.assertEqual(unit, "nm")
        self.assertEqual(axis.tolist(), [400.0, 401.5, 403.0])
        axis, unit = spectrum_axis(path, pixel_count=2)
        self.assertEqual(unit, "pixel")
        self.assertEqual(axis.tolist(), [0.0, 1.0])

    def test_spectrum_axis_without_mapping_uses_pixels(self):
        path = self.path("pixels.spe")
        write_spe(path, [numpy.zeros((1, 3), dtype=numpy.uint16)], 0.1)
        axis, unit = spectrum_axis(path)
        self.assertEqual(unit, "pixel")
        self.assertEqual(axis.tolist(), [0.0, 1.0, 2.0])

    def test_write_rejects_bad_input(self):
        with self.assertRaises(ValueError):
            write_spe(self.path("a.spe"), [], 1.0)
        with self.assertRaises(ValueError):
            write_spe(self.path("a.spe"), [numpy.zeros((2, 2)), numpy.zeros((3, 2))], 1.0)

    def test_acquire_to_file_reads_back(self):
        path = self.path("acq.spe")
        acquire_to_file(SimulatedCamera(sleep_scale=0.0), 0.1, path, frames=2)
        info, data = spe.read(path)
        self.assertEqual((info.height, info.width, info.frame_count), (100, 1340, 2))
        self.assertEqual(data[0].shape, (100, 1340))

    def test_controller_save(self):
        camera = SimulatedCamera(sleep_scale=0.0)
        camera.open()
        controller = AcquisitionController(camera)
        controller.set_exposure(0.2)
        controller.start(False)
        controller.join(5.0)
        path = self.path("saved.spe")
        controller.save(path)
        info, data = spe.read(path)
        self.assertAlmostEqual(info.exposure_seconds, 0.2, places=5)
        self.assertTrue(numpy.array_equal(data[0], controller.last_frame))

    def test_cli_acquire(self):
        path = self.path("cli.spe")
        result = subprocess.run(
            [sys.executable, "-m", "openst133", "acquire", "--simulate", "--exposure", "0.1", "--out", path],
            env={**os.environ, "PYTHONPATH": SOURCE},
            capture_output=True,
            text=True,
            timeout=60,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        info, data = spe.read(path)
        self.assertEqual(data[0].shape, (100, 1340))


class CommandLineTest(unittest.TestCase):
    def run_cli(self, *arguments, prefix=(), extra_environment=None):
        return subprocess.run(
            [*prefix, sys.executable, "-m", "openst133", *arguments],
            env={**os.environ, "PYTHONPATH": SOURCE, **(extra_environment or {})},
            capture_output=True,
            text=True,
            timeout=60,
        )

    def test_view_without_simulate_reports_unavailable(self):
        result = self.run_cli("view", extra_environment=NO_LIBRARY)
        self.assertEqual(result.returncode, 2)
        self.assertIn("cannot use the openst133 library", result.stderr)
        self.assertNotIn("Traceback", result.stderr)

    def test_acquire_without_simulate_reports_unavailable(self):
        result = self.run_cli("acquire", "--out", os.devnull, extra_environment=NO_LIBRARY)
        self.assertEqual(result.returncode, 2)
        self.assertIn("cannot use the openst133 library", result.stderr)

    @unittest.skipUnless(shutil.which("xvfb-run"), "xvfb-run not available")
    def test_spectrum_and_image_rendering(self):
        script = (
            "import numpy\n"
            "import tkinter\n"
            "from openst133.camera import SimulatedCamera\n"
            "from openst133.viewer import ViewerApp\n"
            "root = tkinter.Tk()\n"
            "camera = SimulatedCamera(sleep_scale=0.0)\n"
            "camera.open()\n"
            "app = ViewerApp(root, camera)\n"
            "app.show(numpy.zeros((1, 1340), dtype=numpy.uint16))\n"
            "assert app.last_display == 'spectrum', app.last_display\n"
            "assert app.last_unit == 'pixel', app.last_unit\n"
            "app.show(numpy.zeros((1, 1340), dtype=numpy.uint16), numpy.linspace(400, 800, 1340))\n"
            "assert app.last_unit == 'nm', app.last_unit\n"
            "app.show(numpy.zeros((100, 1340), dtype=numpy.uint16))\n"
            "assert app.last_display == 'image', app.last_display\n"
            "app.close()\n"
            "print('spectrum and image ok')\n"
        )
        result = subprocess.run(
            ["xvfb-run", "-a", sys.executable, "-c", script],
            env={**os.environ, "PYTHONPATH": SOURCE},
            capture_output=True,
            text=True,
            timeout=60,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("spectrum and image ok", result.stdout)

    @unittest.skipUnless(shutil.which("xvfb-run"), "xvfb-run not available")
    def test_view_starts_under_xvfb(self):
        result = self.run_cli(
            "view", "--simulate", "--close-after", "1.5", prefix=("xvfb-run", "-a")
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("Traceback", result.stderr)

    @unittest.skipUnless(shutil.which("xvfb-run"), "xvfb-run not available")
    def test_gui_acquire_shows_frame_under_xvfb(self):
        script = (
            "import tkinter\n"
            "from openst133.camera import SimulatedCamera\n"
            "from openst133.viewer import ViewerApp\n"
            "root = tkinter.Tk()\n"
            "camera = SimulatedCamera(sleep_scale=0.0, seed=1)\n"
            "camera.open()\n"
            "app = ViewerApp(root, camera)\n"
            "app.exposure_text.set('0.05')\n"
            "app.acquire_once()\n"
            "app.controller.join(5)\n"
            "import time\n"
            "deadline = time.monotonic() + 5\n"
            "while app.frames_shown == 0 and time.monotonic() < deadline:\n"
            "    root.update()\n"
            "    time.sleep(0.01)\n"
            "assert app.frames_shown == 1, app.frames_shown\n"
            "assert app.temperature_text.get().endswith(' C'), app.temperature_text.get()\n"
            "app.close()\n"
            "print('gui ok')\n"
        )
        result = subprocess.run(
            ["xvfb-run", "-a", sys.executable, "-c", script],
            env={**os.environ, "PYTHONPATH": SOURCE},
            capture_output=True,
            text=True,
            timeout=60,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("gui ok", result.stdout)


if __name__ == "__main__":
    unittest.main()
