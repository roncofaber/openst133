# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT

"""ScopeFoundry HardwareComponent for an ST-133 camera (Spec-10:100B) through the openst133 package."""

import threading
import time

import numpy
from ScopeFoundry import HardwareComponent

from openst133.export import acquisition_metadata, effective_exposure
from openst133.camera import (
    AcquisitionAborted,
    CameraError,
    CameraUnavailable,
    RealCamera,
    SimulatedCamera,
)

SENSOR_WIDTH = 1340
SENSOR_HEIGHT = 100

UNSUPPORTED_SUFFIX = "not supported by the camera driver yet"
READOUT_RATES = {"1 MHz": 1000000, "100 kHz": 100000}
CAMERA_SETTINGS = ("exposure", "temperature_setpoint", "shutter", "gain", "accumulations", "readout_rate")
READOUT_SETTINGS = ("full_vertical_binning", "roi_x0", "roi_x1", "roi_y0", "roi_y1", "bin_x", "bin_y")


class OpenST133HW(HardwareComponent):
    """Camera settings and acquisition through the openst133 readout API.

    Operations the camera does not support turn their settings read-only with the reason in the tooltip.
    """

    name = "openst133"

    def setup(self):
        self.camera = None
        self.connect_error = None
        self.acquired_exposure = None
        self.acquired_metadata = {}
        self.io_lock = threading.Lock()
        self._stopping = threading.Event()
        self._acquiring = threading.Event()
        self.unsupported = {}
        self._tooltips = {}
        self._readout_applied = None
        self._pending = set()

        self.settings.New("simulate", bool, initial=True, description="Use the simulated camera instead of the USB device.")
        self.settings.New("sim_speed", float, initial=1.0, vmin=0.0, description="Scale of simulated exposure waiting time (0 returns at once).")
        self.exposure = self.settings.New(
            "exposure", float, initial=0.1, unit="s", si=True, vmin=0.001, vmax=60.0, spinbox_decimals=3,
            description="Exposure time of one frame.",
        )
        self.temperature = self.settings.New(
            "temperature", float, initial=0.0, unit="C", ro=True, spinbox_decimals=1, description="Sensor temperature.",
        )
        self.setpoint = self.settings.New(
            "temperature_setpoint", float, initial=-120.0, unit="C", vmin=-152.0, vmax=-25.0, spinbox_decimals=1,
            spinbox_step=0.5, description="Temperature setpoint, -152 to -25 C in 0.5 C steps.",
        )
        self.settings.New(
            "shutter", str, initial="normal", choices=("normal", "closed", "opened"),
            description="normal opens during exposure; closed and opened hold the shutter (Disabled Closed / Opened).",
        )
        self.settings.New("gain", int, initial=2, choices=(1, 2, 3), description="Controller gain.")
        self.settings.New(
            "accumulations", int, initial=1, choices=(1, 3, 10),
            description="Frames summed into one 32-bit frame per acquisition; above 1 single acquisitions only.",
        )
        self.settings.New(
            "readout_rate", str, initial="1 MHz", choices=tuple(READOUT_RATES),
            description="Digitization rate; 100 kHz is slower with lower read noise and reads only the full unbinned frame.",
        )
        self.settings.New("roi_x0", int, initial=0, vmin=0, vmax=SENSOR_WIDTH - 1, description="First column passed to the camera API.")
        self.settings.New("roi_x1", int, initial=SENSOR_WIDTH, vmin=1, vmax=SENSOR_WIDTH, description="End column, exclusive, converted to the API ROI width.")
        self.settings.New("roi_y0", int, initial=0, vmin=0, vmax=SENSOR_HEIGHT - 1, description="First row passed to the camera API.")
        self.settings.New("roi_y1", int, initial=SENSOR_HEIGHT, vmin=1, vmax=SENSOR_HEIGHT, description="End row, exclusive, converted to the API ROI height.")
        self.settings.New("bin_x", int, initial=1, vmin=1, vmax=SENSOR_WIDTH, description="Column binning passed to the camera API.")
        self.settings.New("bin_y", int, initial=1, vmin=1, vmax=SENSOR_HEIGHT, description="Row binning passed to the camera API.")
        self.full_vertical_binning = self.settings.New(
            "full_vertical_binning", bool, initial=False,
            description="Sum every row of the ROI into one spectrum; bin_x still applies and bin_y is ignored.",
        )
        self.settings.New("unavailable", str, initial="", ro=True, description="Operations the camera driver does not support yet.")
        for name in (*CAMERA_SETTINGS, "temperature", *READOUT_SETTINGS):
            lq = self.settings.get_lq(name)
            self._tooltips[lq.name] = lq.description

    def connect(self):
        self.connect_error = None
        self._stopping.clear()
        if self.settings["simulate"]:
            camera = SimulatedCamera(width=SENSOR_WIDTH, height=SENSOR_HEIGHT, sleep_scale=self.settings["sim_speed"])
        else:
            camera = RealCamera(report=self.log.info)
        try:
            camera.open()
        except CameraUnavailable as error:
            self.connect_error = str(error)
            self.log.error(f"cannot open the camera: {error}")
            raise
        self.camera = camera
        self._readout_applied = None
        self._pending = set()
        self.unsupported = self._probe_unsupported(camera)
        self._apply_availability()
        for name in CAMERA_SETTINGS:
            self.settings.get_lq(name).connect_to_hardware(write_func=lambda value, name=name: self._write(name, value))
        self.temperature.connect_to_hardware(read_func=self._read_temperature_lq)
        target = self._camera_target(camera)
        for name in CAMERA_SETTINGS:
            if name == "temperature_setpoint" and target is not None:
                self.setpoint.update_value(target, update_hardware=False)
            else:
                self._write(name, self.settings[name])

    def enable_connection(self, enable=True):
        """Report a camera that cannot be opened in the log and untick connected, instead of an unhandled exception."""
        try:
            super().enable_connection(enable)
        except CameraUnavailable:
            self.connected.update_value(False)

    def disconnect(self):
        self.settings.disconnect_all_from_hardware()
        camera = self.camera
        if camera is not None:
            # A running acquisition holds io_lock until it ends. abort() is repeated because the simulated camera clears
            # an abort that arrives between two frames.
            self._stopping.set()
            while not self.io_lock.acquire(timeout=0.1):
                camera.abort()
            try:
                self.camera = None
                camera.close()
            finally:
                self.io_lock.release()
        self.unsupported = {}
        self._readout_applied = None
        self._pending = set()
        self._apply_availability()

    def threaded_update(self):
        if self.io_lock.acquire(blocking=False):
            try:
                self.temperature.read_from_hardware()
            finally:
                self.io_lock.release()
        time.sleep(1.0)

    @staticmethod
    def _camera_target(camera):
        """The target the camera opened with (persisted or option), shown instead of overwriting it on connect."""
        if not isinstance(camera, RealCamera):
            return None
        try:
            return camera.identity.target_celsius
        except (CameraError, OSError):
            return None

    @staticmethod
    def _probe_unsupported(camera):
        """Map camera.unsupported operation names (RealCamera) to the settings they disable."""
        reasons = {}
        names = {
            "temperature read": "temperature",
            "temperature setpoint": "temperature_setpoint",
            "exposure setup": "exposure",
            "acquisition": "acquisition",
            "readout": "readout",
        }
        for operation in getattr(camera, "unsupported", []):
            reasons[names[operation]] = f"{operation} is {UNSUPPORTED_SUFFIX}"
        return reasons

    def _apply_availability(self):
        """Disabled settings keep their value, become read-only and carry the reason as tooltip."""
        for name in (*CAMERA_SETTINGS, "temperature", *READOUT_SETTINGS):
            lq = self.settings.get_lq(name)
            group = "readout" if name in READOUT_SETTINGS else name
            reason = self.unsupported.get(group)
            if name != "temperature":
                lq.change_readonly(reason is not None)
            description = self._tooltips.get(name, lq.description)
            lq.description = description if reason is None else f"UNAVAILABLE: {reason}"
            for widget in lq.widget_list:
                widget.setToolTip(lq.description)
        self.settings["unavailable"] = "; ".join(f"{name}: {reason}" for name, reason in self.unsupported.items())
        self.settings.get_lq("unavailable").description = self.settings["unavailable"] or "Everything is available."

    def is_available(self, name):
        return self.camera is not None and name not in self.unsupported

    def _read_temperature(self):
        if not self.is_available("temperature"):
            return float("nan")
        try:
            return float(self.camera.temperature())
        except CameraUnavailable as error:
            self.unsupported["temperature"] = str(error)
            return float("nan")

    def _read_temperature_lq(self):
        value = self._read_temperature()
        return self.temperature.val if numpy.isnan(value) else value

    def _write(self, name, value):
        if self.is_available(name):
            self._write_or_defer(name, value)

    def _write_now(self, name, value):
        writers = {
            "exposure": self.camera.set_exposure,
            "temperature_setpoint": self.camera.set_temperature,
            "shutter": self.camera.set_shutter,
            "gain": lambda gain: self.camera.set_gain(int(gain)),
            "accumulations": lambda count: self.camera.set_accumulations(int(count)),
            "readout_rate": lambda rate: self.camera.set_readout_rate(READOUT_RATES[rate]),
        }
        writers[name](value)

    def _write_or_defer(self, name, value):
        # ScopeFoundry calls write functions on the GUI thread: waiting here for a running acquisition (a live stream
        # holds the lock until stopped) would freeze the GUI and with it the button that stops the acquisition. Any
        # other holder, such as the once-a-second temperature read, releases the lock within milliseconds.
        while not self.io_lock.acquire(timeout=0.05):
            if self._acquiring.is_set():
                self._pending.add(name)
                self.log.info(f"{name} is applied when the acquisition restarts")
                return
        try:
            self._pending.discard(name)
            self._write_now(name, value)
        finally:
            self.io_lock.release()

    def _apply_pending(self):
        for name in sorted(self._pending):
            self._pending.discard(name)
            if self.is_available(name):
                self._write_now(name, self.settings[name])

    def readout_selection(self):
        """(x, y, width, height, x_binning, y_binning) from the ROI, binning and full-vertical-binning settings."""
        x0, x1 = self.settings["roi_x0"], self.settings["roi_x1"]
        y0, y1 = self.settings["roi_y0"], self.settings["roi_y1"]
        width, height = x1 - x0, y1 - y0
        bx = self.settings["bin_x"]
        by = height if self.settings["full_vertical_binning"] else self.settings["bin_y"]
        if width <= 0 or height <= 0:
            raise ValueError("ROI is empty: end must be greater than start")
        if width % bx or height % by:
            raise ValueError("ROI dimensions must be divisible by the binning factors")
        return (x0, y0, width, height, bx, by)

    def _snapshot(self):
        """Record what the next frames are acquired with, after pending settings and the readout are applied."""
        self.acquired_exposure = effective_exposure(self.camera, self.settings["exposure"])
        self.acquired_metadata = acquisition_metadata(self.camera)

    def needs_restart(self):
        """True when a setting changed since the running stream started, so the stream must restart to apply it."""
        if self._pending:
            return True
        try:
            return self.readout_selection() != self._readout_applied
        except ValueError:
            return False

    def configure_readout(self):
        if self.camera is None:
            raise CameraError("camera is not connected")
        with self.io_lock:
            self._apply_pending()
            self._configure_readout()

    def _configure_readout(self):
        selection = self.readout_selection()
        if selection == self._readout_applied:
            return
        default = (0, 0, self.camera.width, self.camera.height, 1, 1)
        if "readout" in self.unsupported:
            if selection != default:
                raise CameraUnavailable(self.unsupported["readout"])
            self._readout_applied = selection
            return
        try:
            self.camera.set_readout(*selection)
        except (CameraError, ValueError):
            self._readout_applied = None
            raise
        self._readout_applied = selection

    def acquire(self):
        """Acquire one camera-API readout frame without host-side cropping or binning."""
        self._check_not_stopping()
        if self.camera is None:
            raise CameraError("camera is not connected")
        if "acquisition" in self.unsupported:
            raise CameraUnavailable(self.unsupported["acquisition"])
        with self.io_lock:
            self._acquiring.set()
            try:
                self._check_not_stopping()
                self._apply_pending()
                self._configure_readout()
                self._snapshot()
                return self.camera.acquire()
            finally:
                self._acquiring.clear()

    def stream(self, frames=None):
        """Iterate over streamed frames (camera.stream); the I/O lock is held so no other command interleaves."""
        self._check_not_stopping()
        if self.camera is None:
            raise CameraError("camera is not connected")
        if "acquisition" in self.unsupported:
            raise CameraUnavailable(self.unsupported["acquisition"])
        with self.io_lock:
            self._acquiring.set()
            try:
                self._check_not_stopping()
                self._apply_pending()
                self._configure_readout()
                self._snapshot()
                iterator = self.camera.stream(frames)
                try:
                    for frame in iterator:
                        yield frame
                        if self._stopping.is_set():
                            return
                finally:
                    iterator.close()
            finally:
                self._acquiring.clear()

    def _check_not_stopping(self):
        if self._stopping.is_set():
            raise AcquisitionAborted("the camera was disconnected")

    def abort(self):
        if self.camera is not None:
            self.camera.abort()

