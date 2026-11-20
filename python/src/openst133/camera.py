# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT

"""Small camera interface used by the viewer, with a simulated camera and the real one over the C API."""

import logging
import math
import operator
import queue
import sys
import threading
import time

import numpy

from ._log import camera as logger
from .device import (
    OPERATION_ACQUIRE,
    OPERATION_GET_TEMPERATURE,
    OPERATION_SET_EXPOSURE,
    OPERATION_SET_TEMPERATURE,
    OST_ERR_ABORTED,
    OST_ERR_UNSUPPORTED,
    TARGET_SOURCE_NAMES,
    NativeCamera,
    Readout,
    NativeError,
    list_devices,
    select_device,
    shutter_mode_name,
)

SPEC10_100B_WIDTH = 1340
SPEC10_100B_HEIGHT = 100


def _log_acquisition(started, exposure, shutter, frame):
    if not logger.isEnabledFor(logging.INFO):
        return
    logger.info(
        "acquisition complete exposure_s=%s shutter=%s frame_count=1 duration_s=%.6f "
        "frame_mean=%.6f frame_min=%d frame_max=%d",
        exposure,
        shutter,
        time.monotonic() - started,
        float(numpy.mean(frame)),
        int(numpy.min(frame)),
        int(numpy.max(frame)),
    )


class CameraError(Exception):
    pass


class CameraUnavailable(CameraError):
    pass


class AcquisitionAborted(CameraError):
    pass


class Camera:
    """Interface: open, close, temperature, set_exposure, acquire (one frame), abort."""

    width = SPEC10_100B_WIDTH
    height = SPEC10_100B_HEIGHT

    def open(self):
        raise NotImplementedError

    def close(self):
        raise NotImplementedError

    def temperature(self):
        """Sensor temperature in degrees Celsius."""
        raise NotImplementedError

    def set_temperature(self, celsius):
        """Temperature setpoint in degrees Celsius."""
        raise NotImplementedError

    def set_exposure(self, seconds):
        raise NotImplementedError

    def acquire(self):
        """Block for one exposure and return a (height, width) uint16 array."""
        raise NotImplementedError

    def abort(self):
        """Interrupt a running acquire from another thread; it raises AcquisitionAborted."""
        raise NotImplementedError

    def stream(self, frames=None):
        """Iterate over frames as they complete until frames have arrived (None: until the iterator is closed).

        The default repeats acquire(); RealCamera streams continuously from the camera. Closing the iterator stops
        the acquisition. abort() from another thread ends it too.
        """
        count = 0
        while frames is None or count < frames:
            try:
                frame = self.acquire()
            except AcquisitionAborted:
                return
            count += 1
            yield frame

    def __enter__(self):
        self.open()
        return self

    def __exit__(self, exception_type, exception, traceback):
        self.close()


class RealCamera(Camera):
    """The ST-133 through the C camera API; settings outside their allowed ranges raise CameraUnavailable.

    device opens that exact Device; otherwise selector picks one of the attached cameras (an index from 0 in
    list_devices order, a BUS-PORT[.PORT] path or BUS:ADDR), and without either the only attached camera is used.
    """

    def __init__(
        self, *, library_path=None, transport=None, options=None, device=None, report=None, selector=None
    ):
        if device is not None and selector is not None:
            raise ValueError("pass either device or selector, not both")
        self._library_path = library_path
        self._transport = transport
        self._options = options
        self._device = device
        self._selector = selector
        self._report = report if report is not None else _report_to_stderr
        self._native = None
        self._exposure_ms = None
        self._requested_readout = None
        self.stream_stats = None
        self._shutter = "normal"
        self._gain = 2
        self._accumulations = 1
        self._readout_rate = 1000000
        self.unsupported = []

    def open(self):
        if self._native is not None:
            return
        try:
            device = self._device
            if device is None:
                found = list_devices(self._library_path, self._transport)
                try:
                    device = select_device(found, self._selector, self._library_path)
                except (LookupError, ValueError) as error:
                    raise CameraUnavailable(str(error)) from error
            native = NativeCamera(
                device,
                path=self._library_path,
                transport=self._transport,
                options=self._options,
            )
        except NativeError as error:
            logger.error("camera open failed: %s", error)
            raise CameraUnavailable(str(error)) from error
        except OSError as error:
            logger.error("camera open failed: %s", error)
            raise CameraUnavailable(f"cannot use the openst133 library: {error}") from error
        self._native = native
        width, height, _ = native.geometry
        self.width, self.height = width, height
        self.unsupported = [
            name for name, operation in _OPERATIONS if not native.supports(operation)
        ]
        if self.unsupported:
            self._report(
                "openst133: camera opened; not supported yet: " + ", ".join(self.unsupported)
            )
        logger.info("camera opened backend=native width=%d height=%d", self.width, self.height)

    def close(self):
        native, self._native = self._native, None
        if native is not None:
            native.close()
            logger.info("camera closed backend=native")

    def _require_open(self):
        if self._native is None:
            logger.error("camera operation failed: camera is not open")
            raise CameraError("camera is not open")
        return self._native

    def _call(self, function, *arguments):
        native = self._require_open()
        try:
            return function(native, *arguments)
        except NativeError as error:
            logger.error("camera operation failed: %s", error)
            if error.status == OST_ERR_UNSUPPORTED:
                raise CameraUnavailable(str(error)) from error
            if error.status == OST_ERR_ABORTED:
                raise AcquisitionAborted("acquisition aborted") from error
            raise CameraError(str(error)) from error

    def temperature(self):
        return self._call(NativeCamera.get_temperature)

    def set_temperature(self, celsius):
        """Write the requested setpoint, -152 to -25 C in 0.5 C steps; the native library refuses other targets."""
        self._call(NativeCamera.set_temperature, float(celsius))
        self._setpoint = float(celsius)
        logger.info("setting applied temperature_c=%s", float(celsius))

    @property
    def target_source(self):
        """Numeric ost_camera_target_source selected during open."""
        return self._call(lambda native: native.target_source)

    @property
    def identity(self):
        """Identification decoded during startup."""
        return self._call(lambda native: native.identity)

    def set_shutter(self, mode):
        """Shutter of the next acquisition: "normal", "closed", "opened" or "preopen", or a bool (True: "closed").

        Pre Open is refused because it needs External Sync timing, which is unsupported.
        """
        mode = shutter_mode_name(mode)
        self._call(NativeCamera.set_shutter, mode)
        self._shutter = mode
        logger.info("setting applied shutter=%s", mode)

    @property
    def shutter(self):
        return self._shutter

    def set_gain(self, gain):
        """Controller gain 1, 2 or 3; the native library refuses anything else."""
        gain = operator.index(gain)
        self._call(NativeCamera.set_gain, gain)
        self._gain = gain
        logger.info("setting applied gain=%d", gain)

    @property
    def gain(self):
        return self._gain

    def set_accumulations(self, count):
        """Frames the camera reads and the library sums per acquire(); above 1 acquire() returns int32 sums."""
        count = operator.index(count)
        self._call(NativeCamera.set_accumulations, count)
        self._accumulations = count
        logger.info("setting applied accumulations=%d", count)

    @property
    def accumulations(self):
        return self._accumulations

    def spe_metadata(self):
        """Requested and effective settings, detector identity and setpoint, plus counters after a native stream.

        requested_exposure_seconds is the value passed to set_exposure, before any rounding; the effective exposure
        the camera used is effective_exposure, which callers store as the frame exposure.
        """
        target_source = TARGET_SOURCE_NAMES.get(self._call(lambda native: native.target_source))
        metadata = _settings_metadata(
            self._call(NativeCamera.get_readout), self._readout_rate, self._gain, self._accumulations, self._shutter,
            requested_exposure_seconds=self.__dict__.get("_requested_exposure"),
            requested_readout=self._requested_readout, target_source=target_source,
            stream_stats=self.stream_stats,
        )
        try:
            identity = self.identity
        except (CameraError, OSError, AttributeError):
            identity = None
        if identity is not None:
            metadata.update(
                detector_id=identity.detector_id, detector_name=identity.detector_name,
                chip_width=identity.width, chip_height=identity.height,
            )
        setpoint = self.__dict__.get("_setpoint", identity.target_celsius if identity is not None else None)
        if setpoint is not None:
            metadata["setpoint_celsius"] = float(setpoint)
        return metadata

    def set_roi(self, x, y, width, height):
        """Select a sensor ROI while retaining the current binning; the native library validates the combination."""
        current = self._call(NativeCamera.get_readout)
        requested = Readout(x, y, width, height, current.x_binning, current.y_binning)
        self._call(NativeCamera.set_readout, requested)
        self._requested_readout = requested
        logger.info("setting applied roi=%s", (x, y, width, height))

    def set_binning(self, x_binning, y_binning):
        """Select integer binning within the current ROI; the native library validates the combination."""
        current = self._call(NativeCamera.get_readout)
        requested = Readout(current.x, current.y, current.width, current.height, x_binning, y_binning)
        self._call(NativeCamera.set_readout, requested)
        self._requested_readout = requested
        logger.info("setting applied binning=%s", (x_binning, y_binning))

    def set_readout(self, x, y, width, height, x_binning=1, y_binning=1):
        """Select region and binning together; y_binning equal to height sums the region's rows into one spectrum."""
        requested = Readout(x, y, width, height, x_binning, y_binning)
        self._call(NativeCamera.set_readout, requested)
        self._requested_readout = requested
        logger.info("setting applied roi=%s binning=%s", (x, y, width, height), (x_binning, y_binning))

    def set_full_vertical_binning(self):
        self._call(NativeCamera.set_full_vertical_binning)
        self._requested_readout = None
        logger.info("setting applied full_vertical_binning=true")

    @property
    def readout_rate(self):
        """Readout rate in Hz last set, or the 1 MHz rate a native open selects."""
        return self._readout_rate

    def set_readout_rate(self, rate_hz):
        self._call(NativeCamera.set_readout_rate, rate_hz)
        self._readout_rate = operator.index(rate_hz)
        logger.info("setting applied readout_rate_hz=%s", rate_hz)

    def set_exposure(self, seconds):
        seconds = float(seconds)
        if not seconds > 0.0:
            raise ValueError("exposure must be positive")
        milliseconds = max(1, math.ceil(seconds * 1000.0 - 1e-9))
        self._call(NativeCamera.set_exposure_ms, milliseconds)
        self._exposure_ms = milliseconds
        self._requested_exposure = seconds
        logger.info("setting applied exposure_s=%s exposure_ms=%d", seconds, milliseconds)

    @property
    def effective_exposure(self):
        """Effective exposure in seconds after the native exposure encoding truncates it."""
        return self._call(NativeCamera.get_effective_exposure_ms) / 1000.0

    def acquire(self):
        exposure = None if self._exposure_ms is None else self.effective_exposure
        self.stream_stats = None
        shutter = self._shutter
        logger.debug("acquisition started exposure_s=%s shutter=%s", exposure, shutter)
        started = time.monotonic()
        readout = self._call(NativeCamera.get_readout)
        frame = numpy.zeros(
            (max(1, readout.height // readout.y_binning), max(1, readout.width // readout.x_binning)),
            dtype=numpy.dtype("<i4") if self._accumulations > 1 else numpy.uint16,
        )
        self._call(NativeCamera.acquire_into, frame)
        _log_acquisition(started, exposure, shutter, frame)
        return frame

    def abort(self):
        native = self._native
        if native is None:
            return
        try:
            native.abort()
        except NativeError:
            pass

    def stream(self, frames=None, *, queue_size=4):
        """Stream frames from the camera as they complete, as an iterator of arrays.

        frames None streams until the iterator is closed or abort() is called; an integer stops after that many
        frames. The camera stop sequence is sent at the end. If the consumer falls behind, the oldest queued frame
        is dropped and counted in stream_dropped; stream_stats holds the library counters after the stream ended.
        """
        native = self._require_open()
        if frames is not None and frames < 1:
            raise ValueError("frames must be positive")
        self.stream_dropped = 0
        self.stream_stats = None
        frames_queue = queue.Queue(maxsize=queue_size)
        stopped = threading.Event()
        result = {}
        readout = self._call(lambda camera: camera.get_readout())
        shape = (max(1, readout.height // readout.y_binning), max(1, readout.width // readout.x_binning))

        def on_frame(_index, data):
            frame = numpy.frombuffer(data, dtype=numpy.uint16).reshape(shape)
            while True:
                try:
                    frames_queue.put_nowait(frame)
                    break
                except queue.Full:
                    try:
                        frames_queue.get_nowait()
                        self.stream_dropped += 1
                    except queue.Empty:
                        pass
            return stopped.is_set()

        def worker():
            try:
                result["stats"] = native.stream(0 if frames is None else frames, on_frame)
            except NativeError as error:
                result["error"] = error
            except BaseException as error:  # noqa: BLE001
                result["error"] = error
            finally:
                frames_queue.put(None)

        thread = threading.Thread(target=worker, name="openst133-stream", daemon=True)
        logger.debug("stream started frames=%s", frames)
        thread.start()
        try:
            while True:
                frame = frames_queue.get()
                if frame is None:
                    break
                yield frame
        finally:
            stopped.set()
            if thread.is_alive():
                try:
                    native.abort()
                except NativeError:
                    pass
                while thread.is_alive():
                    try:
                        frames_queue.get(timeout=0.05)
                    except queue.Empty:
                        pass
            thread.join()
            self.stream_stats = result.get("stats")
            if self.stream_stats is not None:
                logger.info(
                    "stream ended frames=%d dropped_by_consumer=%d partial_bytes=%d host_stalls=%d",
                    self.stream_stats.frames_delivered,
                    self.stream_dropped,
                    self.stream_stats.partial_bytes_discarded,
                    self.stream_stats.host_stalls,
                )
        error = result.get("error")
        if error is not None:
            if isinstance(error, NativeError):
                if error.status == OST_ERR_UNSUPPORTED:
                    raise CameraUnavailable(str(error)) from error
                raise CameraError(str(error)) from error
            raise error


_OPERATIONS = (
    ("temperature read", OPERATION_GET_TEMPERATURE),
    ("temperature setpoint", OPERATION_SET_TEMPERATURE),
    ("exposure setup", OPERATION_SET_EXPOSURE),
    ("acquisition", OPERATION_ACQUIRE),
)


def _report_to_stderr(message):
    print(message, file=sys.stderr)


class SimulatedCamera(Camera):
    """100B-sized spectral frames with ROI, integer binning and a slowly cooling sensor."""

    def __init__(
        self,
        *,
        width=SPEC10_100B_WIDTH,
        height=SPEC10_100B_HEIGHT,
        start_temperature=20.0,
        target_temperature=-120.0,
        cooling_time_constant=600.0,
        clock=time.monotonic,
        sleep_scale=1.0,
        seed=None,
    ):
        self.width = width
        self.height = height
        self._roi = (0, 0, width, height)
        self._binning = (1, 1)
        self._start_temperature = start_temperature
        self._target_temperature = target_temperature
        self._time_constant = cooling_time_constant
        self._clock = clock
        self._sleep_scale = sleep_scale
        self._random = numpy.random.default_rng(seed)
        self._exposure = 0.1
        self._shutter = "normal"
        self._gain = 2
        self._accumulations = 1
        self._readout_rate = 1000000
        self._opened_at = None
        self._abort = threading.Event()
        self._is_open = False

    def open(self):
        self._opened_at = self._clock()
        self._is_open = True
        logger.info("camera opened backend=simulated width=%d height=%d", self.width, self.height)

    def close(self):
        if self._is_open:
            logger.info("camera closed backend=simulated")
        self._is_open = False
        self._abort.set()

    def _require_open(self):
        if not self._is_open:
            logger.error("camera operation failed: camera is not open")
            raise CameraError("camera is not open")

    def set_temperature(self, celsius):
        self._require_open()
        self._target_temperature = float(celsius)
        logger.info("setting applied temperature_c=%s", self._target_temperature)

    def temperature(self):
        self._require_open()
        elapsed = max(0.0, self._clock() - self._opened_at)
        fraction = 1.0 - numpy.exp(-elapsed / self._time_constant)
        return float(self._start_temperature + (self._target_temperature - self._start_temperature) * fraction)

    @property
    def exposure(self):
        return self._exposure

    def set_shutter(self, mode):
        self._require_open()
        mode = shutter_mode_name(mode)
        if mode == "preopen":
            raise CameraUnavailable("Pre Open needs External Sync timing, which is unsupported")
        self._shutter = mode
        logger.info("setting applied shutter=%s", mode)

    @property
    def shutter(self):
        return self._shutter

    def set_gain(self, gain):
        self._require_open()
        gain = operator.index(gain)
        if gain not in (1, 2, 3):
            raise CameraUnavailable(f"controller gain {gain} does not exist; use 1, 2 or 3")
        self._gain = gain
        logger.info("setting applied gain=%d", gain)

    @property
    def gain(self):
        return self._gain

    def set_accumulations(self, count):
        self._require_open()
        count = operator.index(count)
        if count not in (1, 3, 10):
            raise CameraUnavailable(f"{count} accumulations: supported values are 1, 3 and 10")
        self._accumulations = count
        logger.info("setting applied accumulations=%d", count)

    @property
    def accumulations(self):
        return self._accumulations

    def spe_metadata(self):
        x, y, width, height = self._roi
        metadata = _settings_metadata(
            Readout(x, y, width, height, *self._binning), self._readout_rate, self._gain,
            self._accumulations, self._shutter, requested_exposure_seconds=self._exposure,
            requested_readout=Readout(x, y, width, height, *self._binning),
        )
        metadata.update(chip_width=self.width, chip_height=self.height)
        target = self.__dict__.get("_target_temperature")
        if target is not None:
            metadata["setpoint_celsius"] = float(target)
        return metadata

    @property
    def readout_rate(self):
        return self._readout_rate

    def set_readout_rate(self, rate_hz):
        self._require_open()
        self._readout_rate = operator.index(rate_hz)
        logger.info("setting applied readout_rate_hz=%d", self._readout_rate)

    def set_exposure(self, seconds):
        self._require_open()
        seconds = float(seconds)
        if not seconds > 0.0:
            raise ValueError("exposure must be positive")
        self._exposure = seconds
        logger.info("setting applied exposure_s=%s", seconds)

    @property
    def roi(self):
        return self._roi

    @property
    def binning(self):
        return self._binning

    def set_roi(self, x, y, width, height):
        self.set_readout(x, y, width, height, *self._binning)

    def set_binning(self, x_binning, y_binning):
        self.set_readout(*self._roi, x_binning, y_binning)

    def set_readout(self, x, y, width, height, x_binning=1, y_binning=1):
        x, y, width, height, x_binning, y_binning = self._integers(x, y, width, height, x_binning, y_binning)
        if x_binning <= 0 or y_binning <= 0:
            raise ValueError("binning factors must be positive integers")
        if x < 0 or y < 0 or width <= 0 or height <= 0 or x + width > self.width or y + height > self.height:
            raise ValueError("ROI must have nonzero size and fit inside the sensor")
        if width % x_binning or height % y_binning:
            raise ValueError("ROI dimensions must be divisible by the binning factors")
        self._roi = (x, y, width, height)
        self._binning = (x_binning, y_binning)
        logger.info("setting applied roi=%s binning=%s", self._roi, self._binning)

    def set_full_vertical_binning(self):
        self._roi = (0, 0, self.width, self.height)
        self._binning = (1, self.height)
        logger.info("setting applied roi=%s binning=%s", self._roi, self._binning)

    def acquire(self):
        self._abort.clear()
        return self._expose()

    def stream(self, frames=None):
        # The abort request is cleared once, as the native stream does, so an abort between two frames ends the stream.
        self._abort.clear()
        count = 0
        while frames is None or count < frames:
            try:
                frame = self._expose()
            except AcquisitionAborted:
                return
            count += 1
            yield frame

    def _expose(self):
        self._require_open()
        shutter = self._shutter
        logger.debug("acquisition started exposure_s=%s shutter=%s", self._exposure, shutter)
        started = time.monotonic()
        if self._abort.wait(self._exposure * self._sleep_scale * self._accumulations):
            logger.info("acquisition aborted")
            raise AcquisitionAborted("acquisition aborted")
        frame = self._frame()
        if self._accumulations > 1:
            frame = sum((self._frame() for _ in range(self._accumulations - 1)), frame.astype(numpy.dtype("<i4")))
        _log_acquisition(started, self._exposure, shutter, frame)
        return frame

    @staticmethod
    def _integers(*values):
        try:
            return tuple(operator.index(value) for value in values)
        except TypeError:
            raise ValueError("ROI and binning values must be integers") from None

    def abort(self):
        self._abort.set()

    def _frame(self):
        rows = numpy.arange(self.height)[:, None]
        columns = numpy.arange(self.width)[None, :]
        centre_row = self.height / 2
        line = numpy.exp(-((rows - centre_row) ** 2) / (2 * (self.height / 40) ** 2))
        spectral_width = max(1.0, self.width / 150)
        peaks = sum(
            height * numpy.exp(-((columns - centre * self.width) ** 2) / (2 * (spectral_width * width) ** 2))
            for centre, height, width in ((0.18, 5000, 0.5), (0.48, 9000, 0.8), (0.76, 6500, 0.4))
        )
        signal = (2500.0 * line + peaks * (0.35 + 0.65 * line)) * min(self._exposure / 0.1, 4.0)
        dark = 40.0 + 0.2 * (self.temperature() + 120.0)
        frame = self._random.poisson(numpy.clip(signal + dark, 0, None)) + self._random.normal(
            0.0, 15.0, (self.height, self.width)
        )
        sensor_frame = numpy.clip(frame, 0, 65535).astype(numpy.uint16)
        frame = self._readout(sensor_frame)
        self._last_sensor_frame = sensor_frame
        return frame

    def _readout(self, sensor_frame):
        x, y, width, height = self._roi
        x_binning, y_binning = self._binning
        roi = numpy.asarray(sensor_frame)[y : y + height, x : x + width]
        binned = roi.reshape(height // y_binning, y_binning, width // x_binning, x_binning).sum(
            axis=(1, 3), dtype=numpy.uint32
        )
        return numpy.clip(binned, 0, 65535).astype(numpy.uint16)


def _settings_metadata(readout, readout_rate, gain, accumulations, shutter, *,
                       requested_exposure_seconds=None, requested_readout=None, target_source=None,
                       stream_stats=None):
    metadata = {
        "accumulations": accumulations,
        "roi": (readout.x, readout.y, readout.width, readout.height),
        "binning": (readout.x_binning, readout.y_binning),
        "readout_rate_hz": readout_rate,
        "controller_gain": gain,
    }
    if shutter in ("normal", "closed", "opened"):
        metadata["shutter"] = shutter
    if requested_exposure_seconds is not None:
        metadata["requested_exposure_seconds"] = requested_exposure_seconds
    if requested_readout is not None:
        metadata["requested_roi"] = (
            requested_readout.x, requested_readout.y, requested_readout.width, requested_readout.height
        )
        metadata["requested_binning"] = (requested_readout.x_binning, requested_readout.y_binning)
    if target_source is not None:
        metadata["target_source"] = target_source
    if stream_stats is not None:
        metadata.update({f"stream_{name}": getattr(stream_stats, name) for name in stream_stats.__dataclass_fields__})
    return metadata


def make_camera(simulate, selector=None):
    if simulate:
        if selector is not None:
            raise ValueError("the simulated camera has no camera selection")
        return SimulatedCamera()
    return RealCamera(selector=selector)
