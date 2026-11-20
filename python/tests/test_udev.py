# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT

import contextlib
import io
from pathlib import Path
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from openst133 import udev
from openst133.__main__ import main


@unittest.skipUnless(sys.platform.startswith("linux"), "udev is Linux-only")
class SetupUdevTests(unittest.TestCase):
    def test_print_writes_the_rule(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            self.assertEqual(main(["setup-udev", "--print"]), 0)
        self.assertIn('ATTRS{idVendor}=="0bd7", ATTRS{idProduct}=="a010"', output.getvalue())

    def test_installs_with_sudo_reloads_and_triggers(self):
        with tempfile.TemporaryDirectory() as temporary:
            calls = []
            with mock.patch.object(udev, "RULES_DIRECTORY", Path(temporary)), \
                    mock.patch.object(udev.os, "geteuid", return_value=1000), \
                    mock.patch.object(udev.shutil, "which", return_value="/usr/bin/sudo"), \
                    mock.patch.object(udev.subprocess, "run", side_effect=lambda command, check: calls.append(command) or SimpleNamespace(returncode=0)), \
                    contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(udev.run(SimpleNamespace(print_rule=False)), 0)
        self.assertEqual([command[:2] for command in calls], [["sudo", "install"], ["sudo", "udevadm"], ["sudo", "udevadm"]])
        self.assertEqual(calls[1][2:], ["control", "--reload-rules"])
        self.assertIn("--attr-match=idProduct=a010", calls[2])

    def test_identical_rule_is_left_alone(self):
        with tempfile.TemporaryDirectory() as temporary:
            (Path(temporary) / udev.RULE_NAME).write_bytes(udev.rule_source().read_bytes())
            with mock.patch.object(udev, "RULES_DIRECTORY", Path(temporary)), \
                    mock.patch.object(udev.subprocess, "run") as run, \
                    contextlib.redirect_stdout(io.StringIO()) as output:
                self.assertEqual(udev.run(SimpleNamespace(print_rule=False)), 0)
        run.assert_not_called()
        self.assertIn("already installed", output.getvalue())


if __name__ == "__main__":
    unittest.main()
