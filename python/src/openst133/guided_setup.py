# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT
"""Guided first-time setup: camera discovery, USB access and private startup data.

Startup data is built from the WinSpec files PI133B.DAT and PICHIPDB.dll. Obtaining them from a licensed WinSpec is up
to the user; any copy works, from an installed WinSpec folder or otherwise.
"""

import hashlib
import os
from pathlib import Path
import sys
from types import SimpleNamespace

REQUIRED_FILES = ("PI133B.DAT", "PICHIPDB.dll")
WINDOWS_FOLDERS = (
    "PI Acton\\WinSpec",
    "Princeton Instruments\\WinSpec",
    "Roper Scientific\\WinSpec",
    "WinSpec32",
    "WinSpec",
)
WINUSB_STEPS = (
    "In Device Manager, right-click the ST-133 entry and choose Update driver > Browse my computer for drivers > "
    "Let me pick from a list > Universal Serial Bus devices > WinUsb Device, then confirm the warning "
    "(docs/user/windows.md, step 5)."
)


def add_arguments(parser):
    parser.add_argument("winspec", nargs="?", help="folder containing PI133B.DAT and PICHIPDB.dll")
    parser.add_argument("--dat", help="path to PI133B.DAT from your licensed WinSpec")
    parser.add_argument("--dll", help="path to PICHIPDB.dll from your licensed WinSpec")
    parser.add_argument("--skip-udev", action="store_true", help="Linux: do not offer to install the udev rule")


def has_winspec_files(folder):
    return all((Path(folder) / name).is_file() for name in REQUIRED_FILES)


def find_winspec():
    if os.name != "nt":
        return None
    roots = [os.environ.get(name) for name in ("ProgramFiles(x86)", "ProgramFiles")]
    candidates = [Path(root) / folder for root in roots if root for folder in WINDOWS_FOLDERS]
    candidates += [Path("C:\\WinSpec"), Path("C:\\WinSpec32")]
    return next((path for path in candidates if has_winspec_files(path)), None)


def choose_sources(arguments):
    """The PI133B.DAT and PICHIPDB.dll paths from explicit files, a folder, a known install folder or prompts."""
    folder = arguments.winspec or (None if arguments.dat or arguments.dll else find_winspec())
    dat = arguments.dat or (str(Path(folder) / "PI133B.DAT") if folder else None)
    dll = arguments.dll or (str(Path(folder) / "PICHIPDB.dll") if folder else None)
    if dat is None:
        dat = ask("Path to PI133B.DAT from your licensed WinSpec (or a folder containing both files): ") or None
        if dat is not None and Path(dat).is_dir():
            dat, dll = str(Path(dat) / "PI133B.DAT"), dll or str(Path(dat) / "PICHIPDB.dll")
    if dll is None and dat is not None:
        dll = ask("Path to PICHIPDB.dll from your licensed WinSpec: ") or None
    return dat, dll


def startup_data_status(path):
    path = Path(path)
    if not path.is_file():
        return "missing"
    sidecar = path.with_name(path.name + ".sha256")
    if not sidecar.is_file():
        return "unverified"
    expected = sidecar.read_text(encoding="ascii", errors="replace").split()[:1]
    return "ok" if expected == [hashlib.sha256(path.read_bytes()).hexdigest()] else "mismatch"


def probe_access(device):
    """Open and close the USB interface without sending a camera command; None means access works."""
    from .device import check_access

    try:
        check_access(device)
    except OSError as error:
        return str(error)
    return None


def ask(prompt):
    if not sys.stdin.isatty():
        return ""
    try:
        return input(prompt).strip().strip('"')
    except EOFError:
        return ""


def step(title):
    print(f"\n== {title}")


def run(arguments):
    from .device import list_devices
    from .startup import StartupDataError, default_output
    from .startup import run as generate

    ready = True

    step("1. Camera")
    try:
        devices = list_devices()
    except OSError as error:
        print(f"cannot use the openst133 library: {error}")
        return 2
    for device in devices:
        print(f"found ST-133 interface at bus {device.bus_number}, address {device.device_address}, port {device.port_path or '-'}")
    if not devices:
        ready = False
        print("no ST-133 (0bd7:a010) is visible. Check controller power and the USB cable, close WinSpec and any VM "
              "holding the camera, and complete docs/user/safety.md before powering it on. Setup continues without it.")

    step("2. USB access")
    if sys.platform.startswith("linux") and not arguments.skip_udev:
        from . import udev

        target = udev.RULES_DIRECTORY / udev.RULE_NAME
        if target.is_file() and target.read_bytes() == udev.rule_source().read_bytes():
            print(f"udev rule installed: {target}")
        elif udev.run(SimpleNamespace(print_rule=False)) != 0:
            ready = False
    probed = devices
    if devices and os.environ.get("OPENST133_NO_HARDWARE") == "1":
        print("OPENST133_NO_HARDWARE=1 is set, so the access check is skipped")
        probed = []
    for device in probed:
        problem = probe_access(device)
        if problem is None:
            print(f"opened and closed port {device.port_path or '-'} without sending camera commands: access works")
            continue
        ready = False
        print(f"cannot open port {device.port_path or '-'}: {problem}")
        if os.name == "nt":
            print(WINUSB_STEPS)
        elif "permission" in problem:
            print("reconnect the camera so the udev rule applies, then run 'openst133 setup' again")

    step("3. Startup data")
    output = Path(default_output())
    status = startup_data_status(output)
    if status == "ok":
        print(f"startup data present and matching its sidecar: {output}")
    else:
        if status != "missing":
            print(f"existing startup data at {output} is {status}; it will be rebuilt")
        dat, dll = choose_sources(arguments)
        missing = [str(path) for path in (dat, dll) if path is not None and not Path(path).is_file()]
        if dat is None or dll is None:
            ready = False
            print("startup data is missing. Provide the two files from your licensed WinSpec:\n"
                  "  openst133 setup --dat /path/to/PI133B.DAT --dll /path/to/PICHIPDB.dll\n"
                  "or a folder containing both: openst133 setup /path/to/folder (docs/user/startup-data.md)")
        elif missing:
            ready = False
            print(f"file not found: {', '.join(missing)}")
        else:
            print(f"building startup data from {dat} and {dll}")
            try:
                generate(SimpleNamespace(directory=None, dat=Path(dat), dll=Path(dll), detector=98, output=None))
            except StartupDataError as error:
                ready = False
                print(f"startup data was not built: {error}")

    step("Result")
    if ready and devices:
        print("ready. Acquire a first frame with:\n  ost acquire --exposure 0.1 --out first-frame.spe")
        return 0
    print("not ready yet; fix the items above and run 'openst133 setup' again")
    return 1
