# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT

import ctypes
import ctypes.util
import errno
import os
import pathlib

NO_HARDWARE_ENV = "OPENST133_NO_HARDWARE"
NO_HARDWARE_ERROR = "OPENST133_NO_HARDWARE=1 forbids opening real hardware; use replay or simulated transport"

_LIBRARY_NAMES = ("libopenst133.so", "libopenst133.dylib", "openst133.dll")


def bundled_file(name):
    """A native file installed in the package; importlib.resources also resolves an editable install, which keeps
    sources in the checkout and built files in site-packages."""
    from importlib import resources

    resource = resources.files(__package__) / name
    return pathlib.Path(str(resource)).resolve() if resource.is_file() else None


def load(path=None, *, hardware=False):
    if hardware and os.environ.get(NO_HARDWARE_ENV) == "1":
        raise PermissionError(errno.EPERM, NO_HARDWARE_ERROR)
    candidate = path or os.environ.get("OPENST133_LIBRARY")
    if candidate is None:
        candidate = next(filter(None, map(bundled_file, _LIBRARY_NAMES)), None)
    if candidate is None:
        candidate = ctypes.util.find_library("openst133")
    if candidate is None:
        raise OSError("openst133 shared library not found; set OPENST133_LIBRARY to its path")
    return ctypes.CDLL(os.fspath(candidate))
