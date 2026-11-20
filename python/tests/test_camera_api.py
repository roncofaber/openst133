# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT

import ctypes
import os
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import numpy

from openst133._library import load
from openst133.camera import CameraError, CameraUnavailable, RealCamera, SimulatedCamera
from openst133.device import (
    Device,
    NativeCamera,
    NativeError,
    list_devices,
)

SOURCE = str(pathlib.Path(__file__).resolve().parents[1] / "src")
WIDTH, HEIGHT = 1340, 100
FRAME_BYTES = WIDTH * HEIGHT * 2
CHUNK = 0x8000
DESCRIPTOR = bytes([0x12, 0x01, 0x00, 0x02, 0, 0, 0, 0x40, 0xD7, 0x0B, 0x10, 0xA0, 0, 1, 1, 2, 3, 1])


def library_available():
    try:
        load()
    except OSError:
        return False
    return True


def pattern(length, start=0):
    return bytes(((i * 7 + (i >> 8)) & 0xFF) for i in range(start, start + length))


class Script:
    """Fixture: expected-transfer script written as a usbmon capture for the replay transport."""

    def __init__(self):
        self.records = []
        self._add("S", 2, 0x80, 0x80, 6, 0x0100, 64, 0, 64, b"")
        self._add("C", 2, 0x80, 0x80, 6, 0x0100, 64, 0, len(DESCRIPTOR), DESCRIPTOR)

    def _add(self, event, ttype, endpoint, request_type, request, value, setup_length, status, length, data):
        self.records.append((event, ttype, endpoint, request_type, request, value, setup_length, status, length, data))

    def bulk_out(self, data):
        self._add("S", 3, 0x08, 0, 0, 0, 0, 0, len(data), data)
        self._add("C", 3, 0x08, 0, 0, 0, 0, 0, len(data), b"")

    def bulk_in(self, requested, data):
        self._add("S", 3, 0x82, 0, 0, 0, 0, 0, requested, b"")
        self._add("C", 3, 0x82, 0, 0, 0, 0, 0, len(data), data)

    def vendor_out(self, request, value):
        zeros = bytes(8)
        self._add("S", 2, 0, 0x40, request, value, 8, 0, 8, zeros)
        self._add("C", 2, 0, 0x40, request, value, 8, 0, 8, b"")

    def frame_setup(self):
        self.bulk_out(bytes([0x01, 0x14, 0x00, 0x02, 0x00, 0x01]))
        self.bulk_out(bytes([0x01, 0x10, 0x00, 0x02, FRAME_BYTES & 0xFF, (FRAME_BYTES >> 8) & 0xFF]))
        self.bulk_out(bytes([0x01, 0x12, 0x00, 0x02, (FRAME_BYTES >> 16) & 0xFF, (FRAME_BYTES >> 24) & 0xFF]))
        self.bulk_out(bytes([0x01, 0x14, 0x00, 0x02, 0x01, 0x00]))
        self.vendor_out(0xF2, CHUNK)
        self.bulk_out(bytes([0x01, 0x16, 0x00, 0x02, 0x01, 0x00]))

    def frame_chunks(self):
        done = 0
        while done < FRAME_BYTES:
            size = min(CHUNK, FRAME_BYTES - done)
            self.bulk_in(size, pattern(size, done))
            done += size

    def write(self, path):
        out = [struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, 189)]
        for index, record in enumerate(self.records):
            event, ttype, endpoint, request_type, request, value, setup_length, status, length, data = record
            header = bytearray(48)
            struct.pack_into("<Q", header, 0, 0x0102030405060700 + index // 2)
            header[8] = ord(event)
            header[9] = ttype
            header[10] = endpoint
            header[11] = 7
            struct.pack_into("<H", header, 12, 2)
            header[14] = 0 if event == "S" and ttype == 2 else 0xFF
            header[15] = 0xFF if not data else 0
            struct.pack_into("<Q", header, 16, 100)
            struct.pack_into("<I", header, 24, index + 1)
            struct.pack_into("<i", header, 28, status)
            struct.pack_into("<I", header, 32, length)
            struct.pack_into("<I", header, 36, len(data))
            header[40] = request_type
            header[41] = request
            struct.pack_into("<H", header, 42, value)
            struct.pack_into("<H", header, 46, setup_length)
            size = 48 + len(data)
            out.append(struct.pack("<IIII", 100, index + 1, size, size))
            out.append(bytes(header))
            out.append(data)
        pathlib.Path(path).write_bytes(b"".join(out))


class HardwareGuardTest(unittest.TestCase):
    @patch.dict(os.environ, {"OPENST133_NO_HARDWARE": "1"})
    def test_real_open_is_refused_before_library_loading(self):
        device = Device(0x0BD7, 0xA010, 255, 255)
        with self.assertRaises(PermissionError) as context:
            NativeCamera(device, path="/must-not-be-loaded/libopenst133.so")
        self.assertIn("OPENST133_NO_HARDWARE", str(context.exception))
        with self.assertRaises(CameraUnavailable) as context:
            RealCamera(
                device=device,
                library_path="/must-not-be-loaded/libopenst133.so",
            ).open()
        self.assertIn("OPENST133_NO_HARDWARE", str(context.exception))


@unittest.skipUnless(library_available(), "openst133 shared library not found")
class ReplayCameraTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.library = load()
        self.library.ost_replay_transport_create.argtypes = [
            ctypes.c_char_p,
            ctypes.POINTER(ctypes.c_void_p),
            ctypes.c_char_p,
            ctypes.c_size_t,
        ]
        self.library.ost_replay_transport_interface.argtypes = [ctypes.c_void_p]
        self.library.ost_replay_transport_interface.restype = ctypes.c_void_p
        self.library.ost_replay_transport_destroy.argtypes = [ctypes.c_void_p]
        self.library.ost_replay_transport_destroy.restype = None

    def transport(self, script):
        path = os.path.join(self.directory.name, "script.pcap")
        script.write(path)
        replay = ctypes.c_void_p()
        error = ctypes.create_string_buffer(256)
        status = self.library.ost_replay_transport_create(
            os.fsencode(path), ctypes.byref(replay), error, len(error)
        )
        self.assertEqual(status, 0, error.value)
        self.addCleanup(self.library.ost_replay_transport_destroy, replay)
        return self.library.ost_replay_transport_interface(replay)

    @patch.dict(os.environ, {"OPENST133_NO_HARDWARE": "1"})
    def test_guard_allows_replay_and_simulated_cameras(self):
        with RealCamera(transport=self.transport(Script())) as replay:
            self.assertEqual((replay.width, replay.height), (WIDTH, HEIGHT))
            replay.abort()
        with SimulatedCamera(width=3, height=2, sleep_scale=0.0, seed=1) as simulated:
            frame = simulated.acquire()
        self.assertEqual(frame.shape, (2, 3))

    def test_acquire_set_exposure_and_unsupported_temperature(self):
        script = Script()
        script.frame_setup()
        script.frame_chunks()
        reports = []
        camera = RealCamera(transport=self.transport(script), report=reports.append)
        camera.open()
        self.addCleanup(camera.close)
        self.assertEqual((camera.width, camera.height), (WIDTH, HEIGHT))
        self.assertEqual(camera.unsupported, ["temperature read", "temperature setpoint"])
        self.assertEqual(len(reports), 1)
        self.assertIn("not supported yet", reports[0])
        camera.set_exposure(0.2501)
        self.assertEqual(camera._native.get_exposure_ms(), 251)
        self.assertEqual(camera._native.get_effective_exposure_ms(), 251)
        self.assertEqual(camera.effective_exposure, 0.251)
        with self.assertRaises(ValueError):
            camera.set_exposure(0)
        with self.assertRaises(CameraUnavailable) as context:
            camera.temperature()
        self.assertIn("not supported yet", str(context.exception))
        frame = camera.acquire()
        self.assertEqual(frame.shape, (HEIGHT, WIDTH))
        self.assertEqual(frame.dtype, numpy.uint16)
        self.assertEqual(frame.tobytes(), pattern(FRAME_BYTES))
        camera.abort()

    def test_native_wrapper_statuses(self):
        device = list_devices(transport=self.transport(Script()))[0]
        native = NativeCamera(device, transport=self.transport(Script()))
        self.assertEqual(native.geometry, (WIDTH, HEIGHT, False))
        with self.assertRaises(ValueError):
            native.acquire_into(memoryview(bytes(4)))
        native.close()
        native.close()
        with self.assertRaises(NativeError):
            native.get_temperature()

    def test_short_buffer_and_abort_status_mapping(self):
        script = Script()
        script.frame_setup()
        script.frame_chunks()
        device = list_devices(transport=self.transport(script))[0]
        native = NativeCamera(device, transport=self.transport(script))
        self.addCleanup(native.close)
        with self.assertRaises(NativeError) as context:
            native.acquire_into(bytearray(10))
        self.assertEqual(context.exception.status, -7)


class CommandLineTest(unittest.TestCase):
    def run_cli(self, *arguments):
        environment = {**os.environ, "PYTHONPATH": SOURCE, "OPENST133_LIBRARY": "/nonexistent/libopenst133.so"}
        return subprocess.run(
            [sys.executable, "-m", "openst133", *arguments],
            env=environment,
            capture_output=True,
            text=True,
            timeout=60,
        )

    def test_missing_library_is_reported_without_touching_a_device(self):
        for arguments in (("acquire", "--out", os.devnull), ("view",)):
            result = self.run_cli(*arguments)
            self.assertEqual(result.returncode, 2, result.stderr)
            self.assertIn("cannot use the openst133 library", result.stderr)
            self.assertNotIn("Traceback", result.stderr)


if __name__ == "__main__":
    unittest.main()
