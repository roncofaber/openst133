# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT

"""Minimal ctypes discovery binding for the openst133 shared library."""

import ctypes
import operator
import os
from dataclasses import dataclass
from typing import List, Optional, Sequence, Tuple, Union

from ._library import load

MAX_PORT_DEPTH = 7


class _CDeviceInfo(ctypes.Structure):
    _fields_ = [
        ("vendor_id", ctypes.c_uint16),
        ("product_id", ctypes.c_uint16),
        ("bus_number", ctypes.c_uint8),
        ("device_address", ctypes.c_uint8),
        ("port_count", ctypes.c_uint8),
        ("port_numbers", ctypes.c_uint8 * MAX_PORT_DEPTH),
    ]


@dataclass(frozen=True)
class Device:
    """USB location of an ST-133 interface; no open or capability is implied.

    port_numbers is the port chain from the root hub (empty when the transport cannot report it). It names the
    physical socket and survives reconnection, while device_address is reassigned by the host each time.
    """

    vendor_id: int
    product_id: int
    bus_number: int
    device_address: int
    port_numbers: Tuple[int, ...] = ()

    @property
    def port_path(self) -> Optional[str]:
        """BUS-PORT[.PORT...], the Linux sysfs device name, or None without a port chain."""
        if not self.port_numbers:
            return None
        return f"{self.bus_number}-" + ".".join(str(port) for port in self.port_numbers)


def _device_to_c(device: Device) -> _CDeviceInfo:
    ports = tuple(device.port_numbers)
    if len(ports) > MAX_PORT_DEPTH:
        raise ValueError(f"a USB port chain has at most {MAX_PORT_DEPTH} ports")
    info = _CDeviceInfo(device.vendor_id, device.product_id, device.bus_number, device.device_address, len(ports))
    for index, port in enumerate(ports):
        info.port_numbers[index] = port
    return info


def _load_library(path: Optional[Union[str, os.PathLike]] = None, *, hardware=False) -> ctypes.CDLL:
    library = load(path, hardware=hardware)
    library.ost_list_devices.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.POINTER(_CDeviceInfo)),
        ctypes.POINTER(ctypes.c_size_t),
    ]
    library.ost_list_devices.restype = ctypes.c_int
    library.ost_free_devices.argtypes = [ctypes.POINTER(_CDeviceInfo)]
    library.ost_free_devices.restype = None
    library.ost_select_device.argtypes = [
        ctypes.POINTER(_CDeviceInfo),
        ctypes.c_size_t,
        ctypes.c_char_p,
        ctypes.POINTER(ctypes.c_size_t),
    ]
    library.ost_select_device.restype = ctypes.c_int
    return library


def list_devices(path: Optional[Union[str, os.PathLike]] = None, transport=None) -> List[Device]:
    """List ST-133 USB interfaces without opening them or sending commands.

    transport is a pointer to a C ost_transport (for example a replay transport); None selects libusb.
    """

    library = _load_library(path)
    devices = ctypes.POINTER(_CDeviceInfo)()
    count = ctypes.c_size_t()
    status = library.ost_list_devices(transport, ctypes.byref(devices), ctypes.byref(count))
    if status != 0:
        raise OSError(status, f"openst133 discovery failed with status {status}")
    try:
        return [
            Device(
                vendor_id=devices[index].vendor_id,
                product_id=devices[index].product_id,
                bus_number=devices[index].bus_number,
                device_address=devices[index].device_address,
                port_numbers=tuple(devices[index].port_numbers[: min(devices[index].port_count, MAX_PORT_DEPTH)]),
            )
            for index in range(count.value)
        ]
    finally:
        library.ost_free_devices(devices)


def check_access(device: Device, path: Optional[Union[str, os.PathLike]] = None, transport=None) -> None:
    """Open and close the camera's USB interface without sending any camera command.

    Raises NativeError with the library's reason (for example a missing udev rule or another process holding the
    interface) when the interface cannot be opened. OPENST133_NO_HARDWARE=1 refuses real hardware.
    """
    library = _load_library(path, hardware=transport is None)
    library.ost_open.argtypes = [ctypes.c_void_p, ctypes.POINTER(_CDeviceInfo), ctypes.POINTER(ctypes.c_void_p)]
    library.ost_open.restype = ctypes.c_int
    library.ost_close.argtypes = [ctypes.c_void_p]
    library.ost_close.restype = None
    library.ost_status_text.argtypes = [ctypes.c_int]
    library.ost_status_text.restype = ctypes.c_char_p
    handle = ctypes.c_void_p()
    status = library.ost_open(transport, ctypes.byref(_device_to_c(device)), ctypes.byref(handle))
    if status == OST_SUCCESS:
        library.ost_close(handle)
        return
    reason = library.ost_status_text(status).decode()
    detail = getattr(library, "ost_libusb_open_error", None) if transport is None else None
    if detail is not None:
        detail.argtypes = []
        detail.restype = ctypes.c_char_p
        text = detail()
        if text:
            reason = f"{reason}: {text.decode()}"
    raise NativeError(status, f"cannot open the camera USB interface: {reason}")


def _describe(devices: Sequence[Device]) -> str:
    return "; ".join(
        f"{index}: {device.port_path or '-'} ({device.bus_number}:{device.device_address})"
        for index, device in enumerate(devices)
    )


def select_device(
    devices: Sequence[Device], selector: Optional[str] = None, path: Optional[Union[str, os.PathLike]] = None
) -> Device:
    """Pick one of devices (as list_devices returns them) with the C rule of ost_select_device.

    selector is the index from 0, a BUS-PORT[.PORT] path or BUS:ADDR. None or an empty selector picks the only device.
    LookupError: no device matches (or none is attached). ValueError: malformed selector, or several devices and
    no selector, which never picks one silently.
    """

    library = _load_library(path)
    devices = list(devices)
    array = (_CDeviceInfo * max(len(devices), 1))()
    for index, device in enumerate(devices):
        array[index] = _device_to_c(device)
    chosen = ctypes.c_size_t()
    encoded = None if selector is None else str(selector).encode("ascii", "replace")
    status = library.ost_select_device(array, len(devices), encoded, ctypes.byref(chosen))
    if status == OST_SUCCESS:
        return devices[chosen.value]
    if not devices:
        raise LookupError("no ST-133 camera found (USB 0bd7:a010)")
    if status == OST_ERR_NOT_FOUND:
        raise LookupError(f"no attached ST-133 matches camera {selector!r}; attached: {_describe(devices)}")
    if not selector:
        raise ValueError(
            f"{len(devices)} ST-133 cameras are attached; choose one by index, BUS-PORT path or BUS:ADDR: "
            f"{_describe(devices)}"
        )
    raise ValueError(f"camera {selector!r} is not an index, BUS-PORT[.PORT] path or BUS:ADDR")


OST_SUCCESS = 0
OST_ERR_INVALID_ARGUMENT = -1
OST_ERR_NO_MEMORY = -2
OST_ERR_IO = -3
OST_ERR_NOT_FOUND = -4
OST_ERR_UNSUPPORTED = -5
OST_ERR_ABORTED = -6
OST_ERR_BUFFER_TOO_SMALL = -7
OST_ERR_TIMEOUT = -8
OST_ERR_BUSY = -9
OST_ERR_ACCESS = -10
OST_ERR_CLOSED = -11

SHUTTER_CLOSED = 0
SHUTTER_OPEN = 1
SHUTTER_DISABLED_OPENED = 2
SHUTTER_PRE_OPEN = 3
SHUTTER_MODES = {
    "normal": SHUTTER_OPEN,
    "closed": SHUTTER_CLOSED,
    "opened": SHUTTER_DISABLED_OPENED,
    "preopen": SHUTTER_PRE_OPEN,
}


def shutter_mode_name(mode) -> str:
    """Shutter mode name for a name in SHUTTER_MODES or a bool (True is Disabled Closed, False Normal)."""
    if isinstance(mode, bool):
        return "closed" if mode else "normal"
    if mode not in SHUTTER_MODES:
        raise ValueError(f"shutter mode must be one of {', '.join(SHUTTER_MODES)} or a bool, not {mode!r}")
    return mode

OPERATION_GET_TEMPERATURE = 0
OPERATION_SET_TEMPERATURE = 1
OPERATION_SET_EXPOSURE = 2
OPERATION_ACQUIRE = 3
OPERATION_ABORT = 4
OPERATION_SET_READOUT = 5

TARGET_SOURCE_NONE = 0
TARGET_SOURCE_OPTION = 1
TARGET_SOURCE_PERSISTED = 2
TARGET_SOURCE_CAMERA_DEFAULT = 3
TARGET_SOURCE_STARTUP_DEFAULT = 4

TARGET_SOURCE_NAMES = {
    TARGET_SOURCE_NONE: "none",
    TARGET_SOURCE_OPTION: "option",
    TARGET_SOURCE_PERSISTED: "persisted",
    TARGET_SOURCE_CAMERA_DEFAULT: "camera-default",
    TARGET_SOURCE_STARTUP_DEFAULT: "startup-default",
}


class _COpenOptions(ctypes.Structure):
    _fields_ = [("target_set", ctypes.c_int), ("target_celsius", ctypes.c_double)]


@dataclass(frozen=True)
class CameraOpenOptions:
    """Camera open settings; None retains the persisted, camera or startup-default target selection."""

    target_celsius: Optional[float] = None


class _CGeometry(ctypes.Structure):
    _fields_ = [("width", ctypes.c_uint32), ("height", ctypes.c_uint32), ("reported", ctypes.c_int)]


class _CReadout(ctypes.Structure):
    _fields_ = [(name, ctypes.c_uint32) for name in ("x", "y", "width", "height", "x_binning", "y_binning")]


@dataclass(frozen=True)
class Readout:
    """Sensor ROI and integer binning applied within it."""

    x: int
    y: int
    width: int
    height: int
    x_binning: int
    y_binning: int


class _CIdentity(ctypes.Structure):
    _fields_ = [
        ("controller_type", ctypes.c_uint32),
        ("detector_id", ctypes.c_uint32),
        ("detector_name", ctypes.c_char * 32),
        ("width", ctypes.c_uint32),
        ("height", ctypes.c_uint32),
        ("target_known", ctypes.c_int),
        ("target_celsius", ctypes.c_double),
    ]


class _CStreamStats(ctypes.Structure):
    _fields_ = [
        ("frames_delivered", ctypes.c_uint32),
        ("frames_lagged", ctypes.c_uint32),
        ("host_stalls", ctypes.c_uint32),
        ("max_ready", ctypes.c_uint32),
        ("short_transfers", ctypes.c_uint32),
        ("max_callback_us", ctypes.c_uint32),
        ("bytes_received", ctypes.c_uint64),
        ("transfers_completed", ctypes.c_uint64),
        ("partial_bytes_discarded", ctypes.c_uint64),
        ("stopped", ctypes.c_int),
    ]


_FRAME_CALLBACK = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_size_t)


@dataclass(frozen=True)
class StreamStats:
    """Counters of a finished stream; partial_bytes_discarded is data received but never part of a complete frame."""

    frames_delivered: int
    frames_lagged: int
    host_stalls: int
    short_transfers: int
    bytes_received: int
    transfers_completed: int
    partial_bytes_discarded: int
    stopped: bool
    max_ready: int = 0
    max_callback_us: int = 0


@dataclass(frozen=True)
class Identity:
    """Camera identification: chip ID 98 is EEV 100x1340B (1340 x 100); the stored target is cached."""

    controller_type: int
    detector_id: int
    detector_name: str
    width: int
    height: int
    target_celsius: Optional[float]


def _uint32(name, value) -> int:
    # ctypes would silently keep only the low 32 bits of a wider Python integer.
    value = operator.index(value)
    if not 0 <= value <= 0xFFFFFFFF:
        raise ValueError(f"{name} {value} is outside the uint32_t range of the camera API")
    return value


class NativeError(OSError):
    """A failed camera call; status is the C ost_status value and message the camera's last-error text."""

    def __init__(self, status: int, message: str):
        super().__init__(status, message)
        self.status = status
        self.message = message

    def __str__(self):
        return self.message


def _bind_camera(library: ctypes.CDLL) -> ctypes.CDLL:
    if getattr(library, "_openst133_camera_bound", False):
        return library
    camera = ctypes.c_void_p
    library.ost_camera_open.argtypes = [ctypes.c_void_p, ctypes.POINTER(_CDeviceInfo), ctypes.POINTER(camera)]
    library.ost_camera_open_with_options.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(_CDeviceInfo),
        ctypes.POINTER(_COpenOptions),
        ctypes.POINTER(camera),
    ]
    library.ost_camera_open_error.argtypes = []
    library.ost_camera_open_error.restype = ctypes.c_char_p
    library.ost_camera_get_identity.argtypes = [camera, ctypes.POINTER(_CIdentity)]
    library.ost_camera_get_identity.restype = ctypes.c_int
    library.ost_camera_close.argtypes = [camera]
    library.ost_camera_close.restype = None
    library.ost_camera_get_target_source.argtypes = [camera, ctypes.POINTER(ctypes.c_int)]
    library.ost_camera_last_error.argtypes = [camera]
    library.ost_camera_last_error.restype = ctypes.c_char_p
    library.ost_camera_get_geometry.argtypes = [camera, ctypes.POINTER(_CGeometry)]
    library.ost_camera_query_support.argtypes = [camera, ctypes.c_int]
    library.ost_camera_get_temperature.argtypes = [camera, ctypes.POINTER(ctypes.c_double)]
    library.ost_camera_set_temperature.argtypes = [camera, ctypes.c_double]
    library.ost_camera_set_shutter.argtypes = [camera, ctypes.c_int]
    library.ost_camera_get_shutter.argtypes = [camera, ctypes.POINTER(ctypes.c_int)]
    library.ost_camera_set_gain.argtypes = [camera, ctypes.c_uint32]
    library.ost_camera_get_gain.argtypes = [camera, ctypes.POINTER(ctypes.c_uint32)]
    library.ost_camera_set_accumulations.argtypes = [camera, ctypes.c_uint32]
    library.ost_camera_get_accumulations.argtypes = [camera, ctypes.POINTER(ctypes.c_uint32)]
    library.ost_camera_set_exposure.argtypes = [camera, ctypes.c_uint32]
    library.ost_camera_get_exposure.argtypes = [camera, ctypes.POINTER(ctypes.c_uint32)]
    library.ost_camera_get_effective_exposure.argtypes = [camera, ctypes.POINTER(ctypes.c_uint32)]
    library.ost_camera_set_readout.argtypes = [camera, ctypes.POINTER(_CReadout)]
    library.ost_camera_get_readout.argtypes = [camera, ctypes.POINTER(_CReadout)]
    library.ost_camera_set_full_vertical_binning.argtypes = [camera]
    library.ost_camera_set_readout_rate.argtypes = [camera, ctypes.c_uint32]
    library.ost_camera_acquire.argtypes = [
        camera,
        ctypes.c_void_p,
        ctypes.c_size_t,
        ctypes.POINTER(ctypes.c_size_t),
    ]
    library.ost_camera_abort.argtypes = [camera]
    library.ost_camera_stream.argtypes = [
        camera,
        ctypes.c_uint32,
        _FRAME_CALLBACK,
        ctypes.c_void_p,
        ctypes.POINTER(_CStreamStats),
    ]
    library.ost_status_text.argtypes = [ctypes.c_int]
    library.ost_status_text.restype = ctypes.c_char_p
    for name in (
        "ost_camera_open_with_options",
        "ost_camera_open",
        "ost_camera_get_target_source",
        "ost_camera_get_geometry",
        "ost_camera_query_support",
        "ost_camera_get_temperature",
        "ost_camera_set_temperature",
        "ost_camera_set_exposure",
        "ost_camera_set_shutter",
        "ost_camera_get_shutter",
        "ost_camera_set_gain",
        "ost_camera_get_gain",
        "ost_camera_set_accumulations",
        "ost_camera_get_accumulations",
        "ost_camera_get_exposure",
        "ost_camera_get_effective_exposure",
        "ost_camera_set_readout",
        "ost_camera_get_readout",
        "ost_camera_set_full_vertical_binning",
        "ost_camera_set_readout_rate",
        "ost_camera_acquire",
        "ost_camera_abort",
        "ost_camera_stream",
    ):
        getattr(library, name).restype = ctypes.c_int
    library._openst133_camera_bound = True
    return library


class NativeCamera:
    """ctypes wrapper of the C camera API; every method raises NativeError on a non-zero status."""

    def __init__(
        self,
        device: Device,
        *,
        path=None,
        transport=None,
        options: Optional[CameraOpenOptions] = None,
    ):
        self._library = _bind_camera(_load_library(path, hardware=transport is None))
        info = _device_to_c(device)
        self._handle = ctypes.c_void_p()
        if options is not None:
            native_options = _COpenOptions(
                target_set=options.target_celsius is not None,
                target_celsius=0.0 if options.target_celsius is None else float(options.target_celsius),
            )
            status = self._library.ost_camera_open_with_options(
                transport, ctypes.byref(info), ctypes.byref(native_options), ctypes.byref(self._handle)
            )
        else:
            status = self._library.ost_camera_open(transport, ctypes.byref(info), ctypes.byref(self._handle))
        if status != OST_SUCCESS:
            self._handle = None
            detail = self._library.ost_camera_open_error()
            reason = f": {detail.decode()}" if detail else ""
            raise NativeError(status, f"cannot open camera: {self._status_text(status)}{reason}")

    def _status_text(self, status: int) -> str:
        return self._library.ost_status_text(status).decode()

    def last_error(self) -> str:
        self._require_open()
        text = self._library.ost_camera_last_error(self._handle)
        return text.decode() if text else ""

    def _require_open(self):
        if self._handle is None:
            raise NativeError(OST_ERR_CLOSED, "camera is closed")

    def _check(self, status: int):
        if status != OST_SUCCESS:
            text = self._library.ost_camera_last_error(self._handle)
            raise NativeError(status, text.decode() if text else self._status_text(status))

    def close(self):
        if self._handle is not None:
            self._library.ost_camera_close(self._handle)
            self._handle = None

    @property
    def target_source(self) -> int:
        self._require_open()
        value = ctypes.c_int()
        self._check(self._library.ost_camera_get_target_source(self._handle, ctypes.byref(value)))
        return value.value

    @property
    def geometry(self):
        """(width, height, reported_by_camera)."""
        self._require_open()
        value = _CGeometry()
        self._check(self._library.ost_camera_get_geometry(self._handle, ctypes.byref(value)))
        return value.width, value.height, bool(value.reported)

    @property
    def identity(self) -> Identity:
        """Identification decoded during startup; NativeError (unsupported) when the camera has not identified."""
        self._require_open()
        value = _CIdentity()
        self._check(self._library.ost_camera_get_identity(self._handle, ctypes.byref(value)))
        return Identity(
            controller_type=value.controller_type,
            detector_id=value.detector_id,
            detector_name=value.detector_name.decode(),
            width=value.width,
            height=value.height,
            target_celsius=value.target_celsius if value.target_known else None,
        )

    def supports(self, operation: int) -> bool:
        self._require_open()
        return self._library.ost_camera_query_support(self._handle, operation) == OST_SUCCESS

    def get_temperature(self) -> float:
        self._require_open()
        value = ctypes.c_double()
        self._check(self._library.ost_camera_get_temperature(self._handle, ctypes.byref(value)))
        return value.value

    def set_temperature(self, celsius: float):
        self._require_open()
        self._check(self._library.ost_camera_set_temperature(self._handle, celsius))

    def set_shutter(self, mode):
        """Select the shutter of the next acquisition by name (see SHUTTER_MODES) or bool (True: Disabled Closed).

        Pre Open ("preopen") is refused because it needs External Sync timing.
        """
        self._require_open()
        state = SHUTTER_MODES[shutter_mode_name(mode)]
        self._check(self._library.ost_camera_set_shutter(self._handle, state))

    def get_shutter(self) -> str:
        self._require_open()
        value = ctypes.c_int()
        self._check(self._library.ost_camera_get_shutter(self._handle, ctypes.byref(value)))
        return {state: name for name, state in SHUTTER_MODES.items()}[value.value]

    def set_gain(self, gain: int):
        """Controller gain 1, 2 or 3 for the next acquisition."""
        self._require_open()
        self._check(self._library.ost_camera_set_gain(self._handle, _uint32("gain", gain)))

    def get_gain(self) -> int:
        self._require_open()
        value = ctypes.c_uint32()
        self._check(self._library.ost_camera_get_gain(self._handle, ctypes.byref(value)))
        return value.value

    def set_accumulations(self, count: int):
        """Frames read and summed per acquisition (1, 3 or 10); above 1 frames are int32 sums."""
        self._require_open()
        self._check(self._library.ost_camera_set_accumulations(self._handle, _uint32("accumulations", count)))

    def get_accumulations(self) -> int:
        self._require_open()
        value = ctypes.c_uint32()
        self._check(self._library.ost_camera_get_accumulations(self._handle, ctypes.byref(value)))
        return value.value

    def set_exposure_ms(self, milliseconds: int):
        self._require_open()
        self._check(self._library.ost_camera_set_exposure(self._handle, _uint32("exposure_ms", milliseconds)))

    def get_exposure_ms(self) -> int:
        self._require_open()
        value = ctypes.c_uint32()
        self._check(self._library.ost_camera_get_exposure(self._handle, ctypes.byref(value)))
        return value.value

    def get_effective_exposure_ms(self) -> int:
        self._require_open()
        value = ctypes.c_uint32()
        self._check(self._library.ost_camera_get_effective_exposure(self._handle, ctypes.byref(value)))
        return value.value

    def get_readout(self) -> Readout:
        self._require_open()
        value = _CReadout()
        self._check(self._library.ost_camera_get_readout(self._handle, ctypes.byref(value)))
        return Readout(value.x, value.y, value.width, value.height, value.x_binning, value.y_binning)

    def set_readout(self, readout: Readout):
        self._require_open()
        value = _CReadout(
            *(
                _uint32(name, getattr(readout, name))
                for name in ("x", "y", "width", "height", "x_binning", "y_binning")
            )
        )
        self._check(self._library.ost_camera_set_readout(self._handle, ctypes.byref(value)))

    def set_full_vertical_binning(self):
        self._require_open()
        self._check(self._library.ost_camera_set_full_vertical_binning(self._handle))

    def set_readout_rate(self, rate_hz: int):
        self._require_open()
        self._check(self._library.ost_camera_set_readout_rate(self._handle, _uint32("readout_rate_hz", rate_hz)))

    def acquire_into(self, buffer) -> int:
        """Acquire one frame into a writable buffer-protocol object (for example a numpy array); returns the bytes."""
        self._require_open()
        view = memoryview(buffer)
        if view.readonly:
            raise ValueError("buffer must be writable")
        address = ctypes.addressof(ctypes.c_char.from_buffer(view))
        acquired = ctypes.c_size_t()
        status = self._library.ost_camera_acquire(self._handle, address, view.nbytes, ctypes.byref(acquired))
        self._check(status)
        return acquired.value

    def abort(self):
        self._require_open()
        self._check(self._library.ost_camera_abort(self._handle))

    def stream(self, frame_count: int, on_frame) -> StreamStats:
        """Stream frames (frame_count 0: until abort()); on_frame(index, frame_bytes) gets a private copy of each frame.

        An exception or a falsy-to-stop return of True from on_frame ends the stream with the camera stop sequence;
        the exception is re-raised here. Blocks until the stream has ended.
        """
        self._require_open()
        frame_count = _uint32("frame_count", frame_count)
        failure = []

        def callback(_userdata, index, address, size):
            try:
                stop = on_frame(index, ctypes.string_at(address, size))
            except BaseException as error:  # noqa: BLE001 - must not unwind through C
                failure.append(error)
                return OST_ERR_ABORTED
            return OST_ERR_ABORTED if stop else OST_SUCCESS

        stats = _CStreamStats()
        status = self._library.ost_camera_stream(
            self._handle, frame_count, _FRAME_CALLBACK(callback), None, ctypes.byref(stats)
        )
        if failure:
            raise failure[0]
        self._check(status)
        return StreamStats(
            frames_delivered=stats.frames_delivered,
            frames_lagged=stats.frames_lagged,
            host_stalls=stats.host_stalls,
            short_transfers=stats.short_transfers,
            max_ready=stats.max_ready,
            max_callback_us=stats.max_callback_us,
            bytes_received=stats.bytes_received,
            transfers_completed=stats.transfers_completed,
            partial_bytes_discarded=stats.partial_bytes_discarded,
            stopped=bool(stats.stopped),
        )


__all__ = [
    "CameraOpenOptions",
    "Device",
    "Identity",
    "NativeCamera",
    "NativeError",
    "Readout",
    "StreamStats",
    "check_access",
    "list_devices",
    "select_device",
    "OPERATION_ABORT",
    "OPERATION_ACQUIRE",
    "OPERATION_GET_TEMPERATURE",
    "OPERATION_SET_EXPOSURE",
    "OPERATION_SET_READOUT",
    "OPERATION_SET_TEMPERATURE",
    "OST_SUCCESS",
    "OST_ERR_ABORTED",
    "OST_ERR_ACCESS",
    "OST_ERR_BUFFER_TOO_SMALL",
    "OST_ERR_BUSY",
    "OST_ERR_CLOSED",
    "OST_ERR_INVALID_ARGUMENT",
    "OST_ERR_IO",
    "OST_ERR_NO_MEMORY",
    "OST_ERR_NOT_FOUND",
    "OST_ERR_TIMEOUT",
    "OST_ERR_UNSUPPORTED",
    "SHUTTER_CLOSED",
    "SHUTTER_DISABLED_OPENED",
    "SHUTTER_MODES",
    "SHUTTER_OPEN",
    "SHUTTER_PRE_OPEN",
    "TARGET_SOURCE_CAMERA_DEFAULT",
    "TARGET_SOURCE_NONE",
    "TARGET_SOURCE_NAMES",
    "TARGET_SOURCE_OPTION",
    "TARGET_SOURCE_PERSISTED",
    "TARGET_SOURCE_STARTUP_DEFAULT",
    "shutter_mode_name",
]
