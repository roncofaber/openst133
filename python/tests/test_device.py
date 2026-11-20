# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT

import os
import pathlib
import subprocess
import sys
import unittest

from openst133 import Device, list_devices


class DeviceTest(unittest.TestCase):
    def test_discovery_returns_device_values(self):
        devices = list_devices()
        for device in devices:
            self.assertIsInstance(device, Device)
            self.assertEqual((device.vendor_id, device.product_id), (0x0BD7, 0xA010))
            self.assertGreaterEqual(device.bus_number, 0)
            self.assertGreaterEqual(device.device_address, 0)

    def test_module_lists_discovered_interfaces(self):
        source = pathlib.Path(__file__).resolve().parent.parent / "src"
        environment = os.environ.copy()
        environment["PYTHONPATH"] = str(source)
        result = subprocess.run(
            [sys.executable, "-m", "openst133", "list"],
            check=False,
            capture_output=True,
            text=True,
            env=environment,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")
        expected = [
            f"{device.bus_number:03d}:{device.device_address:03d} "
            f"{device.vendor_id:04x}:{device.product_id:04x} {device.port_path or '-'}"
            for device in list_devices()
        ]
        self.assertEqual(result.stdout.splitlines(), expected)


if __name__ == "__main__":
    unittest.main()
