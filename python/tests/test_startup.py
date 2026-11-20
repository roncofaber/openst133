# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT

import contextlib
import io
import os
from pathlib import Path
import unittest
from unittest import mock

from openst133 import startup as subject
from openst133.__main__ import main as module_main


class DefaultOutputTests(unittest.TestCase):
    def test_full_file_override_has_priority(self):
        environment = {
            "OPENST133_STARTUP_DATA": "D:/private/custom.bin",
            "OPENST133_DATA": "D:/other-data",
            "APPDATA": "C:/Users/test/AppData/Roaming",
            "HOME": "/home/test",
        }
        with mock.patch.dict(os.environ, environment, clear=True):
            self.assertEqual("D:/private/custom.bin", subject.default_output())

    def test_data_root_has_priority_over_platform_defaults(self):
        for platform in ("posix", "nt"):
            with self.subTest(platform=platform):
                with mock.patch.dict(os.environ, {"OPENST133_DATA": "/data/root"}, clear=True):
                    with mock.patch.object(subject.os, "name", platform):
                        self.assertEqual("/data/root/firmware/st133-startup.bin", subject.default_output())

    def test_linux_uses_home_and_dot_when_home_is_missing(self):
        with mock.patch.dict(os.environ, {"HOME": "/home/test"}, clear=True):
            with mock.patch.object(subject.os, "name", "posix"):
                self.assertEqual(
                    "/home/test/.local/share/openst133/firmware/st133-startup.bin",
                    subject.default_output(),
                )
        with mock.patch.dict(os.environ, {}, clear=True):
            with mock.patch.object(subject.os, "name", "posix"):
                self.assertEqual("./.local/share/openst133/firmware/st133-startup.bin", subject.default_output())

    def test_windows_uses_appdata(self):
        with mock.patch.dict(os.environ, {"APPDATA": "C:/Users/test/AppData/Roaming"}, clear=True):
            with mock.patch.object(subject.os, "name", "nt"):
                self.assertEqual(
                    "C:/Users/test/AppData/Roaming/openst133/firmware/st133-startup.bin",
                    subject.default_output(),
                )

    def test_windows_falls_back_to_current_directory_without_appdata(self):
        with mock.patch.dict(os.environ, {}, clear=True):
            with mock.patch.object(subject.os, "name", "nt"):
                self.assertEqual("./openst133/firmware/st133-startup.bin", subject.default_output())

    def test_empty_overrides_use_the_next_rule(self):
        with mock.patch.dict(os.environ, {"OPENST133_STARTUP_DATA": "", "OPENST133_DATA": ""}, clear=True):
            with mock.patch.object(subject.os, "name", "nt"):
                self.assertEqual("./openst133/firmware/st133-startup.bin", subject.default_output())


class StartupDataCommandTests(unittest.TestCase):
    def test_help_is_available_from_the_installed_command_parser(self):
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout), self.assertRaises(SystemExit) as raised:
            module_main(["startup-data", "--help"])
        self.assertEqual(0, raised.exception.code)
        self.assertIn("installed WinSpec folder", stdout.getvalue())
        self.assertIn("PI133B.DAT", stdout.getvalue())

    def test_command_passes_arguments_to_the_builder(self):
        with mock.patch("openst133.startup.run", return_value=0) as run:
            self.assertEqual(
                0,
                module_main([
                    "startup-data", "C:/WinSpec", "--detector", "98", "--output", "D:/private/startup.bin"
                ]),
            )
        arguments = run.call_args.args[0]
        self.assertEqual(Path("C:/WinSpec"), arguments.directory)
        self.assertEqual(98, arguments.detector)
        self.assertEqual("D:/private/startup.bin", arguments.output)

    def test_builder_errors_are_reported_without_a_traceback(self):
        stderr = io.StringIO()
        with mock.patch("openst133.startup.run", side_effect=subject.StartupDataError("unsupported input")):
            with contextlib.redirect_stderr(stderr):
                self.assertEqual(2, module_main(["startup-data", "C:/WinSpec"]))
        self.assertEqual("openst133 startup-data: error: unsupported input\n", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
