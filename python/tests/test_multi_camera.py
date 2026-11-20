# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT

"""Camera selection with two simulated devices; nothing is opened on USB."""

import contextlib
import os
import ctypes
import io
import pathlib
import re
import unittest
from unittest import mock

from openst133 import __main__ as cli
from openst133 import device
from openst133.camera import CameraUnavailable, RealCamera
from openst133.device import Device, select_device

HEADER = pathlib.Path(__file__).resolve().parents[2] / "include" / "openst133.h"

FIRST = Device(0x0BD7, 0xA010, 1, 12, (2,))
SECOND = Device(0x0BD7, 0xA010, 1, 9, (3,))
CAMERAS = [FIRST, SECOND]


class DeviceLayoutTest(unittest.TestCase):
    def test_device_info_mirrors_the_c_structure(self):
        fields = re.search(r"typedef struct ost_device_info\s*\{(.*?)\}", HEADER.read_text(), re.DOTALL)
        self.assertIsNotNone(fields)
        c_names = re.findall(r"^\s*uint(?:8|16)_t\s+([a-z_]+)", fields.group(1), re.MULTILINE)
        self.assertEqual([name for name, _kind in device._CDeviceInfo._fields_], c_names)
        depth = re.search(r"#define OST_MAX_PORT_DEPTH (\d+)u", HEADER.read_text())
        self.assertEqual(int(depth.group(1)), device.MAX_PORT_DEPTH)
        self.assertEqual(ctypes.sizeof(device._CDeviceInfo), 7 + device.MAX_PORT_DEPTH)

    def test_port_path_and_conversion(self):
        self.assertEqual(Device(0x0BD7, 0xA010, 3, 9, (4, 2, 1)).port_path, "3-4.2.1")
        self.assertIsNone(Device(0x0BD7, 0xA010, 1, 5).port_path)
        info = device._device_to_c(Device(0x0BD7, 0xA010, 3, 9, (4, 2)))
        self.assertEqual((info.bus_number, info.device_address, info.port_count), (3, 9, 2))
        self.assertEqual(list(info.port_numbers), [4, 2, 0, 0, 0, 0, 0])
        with self.assertRaises(ValueError):
            device._device_to_c(Device(0x0BD7, 0xA010, 1, 1, tuple(range(1, 9))))


class SelectDeviceTest(unittest.TestCase):
    def test_each_selector_form_picks_its_camera(self):
        for selector, expected in (
            ("0", FIRST),
            ("1", SECOND),
            ("1-2", FIRST),
            ("1-3", SECOND),
            ("1:12", FIRST),
            ("1:9", SECOND),
        ):
            with self.subTest(selector=selector):
                self.assertIs(select_device(CAMERAS, selector), expected)

    def test_the_only_camera_needs_no_selector(self):
        self.assertIs(select_device([SECOND]), SECOND)
        self.assertIs(select_device([SECOND], ""), SECOND)

    def test_several_cameras_without_a_selector_are_refused_with_the_choices(self):
        with self.assertRaises(ValueError) as raised:
            select_device(CAMERAS)
        self.assertIn("2 ST-133 cameras are attached", str(raised.exception))
        self.assertIn("0: 1-2 (1:12); 1: 1-3 (1:9)", str(raised.exception))

    def test_unmatched_and_malformed_selectors(self):
        for selector in ("2", "1-4", "1:13", "2-2"):
            with self.subTest(selector=selector), self.assertRaises(LookupError):
                select_device(CAMERAS, selector)
        for selector in ("x", "1-", "1:", "1-2.", "é"):
            with self.subTest(selector=selector), self.assertRaises(ValueError):
                select_device(CAMERAS, selector)
        with self.assertRaises(LookupError):
            select_device([], None)


class RealCameraSelectionTest(unittest.TestCase):
    def _open(self, **arguments):
        native = mock.MagicMock()
        native.geometry = (1340, 100, True)
        with mock.patch("openst133.camera.list_devices", return_value=list(CAMERAS)), mock.patch(
            "openst133.camera.NativeCamera", return_value=native
        ) as constructor:
            camera = RealCamera(transport=object(), **arguments)
            camera.open()
        return constructor

    def test_selector_opens_the_chosen_camera(self):
        constructor = self._open(selector="1-3")
        self.assertIs(constructor.call_args.args[0], SECOND)
        constructor = self._open(selector="0")
        self.assertIs(constructor.call_args.args[0], FIRST)

    def test_two_cameras_without_selector_do_not_open_the_first(self):
        with self.assertRaises(CameraUnavailable) as raised:
            self._open()
        self.assertIn("choose one", str(raised.exception))

    def test_unmatched_selector_is_unavailable(self):
        with self.assertRaises(CameraUnavailable) as raised:
            self._open(selector="1-7")
        self.assertIn("no attached ST-133 matches", str(raised.exception))

    def test_device_and_selector_are_exclusive(self):
        with self.assertRaises(ValueError):
            RealCamera(device=FIRST, selector="0")


class CommandLineSelectionTest(unittest.TestCase):
    def test_camera_with_simulate_is_refused(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as raised:
            cli.main(["acquire", "--simulate", "--camera", "0", "--out", "unused.spe"])
        self.assertEqual(raised.exception.code, 2)

    def test_camera_reaches_the_acquisition(self):
        with mock.patch("openst133.viewer.run_acquire", return_value=0) as run:
            self.assertEqual(cli.main(["acquire", "--camera", "1-3", "--out", "unused.spe"]), 0)
        self.assertEqual(run.call_args.args[-1], "1-3")

    def test_list_prints_the_port_path(self):
        output = io.StringIO()
        with mock.patch("openst133.device.list_devices", return_value=[FIRST, Device(0x0BD7, 0xA010, 2, 4)]):
            with contextlib.redirect_stdout(output):
                self.assertEqual(cli.main(["list"]), 0)
        self.assertEqual(output.getvalue().splitlines(), ["001:012 0bd7:a010 1-2", "002:004 0bd7:a010 -"])


if __name__ == "__main__":
    unittest.main()
