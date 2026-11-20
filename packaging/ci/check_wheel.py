# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT

"""Smoke-test an installed openst133 wheel without using source-tree modules."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile

import openst133
from openst133._library import load


def run(command, environment):
    print("+", subprocess.list2cmdline([os.fspath(value) for value in command]), flush=True)
    result = subprocess.run(command, check=True, env=environment, stdout=subprocess.PIPE, text=True)
    print(result.stdout, end="", flush=True)
    return result.stdout


def require(text, fragments, command):
    missing = [fragment for fragment in fragments if fragment not in text]
    if missing:
        raise RuntimeError(f"'{command}' output lacks {missing}")


def main():
    package = Path(openst133.__file__).resolve().parent
    names = ("libopenst133.so", "libopenst133.dylib", "openst133.dll")
    bundled = [path for path in (package / name for name in names) if path.is_file()]
    if len(bundled) != 1:
        raise RuntimeError(f"expected one bundled native library in {package}, found {bundled}")
    native_ost = package / ("ost.exe" if os.name == "nt" else "ost")
    if not native_ost.is_file():
        raise RuntimeError(f"native ost command is not bundled: {native_ost}")
    load()
    print(f"loaded bundled native library: {bundled[0]}", flush=True)

    environment = os.environ.copy()
    environment.pop("OPENST133_LIBRARY", None)
    suffix = ".exe" if os.name == "nt" else ""
    ost = Path(sys.executable).with_name("ost" + suffix)
    tools = Path(sys.executable).with_name("openst133" + suffix)
    require(run((ost, "--help"), environment), ("temperature", "acquire", "spe-info"), "ost --help")
    require(run((ost, "acquire", "--help"), environment), ("--stream", "--fvb"), "ost acquire --help")
    require(run((tools, "--help"), environment), ("view", "acquire"), "openst133 --help")
    require(run((tools, "view", "--help"), environment), ("--simulate",), "openst133 view --help")
    with tempfile.TemporaryDirectory(prefix="openst133-wheel-") as directory:
        output = Path(directory) / "first-frame.spe"
        run(
            (
                tools,
                "acquire",
                "--simulate",
                "--exposure",
                "0.001",
                "--frames",
                "1",
                "--out",
                output,
            ),
            environment,
        )
        if output.stat().st_size <= 4100:
            raise RuntimeError(f"simulated SPE output is missing or empty: {output}")
        print(f"simulated acquisition wrote {output.stat().st_size} bytes", flush=True)


if __name__ == "__main__":
    main()
