# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT

"""Call openst133's C SPE writer directly, using a simulated frame as input."""

import argparse
import ctypes
import os
from pathlib import Path

import numpy
from openst133.camera import SimulatedCamera

OST_SPE_UINT16 = 3


def bind_spe_writer(library):
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
    return library


def write_uint16_spe(library, path, frame, exposure_seconds):
    pixels = numpy.ascontiguousarray(frame, dtype="<u2")
    if pixels.ndim != 2 or 0 in pixels.shape:
        raise ValueError("frame must be a nonempty two-dimensional array")
    height, width = pixels.shape
    status = library.ost_spe_create(
        os.fsencode(path),
        width,
        height,
        OST_SPE_UINT16,
        1,
        ctypes.c_void_p(pixels.ctypes.data),
        pixels.nbytes,
        float(exposure_seconds),
        None,
    )
    if status != 0:
        raise OSError(status, f"ost_spe_create failed for {os.fspath(path)}")


def main():
    parser = argparse.ArgumentParser(description="Write a simulated frame through libopenst133 using ctypes")
    parser.add_argument("output", type=Path)
    parser.add_argument("--library", default=os.environ.get("OPENST133_LIBRARY"), help="path to libopenst133.so or openst133.dll")
    arguments = parser.parse_args()
    if arguments.library is None:
        parser.error("pass --library or set OPENST133_LIBRARY to the built shared library")
    library = bind_spe_writer(ctypes.CDLL(arguments.library))
    with SimulatedCamera(width=64, height=32, sleep_scale=0.0, seed=1) as camera:
        camera.set_exposure(0.01)
        frame = camera.acquire()
        write_uint16_spe(library, arguments.output, frame, camera.exposure)
    print(f"C ost_spe_create wrote {frame.shape[0]}x{frame.shape[1]} frame to {arguments.output}")


if __name__ == "__main__":
    main()
