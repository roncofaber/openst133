# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT

import os
import unittest
from unittest import mock

from openst133 import device, guided_setup
from openst133.device import Device, NativeError, check_access

CAMERA = Device(0x0BD7, 0xA010, 1, 12, (4, 2))


def library(status=device.OST_SUCCESS, detail=b""):
    native = mock.Mock()
    native.ost_open.return_value = status
    native.ost_status_text.return_value = b"device is busy"
    native.ost_libusb_open_error.return_value = detail
    return native


class CheckAccessTest(unittest.TestCase):
    def test_opens_and_closes_the_interface_without_camera_commands(self):
        native = library()
        with mock.patch("openst133.device._load_library", return_value=native) as load:
            check_access(CAMERA)
        load.assert_called_once_with(None, hardware=True)
        info = native.ost_open.call_args.args[1]._obj
        self.assertEqual((info.bus_number, info.device_address, info.port_count), (1, 12, 2))
        native.ost_close.assert_called_once()
        self.assertFalse([name for name, *_ in native.mock_calls if name.startswith("ost_camera")])

    def test_failure_carries_the_library_reason(self):
        native = library(device.OST_ERR_BUSY, b"held by pid 42 (qemu); close the holder or VM and retry")
        with mock.patch("openst133.device._load_library", return_value=native):
            with self.assertRaises(NativeError) as raised:
                check_access(CAMERA)
        self.assertEqual(raised.exception.status, device.OST_ERR_BUSY)
        self.assertIn("device is busy: held by pid 42", str(raised.exception))
        native.ost_close.assert_not_called()

    @mock.patch.dict(os.environ, {"OPENST133_NO_HARDWARE": "1"})
    def test_no_hardware_guard_refuses_before_loading(self):
        with self.assertRaises(PermissionError):
            check_access(CAMERA, path="/must-not-be-loaded/libopenst133.so")

    def test_guided_setup_reports_the_reason(self):
        with mock.patch("openst133.device.check_access", side_effect=NativeError(device.OST_ERR_ACCESS, "denied")):
            self.assertEqual(guided_setup.probe_access(CAMERA), "denied")
        with mock.patch("openst133.device.check_access"):
            self.assertIsNone(guided_setup.probe_access(CAMERA))


if __name__ == "__main__":
    unittest.main()
