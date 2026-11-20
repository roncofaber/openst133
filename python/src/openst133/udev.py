# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT
"""Install the udev rule that lets the logged-in user open the ST-133 without root."""

import os
from pathlib import Path
import shutil
import subprocess
import sys

from ._library import bundled_file

RULE_NAME = "60-openst133.rules"
RULES_DIRECTORY = Path("/etc/udev/rules.d")
TRIGGER = ["udevadm", "trigger", "--subsystem-match=usb", "--attr-match=idVendor=0bd7", "--attr-match=idProduct=a010"]


def rule_source():
    packaged = bundled_file(RULE_NAME)
    if packaged is not None:
        return packaged
    checkout = Path(__file__).resolve().parents[3] / "packaging" / "udev" / RULE_NAME
    if checkout.is_file():
        return checkout
    raise FileNotFoundError(f"{RULE_NAME} is not bundled with this installation")


def install_commands(source, target):
    prefix = [] if os.geteuid() == 0 else ["sudo"]
    return [
        prefix + ["install", "-m", "0644", str(source), str(target)],
        prefix + ["udevadm", "control", "--reload-rules"],
        prefix + TRIGGER,
    ]


def add_arguments(parser):
    parser.add_argument("--print", dest="print_rule", action="store_true", help="print the rule instead of installing it")


def run(arguments):
    if not sys.platform.startswith("linux"):
        print(
            "openst133 setup-udev: udev rules apply only to Linux; on Windows bind the camera to the built-in "
            "WinUSB driver as described in docs/user/windows.md",
            file=sys.stderr,
        )
        return 2
    source = rule_source()
    if arguments.print_rule:
        sys.stdout.write(source.read_text(encoding="utf-8"))
        return 0
    target = RULES_DIRECTORY / RULE_NAME
    if target.is_file() and target.read_bytes() == source.read_bytes():
        print(f"{target} is already installed")
        return 0
    if os.geteuid() != 0 and shutil.which("sudo") is None:
        print(f"openst133 setup-udev: sudo not found; as root, copy {source} to {target} and reload udev", file=sys.stderr)
        return 2
    for command in install_commands(source, target):
        print("+", " ".join(command), flush=True)
        if subprocess.run(command, check=False).returncode != 0:
            print(f"openst133 setup-udev: '{' '.join(command)}' failed", file=sys.stderr)
            return 1
    print(f"installed {target}; if access is still denied, reconnect the camera")
    return 0
