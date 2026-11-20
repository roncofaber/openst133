# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT

import pathlib
import tempfile
import unittest
from unittest import mock

from openst133 import _library, _ost, udev


class BundledFileTest(unittest.TestCase):
    def test_native_files_are_located_through_package_resources(self):
        with tempfile.TemporaryDirectory() as installed:
            built = pathlib.Path(installed)
            for name in ("libopenst133.so", "ost", udev.RULE_NAME):
                (built / name).write_bytes(b"")
            with mock.patch("importlib.resources.files", return_value=built) as files:
                self.assertEqual(_library.bundled_file("libopenst133.so"), built.resolve() / "libopenst133.so")
                self.assertIsNone(_library.bundled_file("missing.so"))
                with mock.patch("os.name", "posix"):
                    self.assertEqual(_ost.executable(), built.resolve() / "ost")
                self.assertEqual(udev.rule_source(), built.resolve() / udev.RULE_NAME)
            files.assert_called_with("openst133")


if __name__ == "__main__":
    unittest.main()
