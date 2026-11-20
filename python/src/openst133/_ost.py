# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT

"""Run the native ost command bundled in the wheel."""

import os
import pathlib
import subprocess
import sys

from ._library import bundled_file


def executable():
    name = "ost.exe" if os.name == "nt" else "ost"
    return bundled_file(name) or pathlib.Path(__file__).resolve().with_name(name)


def main(argv=None):
    arguments = sys.argv[1:] if argv is None else list(argv)
    binary = executable()
    if not binary.is_file():
        print(f"ost: the native ost command is not bundled with this installation ({binary})", file=sys.stderr)
        return 1
    if os.name != "nt":
        sys.stdout.flush()
        sys.stderr.flush()
        os.execv(binary, [os.fspath(binary), *arguments])
    environment = os.environ.copy()
    # delvewheel vendors the executable's DLL dependencies here, which a child process does not search by itself.
    libraries = binary.parent.parent / "openst133.libs"
    if libraries.is_dir():
        environment["PATH"] = os.pathsep.join((os.fspath(libraries), environment.get("PATH", "")))
    try:
        return subprocess.run([os.fspath(binary), *arguments], env=environment).returncode
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
