# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT

"""Log simulated or real ST-133 temperature observations to CSV."""

import argparse
import csv
import datetime
import math
from pathlib import Path
import sys
import time

from .check import ensure_real_camera_allowed

MIN_SETPOINT_C = -152.0
MAX_SETPOINT_C = -25.0
CSV_FIELDS = ("timestamp_utc", "elapsed_s", "temperature_c", "setpoint_c", "lock_state")


class LivePlot:
    def __init__(self, setpoint, pyplot=None):
        if pyplot is None:
            import matplotlib.pyplot as pyplot
        self.pyplot = pyplot
        self.elapsed = []
        self.temperatures = []
        pyplot.ion()
        self.figure, self.axes = pyplot.subplots(figsize=(9, 5))
        self.figure.patch.set_facecolor("#fcfcfb")
        self.axes.set_facecolor("#fcfcfb")
        self.line, = self.axes.plot(
            [], [], color="#2a78d6", linewidth=2, solid_capstyle="round", label="Measured"
        )
        self.endpoint, = self.axes.plot(
            [], [], linestyle="none", marker="o", markersize=8,
            markerfacecolor="#2a78d6", markeredgecolor="#fcfcfb", markeredgewidth=2,
        )
        if setpoint is not None:
            self.axes.axhline(setpoint, color="#898781", linewidth=1.5, label="Requested setpoint")
            self.axes.legend(frameon=False)
        self.axes.set(title="ST-133 detector cooling", xlabel="Elapsed time (s)", ylabel="Temperature (C)")
        self.axes.grid(color="#e1e0d9", linewidth=0.8, linestyle="-")
        self.axes.set_axisbelow(True)
        for spine in self.axes.spines.values():
            spine.set_color("#c3c2b7")
        self.figure.tight_layout()
        self.figure.show()

    def update(self, elapsed, temperature):
        self.elapsed.append(elapsed)
        self.temperatures.append(temperature)
        self.line.set_data(self.elapsed, self.temperatures)
        self.endpoint.set_data([elapsed], [temperature])
        self.axes.relim()
        self.axes.autoscale_view()
        self.figure.canvas.draw_idle()
        self.figure.canvas.flush_events()

    def close(self):
        self.pyplot.close(self.figure)


def lock_state(temperature, setpoint, tolerance, within_since, now, hold):
    if setpoint is None:
        return "unknown", None
    if abs(temperature - setpoint) > tolerance:
        return "unlocked", None
    if within_since is None:
        within_since = now
    state = "locked" if now - within_since >= hold else "unlocked"
    return state, within_since


def observe_temperature(camera):
    temperature = float(camera.temperature())
    if not math.isfinite(temperature):
        raise RuntimeError("camera returned a non-finite temperature")
    return temperature


def log_cooling(
    camera, output, *, interval=10.0, setpoint=None, count=None,
    lock_tolerance=2.0, lock_hold=60.0, plot=False,
    wait_ready=False, settle_time=0.0,
    clock=time.monotonic, sleep=time.sleep, utc_now=None, pyplot=None,
):
    if wait_ready and setpoint is None:
        raise ValueError("acquisition readiness requires a setpoint")
    utc_now = utc_now or (lambda: datetime.datetime.now(datetime.timezone.utc))
    plotter = LivePlot(setpoint, pyplot) if plot else None
    samples = 0
    ready = False
    try:
        with Path(output).open("x", encoding="utf-8", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=CSV_FIELDS)
            writer.writeheader()
            stream.flush()
            try:
                camera.open()
                if setpoint is not None:
                    camera.set_temperature(setpoint)
                started = clock()
                deadline = started
                within_since = None
                while count is None or samples < count:
                    now = clock()
                    temperature = observe_temperature(camera)
                    state, within_since = lock_state(
                        temperature, setpoint, lock_tolerance, within_since, now, lock_hold
                    )
                    elapsed = max(0.0, now - started)
                    stamp = utc_now().astimezone(datetime.timezone.utc).isoformat(
                        timespec="milliseconds"
                    ).replace("+00:00", "Z")
                    writer.writerow({
                        "timestamp_utc": stamp,
                        "elapsed_s": f"{elapsed:.3f}",
                        "temperature_c": f"{temperature:.3f}",
                        "setpoint_c": "" if setpoint is None else f"{setpoint:.3f}",
                        "lock_state": state,
                    })
                    stream.flush()
                    samples += 1
                    print(f"{stamp}  {temperature:.3f} C  {state}", flush=True)
                    if plotter is not None:
                        plotter.update(elapsed, temperature)
                    if wait_ready and state == "locked":
                        stable = max(0.0, now - within_since)
                        if stable >= lock_hold + settle_time:
                            print(
                                "acquisition ready under software criteria: "
                                f"target {setpoint:.3f} C; measured {temperature:.3f} C; "
                                f"tolerance {lock_tolerance:.3f} C; stable {stable:.3f} s; "
                                f"extra settling {settle_time:.3f} s",
                                flush=True,
                            )
                            ready = True
                            break
                    if count is not None and samples >= count:
                        break
                    deadline += interval
                    sleep(max(0.0, deadline - clock()))
            finally:
                camera.close()
    finally:
        if plotter is not None:
            plotter.close()
    if wait_ready and not ready:
        raise RuntimeError(f"acquisition readiness not reached after {samples} samples")
    return samples


def positive(value, label):
    number = float(value)
    if not math.isfinite(number) or number <= 0.0:
        raise argparse.ArgumentTypeError(f"{label} must be a finite positive number")
    return number


def nonnegative(value, label):
    number = float(value)
    if not math.isfinite(number) or number < 0.0:
        raise argparse.ArgumentTypeError(f"{label} must be a finite non-negative number")
    return number


def setpoint(value):
    number = float(value)
    if not math.isfinite(number) or not MIN_SETPOINT_C <= number <= MAX_SETPOINT_C:
        raise argparse.ArgumentTypeError(
            f"setpoint must be within the accepted encoding range of {MIN_SETPOINT_C:g} to {MAX_SETPOINT_C:g} C"
        )
    return number


def sample_count(value):
    number = int(value)
    if number < 1:
        raise argparse.ArgumentTypeError("count must be at least 1")
    return number


def parse_arguments(arguments=None):
    parser = argparse.ArgumentParser(prog="openst133 temperature-log", description=__doc__)
    parser.add_argument("--output", type=Path, required=True, help="new CSV path; an existing path is never overwritten")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--simulate", action="store_true", help="use the simulated cooling curve instead of camera hardware")
    mode.add_argument("--hardware", action="store_true", help="use the connected ST-133")
    parser.add_argument("--setpoint", type=setpoint, help=f"requested target in C, from {MIN_SETPOINT_C:g} to {MAX_SETPOINT_C:g}")
    parser.add_argument("--interval", type=lambda value: positive(value, "interval"), default=10.0, help="seconds between scheduled reads (default: 10)")
    parser.add_argument("--count", type=sample_count, help="stop after this many samples; default is until Ctrl-C")
    parser.add_argument("--lock-tolerance", type=lambda value: nonnegative(value, "lock tolerance"), default=2.0, help="software lock tolerance in C (default: 2)")
    parser.add_argument("--lock-hold", type=lambda value: nonnegative(value, "lock hold"), default=60.0, help="continuous seconds within tolerance before software lock (default: 60)")
    parser.add_argument("--wait-ready", action="store_true", help="stop after software lock plus extra settling; requires --setpoint")
    parser.add_argument("--settle-time", type=lambda value: nonnegative(value, "settle time"), default=0.0, help="extra seconds within tolerance after software lock before readiness (default: 0)")
    parser.add_argument("--plot", action="store_true", help="show a live temperature plot; requires matplotlib and a display")
    return parser.parse_args(arguments)


def main(arguments=None):
    arguments = parse_arguments(arguments)
    try:
        if arguments.simulate:
            from openst133.camera import SimulatedCamera
            camera = SimulatedCamera(cooling_time_constant=600.0)
        else:
            ensure_real_camera_allowed(arguments.hardware)
            from openst133.camera import RealCamera
            camera = RealCamera()
        samples = log_cooling(
            camera,
            arguments.output,
            interval=arguments.interval,
            setpoint=arguments.setpoint,
            count=arguments.count,
            lock_tolerance=arguments.lock_tolerance,
            lock_hold=arguments.lock_hold,
            wait_ready=arguments.wait_ready,
            settle_time=arguments.settle_time,
            plot=arguments.plot,
        )
    except KeyboardInterrupt:
        print("cooling monitor stopped by user", file=sys.stderr)
        return 130
    except Exception as error:
        print(f"cooling monitor failed: {error}", file=sys.stderr)
        return 2
    print(f"wrote {samples} samples to {arguments.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
