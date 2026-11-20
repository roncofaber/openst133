# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT

import argparse
import os
from pathlib import Path

import numpy
from openst133.camera import SimulatedCamera


def acquire_to_file(output, camera=None):
    output = Path(output)
    if output.suffix.lower() not in {".npy", ".spe"}:
        raise ValueError("output name must end in .npy or .spe")
    owns_camera = camera is None
    camera = camera if camera is not None else SimulatedCamera(sleep_scale=0.0, seed=1)
    if owns_camera:
        camera.open()
    try:
        camera.set_exposure(0.01)
        frame = camera.acquire()
        if output.suffix.lower() == ".spe":
            from openst133 import spe

            exposure = getattr(camera, "effective_exposure", None)
            if exposure is None:
                exposure = camera.exposure
            spe.create(os.fspath(output), [frame], exposure)
        else:
            numpy.save(output, frame)
        return frame
    finally:
        if owns_camera:
            camera.close()


def main():
    parser = argparse.ArgumentParser(description="Acquire one simulated frame to NPY or SPE")
    parser.add_argument("output", type=Path, help="output file ending in .npy or .spe")
    arguments = parser.parse_args()
    frame = acquire_to_file(arguments.output)
    print(f"saved {frame.shape[0]}x{frame.shape[1]} {frame.dtype} frame to {arguments.output}")


if __name__ == "__main__":
    main()
