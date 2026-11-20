# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT

import array
import ctypes
import numbers
import operator
import os
import sys
import xml.etree.ElementTree as ElementTree
from dataclasses import dataclass, replace
from typing import Optional

from ._library import load

SPE_SUCCESS = 0
SPE_ERR_RANGE = -7
SPE_FLOAT32 = 0
SPE_INT32 = 1
SPE_UINT16 = 3
_SHUTTERS = {0: None, 1: "normal", 2: "closed", 3: "opened"}
_PROVENANCE_NS = "{urn:openst133:provenance:1}"
_STREAM_FIELDS = (
    "frames_delivered",
    "frames_lagged",
    "host_stalls",
    "short_transfers",
    "bytes_received",
    "transfers_completed",
    "partial_bytes_discarded",
    "stopped",
    "max_ready",
    "max_callback_us",
)


@dataclass(frozen=True)
class SpeRegion:
    width: int
    height: int
    size: int
    stride: int


@dataclass(frozen=True)
class SpeInfo:
    major_version: int
    header_version: float
    data_type: int
    width: int
    height: int
    frame_count: int
    region_count: int
    pixel_count: int
    pixel_size: int
    exposure_seconds: float
    temperature_setpoint: float
    date: str
    software_version: str
    regions: tuple[SpeRegion, ...]


@dataclass(frozen=True)
class SpeMetadata:
    """Acquisition provenance stored in an SPE file; None marks a field the file does not record."""

    exposure_seconds: float
    accumulations: Optional[int] = None
    roi: Optional[tuple[int, int, int, int]] = None
    binning: Optional[tuple[int, int]] = None
    readout_rate_hz: Optional[int] = None
    controller_gain: Optional[int] = None
    detector_id: Optional[int] = None
    detector_name: Optional[str] = None
    chip_width: Optional[int] = None
    chip_height: Optional[int] = None
    temperature_celsius: Optional[float] = None
    setpoint_celsius: Optional[float] = None
    shutter: Optional[str] = None
    acquired_utc: Optional[int] = None
    date: Optional[str] = None
    time_local: Optional[str] = None
    time_utc: Optional[str] = None
    software_version: Optional[str] = None
    requested_exposure_seconds: Optional[float] = None
    requested_roi: Optional[tuple[int, int, int, int]] = None
    requested_binning: Optional[tuple[int, int]] = None
    target_source: Optional[str] = None
    lock_tolerance_celsius: Optional[float] = None
    lock_hold_seconds: Optional[float] = None
    lock_settle_seconds: Optional[float] = None
    stream_frames_delivered: Optional[int] = None
    stream_frames_lagged: Optional[int] = None
    stream_host_stalls: Optional[int] = None
    stream_short_transfers: Optional[int] = None
    stream_bytes_received: Optional[int] = None
    stream_transfers_completed: Optional[int] = None
    stream_partial_bytes_discarded: Optional[int] = None
    stream_stopped: Optional[bool] = None
    stream_max_ready: Optional[int] = None
    stream_max_callback_us: Optional[int] = None
    calibration_status: Optional[str] = None

    def as_dict(self):
        """Recorded fields only, with tuples as lists, for H5 attributes or JSON."""
        values = {}
        for name in self.__dataclass_fields__:
            value = getattr(self, name)
            if value is not None:
                values[name] = list(value) if isinstance(value, tuple) else value
        return values


_WRITABLE_METADATA = (
    "accumulations",
    "roi",
    "binning",
    "readout_rate_hz",
    "controller_gain",
    "detector_id",
    "detector_name",
    "chip_width",
    "chip_height",
    "temperature_celsius",
    "setpoint_celsius",
    "shutter",
    "acquired_utc",
    "requested_exposure_seconds",
    "requested_roi",
    "requested_binning",
    "target_source",
    "lock_tolerance_celsius",
    "lock_hold_seconds",
    "lock_settle_seconds",
    "stream_stopped",
    "stream_max_ready",
    "stream_max_callback_us",
    "calibration_status",
    *(f"stream_{name}" for name in _STREAM_FIELDS if name not in ("stopped", "max_ready", "max_callback_us")),
)


class _Metadata(ctypes.Structure):
    _fields_ = [
        ("exposure_seconds", ctypes.c_double),
        ("accumulations", ctypes.c_uint32),
        ("roi_known", ctypes.c_int),
        ("roi_x", ctypes.c_uint32),
        ("roi_y", ctypes.c_uint32),
        ("roi_width", ctypes.c_uint32),
        ("roi_height", ctypes.c_uint32),
        ("x_binning", ctypes.c_uint32),
        ("y_binning", ctypes.c_uint32),
        ("readout_rate_hz", ctypes.c_uint32),
        ("controller_gain", ctypes.c_uint32),
        ("detector_id", ctypes.c_uint32),
        ("detector_name", ctypes.c_char * 32),
        ("chip_width", ctypes.c_uint32),
        ("chip_height", ctypes.c_uint32),
        ("temperature_known", ctypes.c_int),
        ("temperature_celsius", ctypes.c_double),
        ("setpoint_known", ctypes.c_int),
        ("setpoint_celsius", ctypes.c_double),
        ("shutter", ctypes.c_int),
        ("acquired_utc", ctypes.c_int64),
        ("date", ctypes.c_char * 11),
        ("time_local", ctypes.c_char * 7),
        ("time_utc", ctypes.c_char * 7),
        ("software_version", ctypes.c_char * 17),
        ("requested_exposure_known", ctypes.c_int),
        ("requested_exposure_seconds", ctypes.c_double),
        ("requested_roi_known", ctypes.c_int),
        ("requested_roi_x", ctypes.c_uint32),
        ("requested_roi_y", ctypes.c_uint32),
        ("requested_roi_width", ctypes.c_uint32),
        ("requested_roi_height", ctypes.c_uint32),
        ("requested_x_binning", ctypes.c_uint32),
        ("requested_y_binning", ctypes.c_uint32),
        ("target_source", ctypes.c_char * 24),
        ("lock_tolerance_known", ctypes.c_int),
        ("lock_tolerance_celsius", ctypes.c_double),
        ("lock_hold_known", ctypes.c_int),
        ("lock_hold_seconds", ctypes.c_double),
        ("lock_settle_known", ctypes.c_int),
        ("lock_settle_seconds", ctypes.c_double),
        ("stream_known", ctypes.c_int),
        ("stream_frames_delivered", ctypes.c_uint32),
        ("stream_frames_lagged", ctypes.c_uint32),
        ("stream_host_stalls", ctypes.c_uint32),
        ("stream_max_ready", ctypes.c_uint32),
        ("stream_short_transfers", ctypes.c_uint32),
        ("stream_max_callback_us", ctypes.c_uint32),
        ("stream_bytes_received", ctypes.c_uint64),
        ("stream_transfers_completed", ctypes.c_uint64),
        ("stream_partial_bytes_discarded", ctypes.c_uint64),
        ("stream_stopped", ctypes.c_int),
        ("calibration_status", ctypes.c_char * 32),
    ]


def _unsigned(name, value, limit=0xFFFFFFFF):
    try:
        value = operator.index(value)
    except TypeError as error:
        raise ValueError(f"SPE metadata {name} must be an integer") from error
    if not 0 <= value <= limit:
        raise ValueError(f"SPE metadata {name} out of range")
    return value


def _native_metadata(exposure_seconds, metadata):
    if isinstance(metadata, SpeMetadata):
        metadata = {name: getattr(metadata, name) for name in _WRITABLE_METADATA}
    metadata = {name: value for name, value in dict(metadata or {}).items() if value is not None}
    unknown = sorted(set(metadata) - set(_WRITABLE_METADATA))
    if unknown:
        raise ValueError(f"unknown SPE metadata fields: {', '.join(unknown)}")
    native = _Metadata()
    native.exposure_seconds = float(exposure_seconds)
    for name in ("accumulations", "readout_rate_hz", "controller_gain", "detector_id", "chip_width", "chip_height"):
        if name in metadata:
            setattr(native, name, _unsigned(name, metadata[name]))
    if "roi" in metadata or "binning" in metadata:
        if "roi" not in metadata:
            raise ValueError("SPE metadata binning needs roi")
        roi = tuple(metadata["roi"])
        binning = tuple(metadata.get("binning", (1, 1)))
        if len(roi) != 4 or len(binning) != 2:
            raise ValueError("SPE metadata roi is (x, y, width, height) and binning is (x, y)")
        native.roi_known = 1
        native.roi_x, native.roi_y, native.roi_width, native.roi_height = (_unsigned("roi", value) for value in roi)
        native.x_binning, native.y_binning = (_unsigned("binning", value) for value in binning)
    if "detector_name" in metadata:
        name = str(metadata["detector_name"]).encode("ascii", "replace")
        if len(name) > 31:
            raise ValueError("SPE metadata detector_name is longer than 31 characters")
        native.detector_name = name
    if "temperature_celsius" in metadata:
        native.temperature_known = 1
        native.temperature_celsius = float(metadata["temperature_celsius"])
    if "setpoint_celsius" in metadata:
        native.setpoint_known = 1
        native.setpoint_celsius = float(metadata["setpoint_celsius"])
    if "shutter" in metadata:
        codes = {value: code for code, value in _SHUTTERS.items() if value is not None}
        if metadata["shutter"] not in codes:
            raise ValueError("SPE metadata shutter is 'normal', 'closed' or 'opened'")
        native.shutter = codes[metadata["shutter"]]
    if "acquired_utc" in metadata:
        native.acquired_utc = operator.index(metadata["acquired_utc"])
    if "requested_exposure_seconds" in metadata:
        native.requested_exposure_known = 1
        native.requested_exposure_seconds = float(metadata["requested_exposure_seconds"])
    if ("requested_roi" in metadata) != ("requested_binning" in metadata):
        raise ValueError("SPE metadata requested_roi and requested_binning must be supplied together")
    if "requested_roi" in metadata:
        roi = tuple(metadata["requested_roi"])
        binning = tuple(metadata["requested_binning"])
        if len(roi) != 4 or len(binning) != 2:
            raise ValueError("SPE metadata requested_roi is (x, y, width, height) and requested_binning is (x, y)")
        native.requested_roi_known = 1
        native.requested_roi_x, native.requested_roi_y, native.requested_roi_width, native.requested_roi_height = (
            _unsigned("requested_roi", value) for value in roi
        )
        native.requested_x_binning, native.requested_y_binning = (
            _unsigned("requested_binning", value) for value in binning
        )
    for name, capacity in (("target_source", 24), ("calibration_status", 32)):
        if name in metadata:
            try:
                text = str(metadata[name]).encode("ascii")
            except UnicodeEncodeError as error:
                raise ValueError(f"SPE metadata {name} must be ASCII") from error
            if len(text) >= capacity:
                raise ValueError(f"SPE metadata {name} is longer than {capacity - 1} characters")
            setattr(native, name, text)
    for name in ("lock_tolerance_celsius", "lock_hold_seconds", "lock_settle_seconds"):
        if name in metadata:
            setattr(native, name.replace("_celsius", "_known").replace("_seconds", "_known"), 1)
            setattr(native, name, float(metadata[name]))
    supplied_stream_fields = {f"stream_{name}" for name in _STREAM_FIELDS if f"stream_{name}" in metadata}
    if supplied_stream_fields:
        expected = {f"stream_{name}" for name in _STREAM_FIELDS}
        if supplied_stream_fields != expected:
            raise ValueError("SPE stream provenance requires every stream counter and stream_stopped")
        native.stream_known = 1
        for name in _STREAM_FIELDS:
            field = f"stream_{name}"
            if name == "stopped":
                if not isinstance(metadata[field], bool):
                    raise ValueError("SPE metadata stream_stopped must be a boolean")
                native.stream_stopped = int(metadata[field])
            else:
                limit = 0xFFFFFFFFFFFFFFFF if name in (
                    "bytes_received", "transfers_completed", "partial_bytes_discarded"
                ) else 0xFFFFFFFF
                setattr(native, field, _unsigned(field, metadata[field], limit))
    return native


def _python_metadata(native):
    def known(value):
        return value if value else None

    roi_known = bool(native.roi_known)
    return SpeMetadata(
        exposure_seconds=native.exposure_seconds,
        accumulations=known(native.accumulations),
        roi=(native.roi_x, native.roi_y, native.roi_width, native.roi_height) if roi_known else None,
        binning=(native.x_binning, native.y_binning) if roi_known else None,
        readout_rate_hz=known(native.readout_rate_hz),
        controller_gain=known(native.controller_gain),
        detector_id=known(native.detector_id),
        detector_name=known(native.detector_name.decode("ascii", "replace")),
        chip_width=known(native.chip_width),
        chip_height=known(native.chip_height),
        temperature_celsius=native.temperature_celsius if native.temperature_known else None,
        setpoint_celsius=native.setpoint_celsius if native.setpoint_known else None,
        shutter=_SHUTTERS.get(native.shutter),
        acquired_utc=known(native.acquired_utc),
        date=known(native.date.decode("ascii", "replace")),
        time_local=known(native.time_local.decode("ascii", "replace")),
        time_utc=known(native.time_utc.decode("ascii", "replace")),
        software_version=known(native.software_version.decode("ascii", "replace")),
    )


def _provenance_value(element, name, converter=str):
    if element is None:
        return None
    value = element.get(name)
    return converter(value) if value is not None and value != "unknown" else None


def _provenance_region(element):
    values = [_provenance_value(element, name, int) for name in ("x", "y", "width", "height")]
    binning = [_provenance_value(element, name, int) for name in ("xBinning", "yBinning")]
    return (tuple(values) if all(value is not None for value in values) else None,
            tuple(binning) if all(value is not None for value in binning) else None)


def _provenance_metadata(footer):
    root = ElementTree.fromstring(footer)
    provenance = root.find(f".//{_PROVENANCE_NS}Provenance")
    if provenance is None:
        return {}
    exposure = provenance.find(f"{_PROVENANCE_NS}Exposure")
    requested_roi, requested_binning = _provenance_region(provenance.find(f"{_PROVENANCE_NS}RequestedRegion"))
    effective_roi, effective_binning = _provenance_region(provenance.find(f"{_PROVENANCE_NS}EffectiveRegion"))
    selection = provenance.find(f"{_PROVENANCE_NS}Selection")
    lock = provenance.find(f"{_PROVENANCE_NS}SoftwareLock")
    stream = provenance.find(f"{_PROVENANCE_NS}Stream")
    values = {
        "requested_exposure_seconds": _provenance_value(exposure, "requestedSeconds", float),
        "requested_roi": requested_roi,
        "requested_binning": requested_binning,
        "target_source": _provenance_value(selection, "targetSource"),
        "lock_tolerance_celsius": _provenance_value(lock, "toleranceCelsius", float),
        "lock_hold_seconds": _provenance_value(lock, "holdSeconds", float),
        "lock_settle_seconds": _provenance_value(lock, "settleSeconds", float),
        "stream_stopped": _provenance_value(stream, "stopped", lambda value: value == "true"),
        "calibration_status": _provenance_value(provenance.find(f"{_PROVENANCE_NS}Calibration"), "status"),
    }
    effective_exposure = _provenance_value(exposure, "effectiveSeconds", float)
    if effective_exposure is not None:
        values["exposure_seconds"] = effective_exposure
    if effective_roi is not None:
        values["roi"] = effective_roi
        values["binning"] = effective_binning
    for name in _STREAM_FIELDS:
        if name == "stopped":
            continue
        xml_name = name.split("_")[0] + "".join(part.capitalize() for part in name.split("_")[1:])
        values[f"stream_{name}"] = _provenance_value(stream, xml_name, int)
    return values


class _Region(ctypes.Structure):
    _fields_ = [
        ("width", ctypes.c_uint32),
        ("height", ctypes.c_uint32),
        ("size", ctypes.c_uint64),
        ("stride", ctypes.c_uint64),
    ]


class _Info(ctypes.Structure):
    _fields_ = [
        ("major_version", ctypes.c_uint),
        ("header_version", ctypes.c_float),
        ("data_type", ctypes.c_int),
        ("width", ctypes.c_uint32),
        ("height", ctypes.c_uint32),
        ("frame_count", ctypes.c_uint64),
        ("region_count", ctypes.c_uint64),
        ("pixel_count", ctypes.c_uint64),
        ("pixel_size", ctypes.c_size_t),
        ("exposure_seconds", ctypes.c_double),
        ("temperature_setpoint", ctypes.c_float),
        ("date", ctypes.c_char * 11),
        ("software_version", ctypes.c_char * 17),
    ]


def _load_library(path=None):
    library = load(path)
    library.ost_spe_read.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p)]
    library.ost_spe_read.restype = ctypes.c_int
    library.ost_spe_free.argtypes = [ctypes.c_void_p]
    library.ost_spe_free.restype = None
    library.ost_spe_get_info.argtypes = [ctypes.c_void_p, ctypes.POINTER(_Info)]
    library.ost_spe_get_info.restype = ctypes.c_int
    library.ost_spe_get_region.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint64,
        ctypes.POINTER(_Region),
    ]
    library.ost_spe_get_region.restype = ctypes.c_int
    library.ost_spe_read_frame.argtypes = [
        ctypes.c_void_p,
        ctypes.c_uint64,
        ctypes.c_void_p,
        ctypes.c_size_t,
        ctypes.POINTER(ctypes.c_size_t),
    ]
    library.ost_spe_read_frame.restype = ctypes.c_int
    library.ost_spe_header.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_void_p),
        ctypes.POINTER(ctypes.c_size_t),
    ]
    library.ost_spe_header.restype = ctypes.c_int
    library.ost_spe_xml_footer.argtypes = [
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_void_p),
        ctypes.POINTER(ctypes.c_size_t),
    ]
    library.ost_spe_xml_footer.restype = ctypes.c_int
    library.ost_spe_create.argtypes = [
        ctypes.c_char_p,
        ctypes.c_uint32,
        ctypes.c_uint32,
        ctypes.c_int,
        ctypes.c_uint64,
        ctypes.c_void_p,
        ctypes.c_size_t,
        ctypes.c_double,
        ctypes.POINTER(ctypes.c_float),
    ]
    library.ost_spe_create.restype = ctypes.c_int
    library.ost_spe_write.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    library.ost_spe_write.restype = ctypes.c_int
    library.ost_spe_create_with_metadata.argtypes = [
        ctypes.c_char_p,
        ctypes.c_uint32,
        ctypes.c_uint32,
        ctypes.c_int,
        ctypes.c_uint64,
        ctypes.c_void_p,
        ctypes.c_size_t,
        ctypes.POINTER(_Metadata),
    ]
    library.ost_spe_create_with_metadata.restype = ctypes.c_int
    library.ost_spe_get_metadata.argtypes = [ctypes.c_void_p, ctypes.POINTER(_Metadata)]
    library.ost_spe_get_metadata.restype = ctypes.c_int
    return library


def _check(status, operation):
    if status != SPE_SUCCESS:
        raise OSError(f"{operation} failed with SPE status {status}")


def _creation_data_type(data_type, floating):
    if data_type is None:
        return SPE_FLOAT32 if floating else SPE_UINT16
    if data_type in (SPE_FLOAT32, "float32"):
        return SPE_FLOAT32
    if data_type in (SPE_UINT16, "uint16"):
        return SPE_UINT16
    if data_type in (SPE_INT32, "int32"):
        return SPE_INT32
    raise ValueError("SPE creation supports only uint16, int32 and float32 pixels")


def _creation_shape(shape, expected, frame_count):
    if len(shape) != 2 or 0 in shape:
        raise ValueError("frames must be non-empty 2-D arrays of one shape")
    if expected is None:
        expected = shape
    if shape != expected:
        raise ValueError("frames must be non-empty 2-D arrays of one shape")
    height, width = expected
    if width > 65535 or height > 65535 or frame_count > 2147483647:
        raise ValueError("frame dimensions or count out of SPE range")
    return height, width


def _pack_numpy_frames(frames, data_type, numpy):
    arrays = [numpy.asarray(frame) for frame in frames]
    expected = None
    for array_value in arrays:
        expected = _creation_shape(array_value.shape, expected, len(arrays))
    height, width = expected
    floating = any(numpy.issubdtype(value.dtype, numpy.floating) for value in arrays)
    if data_type is None and all(value.dtype == numpy.int32 for value in arrays):
        data_type = SPE_INT32
    data_type = _creation_data_type(data_type, floating)
    if any(numpy.issubdtype(value.dtype, numpy.complexfloating) for value in arrays):
        raise ValueError("complex pixels are not supported")
    if data_type == SPE_UINT16:
        for value in arrays:
            if numpy.issubdtype(value.dtype, numpy.integer) and value.size:
                if value.min() < 0 or value.max() > 65535:
                    raise ValueError("uint16 pixel value out of range")
            elif numpy.issubdtype(value.dtype, numpy.floating) and value.size:
                if not numpy.isfinite(value).all() or value.min() < 0 or value.max() > 65535:
                    raise ValueError("uint16 pixel value out of range")
    if data_type == SPE_INT32:
        limits = numpy.iinfo(numpy.int32)
        for value in arrays:
            if value.size and (not numpy.isfinite(value).all() or value.min() < limits.min or value.max() > limits.max):
                raise ValueError("int32 pixel value out of range")
    dtype = {SPE_FLOAT32: numpy.float32, SPE_INT32: numpy.dtype("<i4").type}.get(data_type, numpy.uint16)
    try:
        packed = [numpy.ascontiguousarray(value, dtype=dtype).tobytes() for value in arrays]
    except (TypeError, ValueError, OverflowError) as error:
        raise ValueError(f"cannot convert pixels to {dtype.__name__}") from error
    return width, height, data_type, b"".join(packed)


def _pack_plain_frames(frames, data_type):
    values = []
    expected = None
    for frame in frames:
        try:
            rows = [list(row) for row in frame]
        except TypeError as error:
            raise ValueError("frames must be non-empty 2-D arrays of one shape") from error
        shape = (len(rows), len(rows[0]) if rows else 0)
        if any(len(row) != shape[1] for row in rows):
            raise ValueError("frames must be non-empty 2-D arrays of one shape")
        _creation_shape(shape, expected, len(frames))
        expected = shape
        flat = [value for row in rows for value in row]
        values.append(flat)
    height, width = expected
    floating = any(
        isinstance(value, numbers.Real) and not isinstance(value, numbers.Integral)
        for frame in values
        for value in frame
    )
    data_type = _creation_data_type(data_type, floating)
    if data_type == SPE_UINT16:
        for value in (value for frame in values for value in frame):
            if isinstance(value, numbers.Integral) and not 0 <= value <= 65535:
                raise ValueError("uint16 pixel value out of range")
    typecode = {SPE_FLOAT32: "f", SPE_INT32: "i"}.get(data_type, "H")
    try:
        packed = [array.array(typecode, frame).tobytes() for frame in values]
    except (TypeError, ValueError, OverflowError) as error:
        names = {"f": "float32", "i": "int32", "H": "uint16"}
        raise ValueError(f"cannot convert pixels to {names[typecode]}") from error
    return width, height, data_type, b"".join(packed)


def create(path, frames, exposure_seconds, *, temperature_setpoint=None, data_type=None, metadata=None, library=None):
    """Create an SPE 3.0 file from equally shaped uint16 or float32 frames, or a WinSpec LONG (SPE 2.2) file from int32.

    numpy int32 frames select int32 unless data_type says otherwise; SPE 3.0 has no signed 32-bit pixel format, so
    such a file has the 2.2 header WinSpec writes for accumulated data and no XML footer.

    metadata is a mapping (or SpeMetadata) of the optional SpeMetadata fields: accumulations, roi
    (x, y, width, height in sensor pixels), binning, readout_rate_hz, controller_gain, detector_id,
    detector_name, chip_width, chip_height, temperature_celsius, setpoint_celsius, shutter
    ("normal", "closed" or "opened") and acquired_utc (Unix seconds; the time of writing when omitted).
    SPE 3.0 also records requested_exposure_seconds, requested_roi, requested_binning, target_source,
    lock_tolerance_celsius, lock_hold_seconds, lock_settle_seconds, all stream_* fields together, and
    calibration_status in its provenance XML. Omitted extended fields are unknown and read back as None.
    temperature_setpoint, when given, sets setpoint_celsius.
    """
    frames = list(frames)
    if not frames:
        raise ValueError("no frames to save")
    try:
        import numpy
    except ImportError:
        width, height, data_type, pixels = _pack_plain_frames(frames, data_type)
    else:
        width, height, data_type, pixels = _pack_numpy_frames(frames, data_type, numpy)
    native = _native_metadata(exposure_seconds, metadata)
    if temperature_setpoint is not None:
        native.setpoint_known = 1
        native.setpoint_celsius = float(temperature_setpoint)
    library = _load_library(library)
    pixel_pointer = ctypes.c_char_p(pixels)
    status = library.ost_spe_create_with_metadata(
        os.fsencode(path),
        width,
        height,
        data_type,
        len(frames),
        pixel_pointer,
        len(pixels),
        ctypes.byref(native),
    )
    _check(status, f"create SPE file {os.fspath(path)!r}")


class SpeFile:
    def __init__(self, path, *, library=None):
        self._library = _load_library(library)
        self._handle = ctypes.c_void_p()
        status = self._library.ost_spe_read(os.fsencode(path), ctypes.byref(self._handle))
        _check(status, f"read SPE file {os.fspath(path)!r}")
        info = _Info()
        try:
            _check(self._library.ost_spe_get_info(self._handle, ctypes.byref(info)), "read SPE metadata")
            regions = []
            for index in range(info.region_count):
                region = _Region()
                _check(
                    self._library.ost_spe_get_region(self._handle, index, ctypes.byref(region)),
                    f"read SPE region {index}",
                )
                regions.append(SpeRegion(region.width, region.height, region.size, region.stride))
            self.info = SpeInfo(
                info.major_version,
                info.header_version,
                info.data_type,
                info.width,
                info.height,
                info.frame_count,
                info.region_count,
                info.pixel_count,
                info.pixel_size,
                info.exposure_seconds,
                info.temperature_setpoint,
                os.fsdecode(info.date),
                os.fsdecode(info.software_version),
                tuple(regions),
            )
        except BaseException:
            self._library.ost_spe_free(self._handle)
            self._handle = None
            raise

    def close(self):
        handle, self._handle = self._handle, None
        if handle:
            self._library.ost_spe_free(handle)

    def __enter__(self):
        if self._handle is None:
            raise ValueError("SPE file is closed")
        return self

    def __exit__(self, exception_type, exception, traceback):
        self.close()

    def _pointer_bytes(self, function):
        if self._handle is None:
            raise ValueError("SPE file is closed")
        pointer = ctypes.c_void_p()
        size = ctypes.c_size_t()
        _check(function(self._handle, ctypes.byref(pointer), ctypes.byref(size)), "read SPE bytes")
        return ctypes.string_at(pointer, size.value)

    @property
    def metadata(self):
        if self._handle is None:
            raise ValueError("SPE file is closed")
        native = _Metadata()
        _check(self._library.ost_spe_get_metadata(self._handle, ctypes.byref(native)), "read SPE metadata")
        metadata = _python_metadata(native)
        if self.info.major_version == 3:
            return replace(metadata, **_provenance_metadata(self.xml_footer))
        return metadata

    @property
    def header(self):
        return self._pointer_bytes(self._library.ost_spe_header)

    @property
    def xml_footer(self):
        if self.info.major_version != 3:
            return None
        return self._pointer_bytes(self._library.ost_spe_xml_footer).decode("utf-8")

    def frame(self, index):
        if self._handle is None:
            raise ValueError("SPE file is closed")
        if index < 0 or index >= self.info.frame_count:
            raise IndexError("SPE frame index out of range")
        buffer_size = sum(region.size for region in self.info.regions)
        buffer = ctypes.create_string_buffer(buffer_size)
        pixel_bytes = ctypes.c_size_t()
        status = self._library.ost_spe_read_frame(
            self._handle, index, buffer, buffer_size, ctypes.byref(pixel_bytes)
        )
        if status == SPE_ERR_RANGE:
            raise IndexError("SPE frame index out of range")
        _check(status, f"read SPE frame {index}")
        typecodes = {0: "f", 1: "i", 2: "h", 3: "H", 5: "d", 6: "B", 8: "I"}
        try:
            values = array.array(typecodes[self.info.data_type])
        except KeyError as error:
            raise OSError(f"unsupported SPE datatype {self.info.data_type}") from error
        if values.itemsize != self.info.pixel_size:
            raise OSError(f"unsupported array typecode for SPE datatype {self.info.data_type}")
        values.frombytes(buffer.raw[: pixel_bytes.value])
        if sys.byteorder != "little":
            values.byteswap()
        return values

    @property
    def data(self):
        return [self.frame(index) for index in range(self.info.frame_count)]

    def write(self, path):
        if self._handle is None:
            raise ValueError("SPE file is closed")
        status = self._library.ost_spe_write(self._handle, os.fsencode(path))
        _check(status, f"write SPE file {os.fspath(path)!r}")


def read(path, *, library=None):
    with SpeFile(path, library=library) as spe:
        info = spe.info
        frames = spe.data
    try:
        import numpy
    except ImportError:
        return info, frames
    shape = (info.height, info.width) if info.height * info.width == info.pixel_count else (info.pixel_count,)
    return info, [numpy.asarray(frame).reshape(shape) for frame in frames]
