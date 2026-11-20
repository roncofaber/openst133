# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT
"""Save frames as SPE, HDF5, TIFF or CSV, chosen by the file extension."""

import datetime
import json
import os
from importlib import metadata as package_metadata
from pathlib import Path

import numpy

from .spe import _WRITABLE_METADATA as SPE_FIELDS
from .spe import create as write_spe

FORMATS = {
    ".spe": "SPE",
    ".h5": "HDF5",
    ".hdf5": "HDF5",
    ".tif": "TIFF",
    ".tiff": "TIFF",
    ".csv": "CSV",
}
FILE_TYPES = [
    ("SPE", "*.spe"),
    ("HDF5", "*.h5 *.hdf5"),
    ("TIFF", "*.tif *.tiff"),
    ("CSV (spectra)", "*.csv"),
]


def effective_exposure(camera, requested=None):
    """The exposure in seconds the camera used after its encoding, or requested when the camera cannot say."""
    try:
        value = getattr(camera, "effective_exposure", None)
    except Exception:
        value = None
    return float(requested if value is None else value)


def acquisition_metadata(camera, temperature=True):
    """Settings metadata for frames about to be acquired, shared by every openst133 front end.

    Holds the requested exposure as passed by the user, readout, gain, shutter, rate, accumulations, detector
    identity, setpoint and, read while no acquisition runs, the sensor temperature. Store the effective exposure
    (effective_exposure) as the frame exposure. Returns {} for a camera that reports no settings.
    """
    describe = getattr(camera, "spe_metadata", None)
    try:
        metadata = dict(describe()) if describe is not None else {}
    except Exception:
        return {}
    if temperature and hasattr(camera, "temperature"):
        try:
            metadata["temperature_celsius"] = round(float(camera.temperature()), 2)
        except Exception:
            pass
    return metadata


def with_extension(path, file_type):
    """Append the first extension of the chosen file type (a FILE_TYPES name) when path has none."""
    path = str(path)
    if Path(path).suffix:
        return path
    for name, patterns in FILE_TYPES:
        if file_type == name or str(file_type).startswith(f"{name} ("):
            return path + patterns.split()[0].lstrip("*")
    return path


def file_format(path):
    """The format name for a path's extension; ValueError names the supported extensions otherwise."""
    suffix = Path(path).suffix.lower()
    if suffix not in FORMATS:
        raise ValueError(f"unknown file type {suffix or '(none)'}: use one of {', '.join(FORMATS)}")
    return FORMATS[suffix]


def software_version():
    """The installed openst133 version, or "unknown" when running from a source tree that was never installed."""
    try:
        return package_metadata.version("openst133")
    except package_metadata.PackageNotFoundError:
        return "unknown"


def _attributes(exposure_seconds, metadata):
    attributes = {
        "exposure_seconds": float(exposure_seconds),
        "software": f"openst133 {software_version()}",
        "saved_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
    }
    for key, value in (metadata or {}).items():
        if value is not None:
            attributes[key] = list(value) if isinstance(value, tuple) else value
    return attributes


def sensor_columns(width, metadata):
    """Sensor column of each point in a frame row, from the readout region and binning when they are known."""
    try:
        x = int(metadata["roi"][0])
        x_binning = int(metadata["binning"][0])
    except (KeyError, TypeError, ValueError, IndexError):
        return numpy.arange(width)
    return x + numpy.arange(width) * x_binning


def _write_hdf5(path, stack, attributes):
    try:
        import h5py
    except ImportError:
        raise ValueError(
            "HDF5 needs the h5py package, which has no build for this Python version yet; save as TIFF or SPE"
        ) from None
    with h5py.File(path, "x") as handle:
        dataset = handle.create_dataset("frames", data=stack, compression="gzip", shuffle=True)
        dataset.attrs["dimensions"] = "frame, row, column"
        for key, value in attributes.items():
            handle.attrs[key] = value
        if stack.shape[1] == 1:
            handle.create_dataset("sensor_column", data=sensor_columns(stack.shape[2], attributes))


def _write_tiff(path, stack, attributes):
    import tifffile

    with open(path, "xb") as handle:
        tifffile.imwrite(handle, stack, photometric="minisblack", metadata=attributes)


def _write_csv(path, stack, attributes):
    if stack.shape[1] != 1:
        raise ValueError("CSV holds spectra (one-row frames such as FVB); save images as HDF5, TIFF or SPE")
    columns = sensor_columns(stack.shape[2], attributes)
    with open(path, "x", newline="") as handle:
        for key, value in attributes.items():
            handle.write(f"# {key}: {json.dumps(value)}\n")
        handle.write(",".join(["sensor_column"] + [f"frame_{index}" for index in range(stack.shape[0])]) + "\n")
        for column, values in zip(columns, stack[:, 0, :].T):
            handle.write(",".join(str(int(value)) for value in (column, *values)) + "\n")


def save_frames(path, frames, exposure_seconds, metadata=None):
    """Write equally shaped frames to path in the format its extension names, refusing to overwrite a file."""
    kind = file_format(path)
    frames = list(frames)
    if not frames:
        raise ValueError("no frames to save")
    if kind == "SPE":
        fields = {key: value for key, value in (metadata or {}).items() if key in SPE_FIELDS}
        return write_spe(path, frames, exposure_seconds, metadata=fields)
    if os.path.exists(path):
        raise FileExistsError(f"{path} already exists")
    stack = numpy.stack([numpy.asarray(frame) for frame in frames])
    attributes = _attributes(exposure_seconds, metadata)
    {"HDF5": _write_hdf5, "TIFF": _write_tiff, "CSV": _write_csv}[kind](path, stack, attributes)
    return None
