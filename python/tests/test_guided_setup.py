# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT

import contextlib
import hashlib
import io
from pathlib import Path
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from openst133 import guided_setup
from openst133.device import Device

CAMERA = Device(0x0BD7, 0xA010, 1, 12, (1,))


class GuidedSetupTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.output = Path(self.temporary.name) / "firmware" / "st133-startup.bin"
        patches = [
            mock.patch("openst133.startup.default_output", return_value=str(self.output)),
            mock.patch.dict("os.environ", {"OPENST133_NO_HARDWARE": ""}),
            mock.patch.object(guided_setup, "find_winspec", return_value=None),
            mock.patch.object(guided_setup, "ask", return_value=""),
        ]
        for patch in patches:
            patch.start()
            self.addCleanup(patch.stop)

    def run_setup(self, devices, winspec=None, probe=None, generate=None, dat=None, dll=None):
        output = io.StringIO()
        with mock.patch("openst133.device.list_devices", return_value=devices), \
                mock.patch.object(guided_setup, "probe_access", return_value=probe) as probed, \
                mock.patch("openst133.startup.run", side_effect=generate or (lambda arguments: 0)) as generated, \
                contextlib.redirect_stdout(output):
            status = guided_setup.run(SimpleNamespace(winspec=winspec, dat=dat, dll=dll, skip_udev=True))
        return status, output.getvalue(), probed, generated

    def write_startup_data(self):
        self.output.parent.mkdir(parents=True)
        self.output.write_bytes(b"startup")
        digest = hashlib.sha256(b"startup").hexdigest()
        self.output.with_name(self.output.name + ".sha256").write_text(f"{digest}  {self.output.name}\n")

    def test_missing_camera_and_startup_data_is_not_ready(self):
        status, text, probed, generated = self.run_setup([])
        self.assertEqual(status, 1)
        self.assertIn("no ST-133", text)
        self.assertIn("--dat /path/to/PI133B.DAT --dll /path/to/PICHIPDB.dll", text)
        probed.assert_not_called()
        generated.assert_not_called()

    def test_builds_startup_data_from_the_given_folder(self):
        winspec = Path(self.temporary.name) / "WinSpec"
        winspec.mkdir()
        for name in guided_setup.REQUIRED_FILES:
            (winspec / name).write_bytes(b"x")
        status, text, probed, generated = self.run_setup([CAMERA], winspec=str(winspec))
        self.assertEqual(status, 0)
        self.assertEqual(generated.call_args.args[0].dat, winspec / "PI133B.DAT")
        self.assertEqual(generated.call_args.args[0].dll, winspec / "PICHIPDB.dll")
        self.assertIn("ost acquire --exposure 0.1", text)

    def test_builds_startup_data_from_individual_files(self):
        dat = Path(self.temporary.name) / "a" / "pi133b.dat"
        dll = Path(self.temporary.name) / "b" / "chipdb.dll"
        for path in (dat, dll):
            path.parent.mkdir()
            path.write_bytes(b"x")
        status, _, _, generated = self.run_setup([CAMERA], dat=str(dat), dll=str(dll))
        self.assertEqual(status, 0)
        self.assertEqual((generated.call_args.args[0].dat, generated.call_args.args[0].dll), (dat, dll))

    def test_prompts_for_each_file_when_interactive(self):
        dat = Path(self.temporary.name) / "PI133B.DAT"
        dll = Path(self.temporary.name) / "PICHIPDB.dll"
        for path in (dat, dll):
            path.write_bytes(b"x")
        with mock.patch.object(guided_setup, "ask", side_effect=[str(dat), str(dll)]):
            status, _, _, generated = self.run_setup([CAMERA])
        self.assertEqual(status, 0)
        self.assertEqual((generated.call_args.args[0].dat, generated.call_args.args[0].dll), (dat, dll))

    def test_folder_without_winspec_files_is_refused(self):
        status, text, _, generated = self.run_setup([CAMERA], winspec=self.temporary.name)
        self.assertEqual(status, 1)
        self.assertIn("file not found", text)
        generated.assert_not_called()

    def test_access_failure_is_reported_with_guidance(self):
        self.write_startup_data()
        with mock.patch.object(guided_setup.os, "name", "nt"):
            status, text, _, _ = self.run_setup([CAMERA], probe="permission denied opening camera USB interface 0")
        self.assertEqual(status, 1)
        self.assertIn("WinUsb Device", text)

    def test_ready_when_everything_is_in_place(self):
        self.write_startup_data()
        status, text, probed, generated = self.run_setup([CAMERA])
        self.assertEqual(status, 0)
        self.assertIn("matching its sidecar", text)
        probed.assert_called_once_with(CAMERA)
        generated.assert_not_called()


if __name__ == "__main__":
    unittest.main()
