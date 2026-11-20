# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT

import argparse
import logging
import math
import sys


LOG_LEVELS = ("DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL")
FORWARDED = {
    "check": "run the driver check and dark characterization ('openst133 check --help')",
    "temperature-log": "log detector temperature to CSV, optionally requesting a target ('openst133 temperature-log --help')",
}
CAMERA_HELP = "camera to use: index from 'list' (from 0), BUS-PORT[.PORT] path or BUS:ADDR; required with several cameras"


def add_logging_options(parser):
    parser.add_argument("--log-level", choices=LOG_LEVELS, default="WARNING", help="logging threshold (default: WARNING)")
    parser.add_argument("--log-file", help="append log records to this plain file instead of stderr")


def configure_logging(arguments):
    handler = logging.FileHandler(arguments.log_file, encoding="utf-8") if arguments.log_file else logging.StreamHandler()
    logging.basicConfig(
        level=getattr(logging, arguments.log_level),
        handlers=[handler],
        format="%(asctime)s %(name)s %(levelname)s %(message)s",
        force=True,
    )


def build_parser():
    parser = argparse.ArgumentParser(
        prog="openst133",
        description="openst133 tools: discovery, USB setup, startup-data generation, the viewer, acquisition, the hardware check and temperature logging. The low-level camera command line is the native 'ost'.",
    )
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser(
        "list", help="list ST-133 USB interfaces as bus:address, VID:PID and port path, in --camera index order"
    )
    view = commands.add_parser("view", help="open the camera viewer")
    view.add_argument("--simulate", action="store_true", help="use the simulated camera")
    view.add_argument("--close-after", type=float, default=None, help="close the window after N seconds")
    view.add_argument("--camera", default=None, help=CAMERA_HELP)
    add_logging_options(view)
    acquire = commands.add_parser("acquire", help="acquire frames and save them as SPE")
    acquire.add_argument("--simulate", action="store_true", help="use the simulated camera")
    acquire.add_argument("--exposure", type=float, default=0.1, help="exposure in seconds")
    acquire.add_argument("--frames", type=int, default=1, help="number of frames")
    acquire.add_argument("--out", required=True, help="output file: .spe, .h5, .tif or .csv (spectra), chosen by extension")
    acquire.add_argument("--camera", default=None, help=CAMERA_HELP)
    add_logging_options(acquire)
    startup_data = commands.add_parser(
        "startup-data",
        help="build private startup data from an installed WinSpec folder",
        description="Build private ST-133 startup data from the user's installed WinSpec folder.",
    )
    from .startup import add_arguments as add_startup_data_arguments

    add_startup_data_arguments(startup_data)
    setup = commands.add_parser(
        "setup",
        help="guided first-time setup: find the camera, allow USB access and build startup data from WinSpec",
        description="Guided first-time setup: find the camera, allow USB access and build the private startup data "
        "from an installed WinSpec folder. Sends no camera commands; rerun it until it reports ready.",
    )
    from .guided_setup import add_arguments as add_setup_arguments

    add_setup_arguments(setup)
    setup_udev = commands.add_parser(
        "setup-udev",
        help="Linux: install the udev rule for non-root camera access (asks for sudo once)",
        description="Install the udev rule that lets the logged-in user open the ST-133 without root.",
    )
    from .udev import add_arguments as add_udev_arguments

    add_udev_arguments(setup_udev)
    for name, text in FORWARDED.items():
        commands.add_parser(name, help=text, add_help=False)
    return parser


def main(argv=None):
    argv = sys.argv[1:] if argv is None else list(argv)
    if argv and argv[0] in FORWARDED:
        if argv[0] == "check":
            from .check import main as forwarded
        else:
            from .temperature_log import main as forwarded
        return forwarded(argv[1:])
    parser = build_parser()
    arguments = parser.parse_args(argv)
    if arguments.command == "setup":
        from .guided_setup import run as run_setup

        return run_setup(arguments)
    if arguments.command == "setup-udev":
        from .udev import run as run_udev

        return run_udev(arguments)
    if arguments.command == "list":
        from .device import list_devices

        try:
            devices = list_devices()
        except OSError as error:
            print(f"openst133 list: {error}", file=sys.stderr)
            return 2
        for device in devices:
            print(
                f"{device.bus_number:03d}:{device.device_address:03d} "
                f"{device.vendor_id:04x}:{device.product_id:04x} {device.port_path or '-'}"
            )
        return 0
    if arguments.command == "startup-data":
        from .startup import StartupDataError, run

        try:
            return run(arguments)
        except StartupDataError as error:
            print(f"openst133 startup-data: error: {error}", file=sys.stderr)
            return 2
    if arguments.simulate and arguments.camera is not None:
        parser.error("--camera selects a real camera and cannot be combined with --simulate")
    if arguments.command == "view":
        configure_logging(arguments)
        from .viewer import run_view

        return run_view(arguments.simulate, arguments.close_after, arguments.camera)
    if arguments.frames < 1 or not (arguments.exposure > 0 and math.isfinite(arguments.exposure)):
        parser.error("--exposure must be a finite positive number and --frames at least 1")
    configure_logging(arguments)
    from .viewer import run_acquire

    return run_acquire(arguments.simulate, arguments.exposure, arguments.out, arguments.frames, arguments.camera)


if __name__ == "__main__":
    raise SystemExit(main())
