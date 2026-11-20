# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT

import os
import time

import h5py
import numpy
import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from ScopeFoundry import BaseMicroscopeApp  # noqa: E402

from openst133 import spe  # noqa: E402
from openst133.camera import CameraUnavailable, RealCamera  # noqa: E402
from ScopeFoundryHW.openst133 import OpenST133HW, OpenST133Measure, close_app  # noqa: E402


class App(BaseMicroscopeApp):
    name = "openst133_test_app"

    def setup(self):
        self.add_hardware(OpenST133HW(self))
        self.add_measurement(OpenST133Measure(self))


@pytest.fixture
def app(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    app = App([])
    app.settings["save_dir"] = str(tmp_path)
    app.hardware["openst133"].settings["sim_speed"] = 0.0
    yield app
    close_app(app)


def wait_done(measure, timeout=20):
    deadline = time.time() + timeout
    while measure.is_measuring() and time.time() < deadline:
        time.sleep(0.02)
        app_events()
    app_events()
    assert not measure.is_measuring()


def app_events():
    from qtpy import QtWidgets

    QtWidgets.QApplication.processEvents()


def test_fvb_acquisition_saves_spectrum_wavelength_and_spe(app, tmp_path):
    hw = app.hardware["openst133"]
    measure = app.measurements["openst133_acquire"]
    hw.settings["connected"] = True
    assert hw.camera is not None
    hw.settings["exposure"] = 0.02
    hw.settings["full_vertical_binning"] = True
    hw.settings["temperature_setpoint"] = -90.0
    measure.settings["mode"] = "single"
    measure.settings["n_frames"] = 3
    measure.settings["save_spe"] = True
    measure.settings["wavelength_start"] = 400.0
    measure.settings["wavelength_step"] = 0.1
    measure.start()
    wait_done(measure)
    assert hw.camera.roi == (0, 0, 1340, 100)
    assert hw.camera.binning == (1, 100)
    assert len(measure.saved_paths) == 2
    h5_path, spe_path = measure.saved_paths
    with h5py.File(h5_path, "r") as f:
        group = f["measurement/openst133_acquire"]
        assert group["frames"].shape == (3, 1, 1340)
        assert group["spectra"].shape == (3, 1340)
        assert numpy.array_equal(group["spectra"][:], group["frames"][:, 0, :])
        assert group["temperature"].shape == (3,)
        assert group["wavelength"].shape == (1340,)
        assert numpy.allclose(group["wavelength"][:], 400.0 + 0.1 * numpy.arange(1340))
        assert group.attrs["exposure_time"] == pytest.approx(0.02)
        assert group.attrs["n_frames"] == 3
        assert f["hardware/openst133/settings"].attrs["exposure"] == pytest.approx(0.02)
        assert bool(f["hardware/openst133/settings"].attrs["full_vertical_binning"]) is True
        assert f["measurement/openst133_acquire/settings"].attrs["n_frames"] == 3
        assert f["app/settings"].attrs["save_dir"] == str(tmp_path)
        assert f["hardware/openst133/settings/units"].attrs["exposure"] == "s"
        assert numpy.isfinite(group["temperature"][:]).all()
    info, frames = spe.read(spe_path)
    assert (info.width, info.height, info.frame_count) == (1340, 1, 3)
    assert info.exposure_seconds == pytest.approx(0.02)
    assert info.temperature_setpoint == pytest.approx(-90.0)
    with h5py.File(h5_path, "r") as f:
        expected = f["measurement/openst133_acquire/frames"][0, 0, :]
    assert numpy.array_equal(numpy.asarray(frames[0]).reshape(1340), expected)


def test_h5_and_spe_hold_the_same_acquisition_metadata(app, tmp_path):
    hw = app.hardware["openst133"]
    measure = app.measurements["openst133_acquire"]
    hw.settings["connected"] = True
    hw.settings["exposure"] = 0.05
    hw.settings["roi_x0"], hw.settings["roi_x1"] = 100, 300
    hw.settings["roi_y0"], hw.settings["roi_y1"] = 20, 60
    hw.settings["bin_x"], hw.settings["bin_y"] = 2, 4
    hw.settings["temperature_setpoint"] = -100.0
    hw.camera.set_shutter(True)
    hw.camera.set_readout_rate(100000)
    measure.settings["mode"] = "single"
    measure.settings["n_frames"] = 2
    measure.settings["save_spe"] = True
    started = int(time.time())
    measure.start()
    wait_done(measure)
    h5_path, spe_path = measure.saved_paths
    with spe.SpeFile(spe_path) as written:
        info, stored = written.info, written.metadata
    assert (info.width, info.height, info.frame_count) == (100, 10, 2)
    with h5py.File(h5_path, "r") as f:
        attrs = dict(f["measurement/openst133_acquire"].attrs)
    expected = {
        "exposure_seconds": 0.05,
        "accumulations": 1,
        "roi": (100, 20, 200, 40),
        "binning": (2, 4),
        "readout_rate_hz": 100000,
        "chip_width": 1340,
        "chip_height": 100,
        "setpoint_celsius": -100.0,
        "shutter": "closed",
    }
    for key, value in expected.items():
        assert getattr(stored, key) == pytest.approx(value), key
        assert tuple(numpy.atleast_1d(attrs[key])) == pytest.approx(tuple(numpy.atleast_1d(value))), key
    assert stored.temperature_celsius == pytest.approx(float(attrs["temperature_celsius"]))
    assert started <= stored.acquired_utc == int(attrs["acquired_utc"]) <= int(time.time())
    assert stored.software_version.startswith("openst133 ")
    assert str(attrs["software_version"]).startswith("openst133 ")
    assert "detector_id" not in attrs and stored.detector_id is None
    assert int(attrs["controller_gain"]) == stored.controller_gain == 2


def test_spe_default_off_and_h5_default_on(app):
    measure = app.measurements["openst133_acquire"]
    assert measure.settings["save_h5"] is True
    assert measure.settings["save_spe"] is False
    assert measure.settings["mode"] == "live"


def test_live_view_runs_until_stopped_and_saves_nothing(app, tmp_path):
    hw = app.hardware["openst133"]
    measure = app.measurements["openst133_acquire"]
    hw.settings["connected"] = True
    hw.settings["exposure"] = 0.01
    measure.start()
    deadline = time.time() + 10
    while measure.last_frame is None and time.time() < deadline:
        time.sleep(0.02)
        app_events()
    assert measure.last_frame.shape == (100, 1340)
    measure.interrupt()
    wait_done(measure)
    assert list(tmp_path.glob("*.h5")) == []
    assert len(measure.save_last_frame()) == 1
    assert len(list(tmp_path.glob("*.h5"))) == 1
    with h5py.File(measure.saved_paths[0], "r") as f:
        group = f["measurement/openst133_acquire"]
        assert group["frames"].shape == (1, 100, 1340)
        assert numpy.array_equal(group["frames"][0], measure.last_frame)
        assert numpy.isnan(group["temperature"][0])
        assert numpy.isnan(group.attrs["temperature_mean"])
        assert f["hardware/openst133/settings"].attrs["exposure"] == pytest.approx(0.01)
        assert f["measurement/openst133_acquire/settings"].attrs["mode"] == "live"


def test_stopped_single_acquisition_saves_nothing(app, tmp_path):
    hw = app.hardware["openst133"]
    measure = app.measurements["openst133_acquire"]
    hw.settings["connected"] = True
    hw.settings["sim_speed"] = 1.0
    hw.settings["exposure"] = 0.02
    measure.settings["mode"] = "single"
    measure.settings["n_frames"] = 1000
    measure.start()
    deadline = time.time() + 10
    while len(measure.frames) < 3 and time.time() < deadline:
        time.sleep(0.02)
        app_events()
    measure.activation.update_value(False)
    wait_done(measure)
    assert 3 <= len(measure.frames) < 1000
    assert measure.end_state == "stop_interrupted"
    assert measure.saved_paths == []
    assert list(tmp_path.glob("*.h5")) == []


def test_unsupported_real_operations_disable_settings(app, monkeypatch):
    hw = app.hardware["openst133"]

    class FakeReal(RealCamera):
        def __init__(self, **kwargs):
            super().__init__(**kwargs)
            self.width, self.height = 1340, 100

        def open(self):
            self.unsupported = ["temperature read", "temperature setpoint", "exposure setup", "acquisition"]

        def set_shutter(self, mode):
            pass

        def set_gain(self, gain):
            pass

        def set_accumulations(self, count):
            pass

        def set_readout_rate(self, rate_hz):
            pass

        def close(self):
            pass

    monkeypatch.setattr("ScopeFoundryHW.openst133.openst133_hw.RealCamera", FakeReal)
    hw.settings["simulate"] = False
    hw.settings["connected"] = True
    assert hw.camera is not None
    for name in ("exposure", "temperature_setpoint"):
        lq = hw.settings.get_lq(name)
        assert lq.ro is True
        assert lq.description.startswith("UNAVAILABLE:")
    for name in (
        "full_vertical_binning", "roi_x0", "roi_x1", "roi_y0", "roi_y1", "bin_x", "bin_y",
    ):
        lq = hw.settings.get_lq(name)
        assert lq.ro is False
        assert not lq.description.startswith("UNAVAILABLE:")
    assert "acquisition" in hw.settings["unavailable"]
    assert "readout" not in hw.settings["unavailable"]
    assert numpy.isnan(hw._read_temperature())
    with pytest.raises(CameraUnavailable):
        hw.acquire()
    measure = app.measurements["openst133_acquire"]
    measure.pre_run()
    assert "acquisition" in measure.not_ready
    hw.settings["connected"] = False
    assert hw.settings.get_lq("exposure").ro is False


def test_roi_and_binning_pass_through_and_save_matching_shapes(app):
    hw = app.hardware["openst133"]
    measure = app.measurements["openst133_acquire"]
    hw.settings["connected"] = True
    hw.settings["roi_x1"] = 1000
    hw.settings["bin_x"] = 4
    assert hw.acquire().shape == (100, 250)
    hw.settings["roi_x0"], hw.settings["roi_x1"] = 100, 1000
    hw.settings["roi_y0"], hw.settings["roi_y1"] = 10, 50
    hw.settings["bin_x"], hw.settings["bin_y"] = 3, 4
    measure.settings["mode"] = "single"
    measure.settings["n_frames"] = 2
    measure.start()
    wait_done(measure)
    assert hw.camera.roi == (100, 10, 900, 40)
    assert hw.camera.binning == (3, 4)
    assert not hasattr(hw, "process_frame")
    with h5py.File(measure.saved_paths[0], "r") as f:
        group = f["measurement/openst133_acquire"]
        assert group["frames"].shape == (2, 10, 300)
        assert group["spectra"].shape == (2, 300)
        assert "wavelength" not in group


def test_camera_settings_and_fvb_over_a_region_reach_the_camera(app):
    hw = app.hardware["openst133"]
    hw.settings["sim_speed"] = 0.0
    hw.settings["connected"] = True
    camera = hw.camera
    hw.settings["gain"] = 3
    hw.settings["shutter"] = "closed"
    hw.settings["accumulations"] = 3
    hw.settings["readout_rate"] = "100 kHz"
    hw.settings["temperature_setpoint"] = -110.5
    assert (camera.gain, camera.shutter, camera.accumulations, camera.readout_rate) == (3, "closed", 3, 100000)
    hw.settings["accumulations"] = 1
    hw.settings["readout_rate"] = "1 MHz"
    hw.settings["roi_x0"], hw.settings["roi_x1"] = 200, 1000
    hw.settings["bin_x"] = 2
    hw.settings["full_vertical_binning"] = True
    assert hw.readout_selection() == (200, 0, 800, 100, 2, 100)
    assert hw.acquire().shape == (1, 400)
    assert (camera.roi, camera.binning) == ((200, 0, 800, 100), (2, 100))
    hw.settings["connected"] = False


def test_panel_matches_the_viewer_controls(app, tmp_path):
    hw = app.hardware["openst133"]
    measure = app.measurements["openst133_acquire"]
    panel = measure.panel
    hw.settings["connected"] = True
    assert panel.output.text() == "Output 1340 x 100 image"
    assert panel.message.isVisible() and panel.message.toPlainText() == "No frame available yet"
    assert not panel.region[0].isEnabled()

    panel.window.setPos((200, 10), update=False, finish=False)
    panel.window.setSize((400, 40), finish=True)
    app_events()
    assert panel.custom.isChecked()
    assert [box.value() for box in panel.region] == [200, 10, 400, 40]
    assert hw.readout_selection() == (200, 10, 400, 40, 1, 1)

    panel.bin_x.setValue(4)
    panel.bin_x.editingFinished.emit()
    panel.bin_x.stepBy(1)
    assert hw.settings["bin_x"] == 5
    panel.fvb.click()
    app_events()
    assert panel.output.text() == "Output 80 x 1 spectrum" and not panel.bin_y.isEnabled()

    panel.acquire_once()
    wait_done(measure)
    measure.update_display()
    assert panel.spectrum.isVisible() and not panel.view.isVisible()
    assert panel.save_to(str(tmp_path / "spectrum.csv")) and (tmp_path / "spectrum.csv").exists()

    panel.fvb.click()
    hw.settings["bin_x"] = 1
    panel.reset_region()
    app_events()
    assert panel.full.isChecked() and hw.readout_selection() == (0, 0, 1340, 100, 1, 1)
    panel.acquire_continuous()
    deadline = time.time() + 10
    while measure.last_frame is None or measure.last_frame.shape != (100, 1340):
        app_events()
        assert time.time() < deadline
    assert panel.buttons["Stop"].isEnabled() and not panel.buttons["Continuous"].isEnabled()
    hw.settings["bin_x"] = 2
    while measure.last_frame.shape != (100, 670):
        app_events()
        assert time.time() < deadline, "the live stream did not restart with the new binning"
    assert measure.is_measuring()
    panel.stop()
    wait_done(measure)
    assert panel.buttons["Continuous"].isEnabled() and not panel.buttons["Stop"].isEnabled()
    hw.settings["connected"] = False


def test_starting_without_a_connection_connects_or_reports_without_raising(app, monkeypatch):
    hw = app.hardware["openst133"]
    measure = app.measurements["openst133_acquire"]
    assert hw.camera is None
    measure.panel.acquire_once()
    wait_done(measure)
    assert hw.camera is not None and measure.last_frame is not None
    hw.settings["connected"] = False

    def refuse(self):
        raise CameraUnavailable("camera is busy")

    monkeypatch.setattr("openst133.camera.SimulatedCamera.open", refuse)
    measure.panel.acquire_continuous()
    wait_done(measure)
    assert hw.camera is None
    assert measure.panel.status.text() == "not started: cannot open the camera: camera is busy"
    assert measure.run_state.val != "stop_failure"


def test_dragging_outside_the_window_draws_a_new_one(app):
    from qtpy import QtCore

    hw = app.hardware["openst133"]
    panel = app.measurements["openst133_acquire"].panel
    panel._drawn(QtCore.QRectF(QtCore.QPointF(700.4, 80.2), QtCore.QPointF(100.0, 20.0)))
    assert panel.custom.isChecked() and hw.readout_selection() == (100, 20, 600, 60, 1, 1)
    panel._drawn(QtCore.QRectF(QtCore.QPointF(-30.0, 40.0), QtCore.QPointF(1400.0, 60.0)))
    assert hw.readout_selection() == (0, 40, 1340, 20, 1, 1)
    panel._drawn(QtCore.QRectF(5.0, 5.0, 1.0, 1.0))
    assert hw.readout_selection() == (0, 40, 1340, 20, 1, 1)


def test_a_camera_that_cannot_open_unticks_connected_without_raising(app, monkeypatch):
    hw = app.hardware["openst133"]

    def refuse(self):
        raise CameraUnavailable("camera is busy")

    monkeypatch.setattr("openst133.camera.SimulatedCamera.open", refuse)
    hw.settings["connected"] = True
    assert hw.camera is None and hw.settings["connected"] is False


def test_window_has_edge_handles_cursors_and_hides_on_the_full_sensor(app):
    from qtpy import QtCore

    hw = app.hardware["openst133"]
    panel = app.measurements["openst133_acquire"].panel
    assert not panel.window.isVisible()
    assert len(panel.window.getHandles()) == 8
    assert panel.window.cursor().shape() == QtCore.Qt.SizeAllCursor
    shapes = [handle.cursor().shape() for handle in panel.window.getHandles()]
    assert shapes.count(QtCore.Qt.SizeHorCursor) == 2 and shapes.count(QtCore.Qt.SizeVerCursor) == 2
    assert shapes.count(QtCore.Qt.SizeFDiagCursor) == 2 and shapes.count(QtCore.Qt.SizeBDiagCursor) == 2
    panel._drawn(QtCore.QRectF(100.0, 20.0, 600.0, 60.0))
    assert panel.window.isVisible()
    panel.reset_region()
    assert not panel.window.isVisible()
    assert hw.readout_selection() == (0, 0, 1340, 100, 1, 1)


def test_dragging_past_the_image_edge_stops_at_the_sensor_edge(app):
    from qtpy import QtCore

    hw = app.hardware["openst133"]
    panel = app.measurements["openst133_acquire"].panel
    box = panel.view.getViewBox()
    box.mouseDragEvent(FakeDrag(box, (600, 30), (-400, 400), finish=False))
    band = box.band.rect()
    assert (band.left(), band.bottom()) == (0, 100) and box.band.isVisible()
    box.mouseDragEvent(FakeDrag(box, (600, 30), (-400, 400), finish=True))
    x, y, w, h, _, _ = hw.readout_selection()
    assert (x, x + w, y + h) == (0, 600, 100) and abs(y - 30) <= 1, (x, y, w, h)

    panel._drawn(QtCore.QRectF(100.0, 20.0, 600.0, 60.0))
    top = next(handle for handle in panel.window.getHandles() if handle.cursor().shape() == QtCore.Qt.SizeVerCursor
               and panel.window.handles[panel.window.indexOfHandle(handle)]["pos"].y() == 0)
    panel.window.movePoint(top, pg_point(400, -500), finish=True)
    app_events()
    assert hw.readout_selection()[1::2][:2] == (0, 80), hw.readout_selection()


class FakeDrag:
    def __init__(self, box, start, end, finish):
        from qtpy import QtCore

        self._start = box.mapFromView(QtCore.QPointF(*start))
        self._end = box.mapFromView(QtCore.QPointF(*end))
        self._finish = finish

    def button(self):
        from qtpy import QtCore

        return QtCore.Qt.LeftButton

    def accept(self):
        pass

    def ignore(self):
        pass

    def buttonDownPos(self):
        return self._start

    def pos(self):
        return self._end

    def isFinish(self):
        return self._finish


def pg_point(x, y):
    import pyqtgraph

    return pyqtgraph.Point(x, y)


def test_saved_exposure_is_the_effective_one_and_the_request_is_kept(app, monkeypatch):
    from openst133.camera import SimulatedCamera

    monkeypatch.setattr(SimulatedCamera, "effective_exposure", property(lambda self: round(self._exposure * 0.9984, 6)), raising=False)
    hw = app.hardware["openst133"]
    measure = app.measurements["openst133_acquire"]
    hw.settings["connected"] = True
    hw.settings["exposure"] = 10.0
    hw.settings["sim_speed"] = 0.0
    hw.camera.sleep_scale = 0.0
    measure.settings["mode"] = "single"
    measure.settings["save_spe"] = True
    measure.start()
    wait_done(measure)
    h5_path, spe_path = measure.saved_paths
    with h5py.File(h5_path, "r") as f:
        attrs = dict(f["measurement/openst133_acquire"].attrs)
    assert float(attrs["exposure_seconds"]) == float(attrs["exposure_time"]) == 9.984
    assert float(attrs["requested_exposure_seconds"]) == 10.0
    info, _ = spe.read(spe_path)
    assert info.exposure_seconds == pytest.approx(9.984, abs=1e-6)


def test_h5_is_self_describing(app):
    hw = app.hardware["openst133"]
    measure = app.measurements["openst133_acquire"]
    hw.settings["connected"] = True
    hw.settings["roi_x0"], hw.settings["roi_x1"], hw.settings["bin_x"] = 200, 1000, 4
    measure.settings["mode"] = "single"
    measure.settings["n_frames"] = 3
    started = time.time()
    measure.start()
    wait_done(measure)
    with h5py.File(measure.saved_paths[0], "r") as f:
        group = f["measurement/openst133_acquire"]
        times = group["frame_time_utc"][...]
        assert times.shape == (3,) and started <= times[0] <= times[-1] <= time.time()
        assert group["sensor_column"][:3].tolist() == [200, 204, 208] and group["sensor_row"].shape == (100,)
        assert group["frames"].attrs["dimensions"] == "frame, row, column"
        assert group["temperature"].attrs["unit"] == "C" and group["frame_time_utc"].attrs["unit"] == "s"
        assert "acquisition_record" in group.attrs
