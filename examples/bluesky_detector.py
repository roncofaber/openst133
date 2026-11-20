# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT

"""A plain Python detector implementing the Bluesky trigger/read/describe protocols."""

import threading
import time
from concurrent.futures import ThreadPoolExecutor

try:
    from ophyd import DeviceStatus
except ImportError as error:
    DeviceStatus = None
    _OPHYD_IMPORT_ERROR = error
else:
    _OPHYD_IMPORT_ERROR = None

from openst133.camera import SimulatedCamera


class OpenST133Detector:
    def __init__(self, name="st133", camera=None, exposure=0.01, timeout=None):
        self.name = name
        self.camera = camera if camera is not None else SimulatedCamera(sleep_scale=0.0, seed=1)
        self.timeout = timeout
        self._executor = ThreadPoolExecutor(max_workers=1)
        self._lock = threading.Lock()
        self._future = None
        self._frame = None
        self._timestamp = None
        self.camera.open()
        self.camera.set_exposure(exposure)

    @property
    def image_name(self):
        return f"{self.name}_image"

    def trigger(self):
        if DeviceStatus is None:
            raise RuntimeError("trigger needs the optional ophyd package") from _OPHYD_IMPORT_ERROR
        with self._lock:
            if self._future is not None and not self._future.done():
                raise RuntimeError("an openst133 acquisition is already running")
            status = DeviceStatus(self, timeout=self.timeout)
            self._future = self._executor.submit(self.camera.acquire)
            self._future.add_done_callback(lambda future: self._finished(future, status))
            return status

    def _finished(self, future, status):
        try:
            frame = future.result()
        except Exception as error:
            status.set_exception(error)
            return
        self._frame = frame
        self._timestamp = time.time()
        status.set_finished()

    def read(self):
        if self._frame is None:
            raise RuntimeError("no frame is available; trigger and wait for completion first")
        return {self.image_name: {"value": self._frame, "timestamp": self._timestamp}}

    def describe(self):
        if self._frame is None:
            raise RuntimeError("no frame shape is available; trigger and wait for completion first")
        return {
            self.image_name: {
                "source": "openst133",
                "dtype": "array",
                "shape": list(self._frame.shape),
                "dtype_numpy": self._frame.dtype.str,
            }
        }

    def close(self):
        self.camera.abort()
        self._executor.shutdown(wait=True)
        self.camera.close()

    def __enter__(self):
        return self

    def __exit__(self, exception_type, exception, traceback):
        self.close()


def main():
    if DeviceStatus is None:
        raise SystemExit("Install ophyd in your own environment, then run this example again")
    with OpenST133Detector() as detector:
        status = detector.trigger()
        status.wait(10)
        reading = detector.read()[detector.image_name]
        frame = reading["value"]
        print(f"{detector.image_name}: shape={frame.shape}, dtype={frame.dtype}, mean={frame.mean():.1f}")
        print(f"descriptor: {detector.describe()[detector.image_name]}")


if __name__ == "__main__":
    main()
