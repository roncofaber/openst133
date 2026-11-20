# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT

"""User-initiated ST-133 driver smoke check and camera characterization."""

import argparse
import dataclasses
import datetime
import json
import logging
import math
import shlex
import subprocess
import sys
import time
from pathlib import Path

import numpy as np

from .characterization import characterize, synthetic_frames


WIDTH = 1340
HEIGHT = 100


def inspect_frames(frames):
    values = np.asarray(frames)
    if values.ndim == 2:
        values = values[np.newaxis, :, :]
    result = {
        "passed": True,
        "frame_shape": list(values.shape),
        "dtype": str(values.dtype),
        "all_zero_frames": [],
        "saturated_frames": [],
        "stuck_rows": [],
        "stuck_columns": [],
    }
    valid_structure = values.ndim == 3 and values.shape[1:] == (HEIGHT, WIDTH) and values.dtype == np.uint16
    if not valid_structure:
        result["passed"] = False
        return result
    for index, frame in enumerate(values):
        if not np.any(frame):
            result["all_zero_frames"].append(index)
        if np.any(frame == np.iinfo(np.uint16).max):
            result["saturated_frames"].append(index)
        if np.ptp(frame) == 0:
            continue
        result["stuck_rows"].extend(
            [index, int(row)] for row in np.flatnonzero(np.ptp(frame, axis=1) == 0)
        )
        result["stuck_columns"].extend(
            [index, int(column)] for column in np.flatnonzero(np.ptp(frame, axis=0) == 0)
        )
    result["passed"] = not any(
        result[name]
        for name in ("all_zero_frames", "saturated_frames", "stuck_rows", "stuck_columns")
    )
    return result


def qemu_conflicts(listing):
    conflicts = []
    for line in listing.splitlines():
        try:
            fields = shlex.split(line)
        except ValueError:
            continue
        if len(fields) < 2 or not fields[0].isdigit():
            continue
        executable = fields[1].rsplit("/", 1)[-1]
        arguments = " ".join(fields[2:]).lower()
        if not executable.startswith("qemu-system"):
            continue
        camera_ids = "0bd7" in arguments and "a010" in arguments
        direct_usb = "/dev/bus/usb/" in arguments
        if camera_ids or direct_usb:
            conflicts.append(fields[0])
    return conflicts


def ensure_real_camera_allowed(acknowledged):
    if not acknowledged:
        raise RuntimeError("choose --hardware to use the connected camera or --simulate for a synthetic run")
    listing = subprocess.run(
        ["ps", "-eo", "pid=,args="], check=True, text=True, capture_output=True
    ).stdout
    conflicts = qemu_conflicts(listing)
    if conflicts:
        raise RuntimeError(
            "QEMU appears to hold the camera or a direct USB device in process(es) "
            + ", ".join(conflicts)
            + "; stop that VM before running this check"
        )


def _set_exposure(camera, seconds):
    camera.set_exposure(seconds)


def _acquire(camera, exposure, shutter):
    _set_exposure(camera, exposure)
    if hasattr(camera, "set_shutter"):
        camera.set_shutter(shutter)
    frame = camera.acquire()
    return np.asarray(frame)


def _acquire_timed(camera, exposure, shutter):
    _set_exposure(camera, exposure)
    camera.set_shutter(shutter)
    started = time.monotonic()
    frame = np.asarray(camera.acquire())
    return frame, time.monotonic() - started


def _frame_summary(frame, shape=(HEIGHT, WIDTH), dtype=np.uint16, accumulations=None):
    values = np.asarray(frame)
    expected_dtype = np.dtype(dtype)
    result = {
        "shape": list(values.shape),
        "expected_shape": list(shape),
        "dtype": str(values.dtype),
        "expected_dtype": str(expected_dtype),
        "pixel_count": int(values.size),
    }
    if values.size:
        result.update({
            "minimum_dn": int(np.min(values)),
            "maximum_dn": int(np.max(values)),
            "mean_dn": float(np.mean(values)),
            "standard_deviation_dn": float(np.std(values)),
            "zero_pixels": int(np.count_nonzero(values == 0)),
        })
        if accumulations is None:
            if values.dtype == np.dtype(np.uint16):
                result["saturated_uint16_pixels"] = int(
                    np.count_nonzero(values == np.iinfo(np.uint16).max)
                )
        else:
            result["summed_output_maximum_dn"] = result["maximum_dn"]
            result["theoretical_maximum_dn"] = accumulations * np.iinfo(np.uint16).max
    result["passed"] = bool(
        values.shape == tuple(shape)
        and values.dtype == expected_dtype
        and values.size
        and np.all(np.isfinite(values))
    )
    return result


def _full_frame(camera):
    camera.set_accumulations(1)
    camera.set_binning(1, 1)
    camera.set_roi(0, 0, camera.width, camera.height)


def _geometry_metadata(camera):
    metadata = camera.spe_metadata()
    return {"roi": list(metadata["roi"]), "binning": list(metadata["binning"])}


def _gain_checks(camera, exposure):
    _full_frame(camera)
    frames = []
    for gain in (1, 2, 3):
        camera.set_gain(gain)
        frame, duration = _acquire_timed(camera, exposure, "closed")
        summary = _frame_summary(frame)
        frames.append({
            "passed": bool(summary["passed"] and camera.gain == gain),
            "selected_gain": gain,
            "duration_seconds": duration,
            "frame": summary,
        })
    return {"passed": all(item["passed"] for item in frames), "exposure_seconds": exposure, "frames": frames}


def _shutter_checks(camera, exposure):
    _full_frame(camera)
    camera.set_gain(2)
    frames = {}
    for mode in ("closed", "opened"):
        frame, duration = _acquire_timed(camera, exposure, mode)
        summary = _frame_summary(frame)
        frames[mode] = {
            "passed": bool(summary["passed"] and camera.shutter == mode),
            "duration_seconds": duration,
            "frame": summary,
        }
    return {
        "passed": all(item["passed"] for item in frames.values()),
        "exposure_seconds": exposure,
        "frames": frames,
        "opened_minus_closed_mean_dn": frames["opened"]["frame"]["mean_dn"] - frames["closed"]["frame"]["mean_dn"],
    }


def _geometry_check(camera, exposure, *, fvb):
    _full_frame(camera)
    camera.set_gain(2)
    if fvb:
        camera.set_full_vertical_binning()
        shape = (1, camera.width)
    else:
        camera.set_roi(0, 40, 400, 20)
        camera.set_binning(4, 1)
        shape = (20, 100)
    frame, duration = _acquire_timed(camera, exposure, "closed")
    summary = _frame_summary(frame, shape)
    return {
        "passed": summary["passed"],
        "exposure_seconds": exposure,
        "duration_seconds": duration,
        **_geometry_metadata(camera),
        "frame": summary,
    }


def _stream_check(camera, exposure, frame_count):
    _full_frame(camera)
    camera.set_gain(2)
    camera.set_shutter("closed")
    camera.set_exposure(exposure)
    camera.set_full_vertical_binning()
    started = time.monotonic()
    frames = [np.asarray(frame) for frame in camera.stream(frames=frame_count)]
    duration = time.monotonic() - started
    shape_matches = all(frame.shape == (1, camera.width) for frame in frames)
    dtypes_match = all(frame.dtype == np.dtype(np.uint16) for frame in frames)
    stats = getattr(camera, "stream_stats", None)
    counters = dataclasses.asdict(stats) if stats is not None else None
    consumer_dropped = int(getattr(camera, "stream_dropped", 0))
    partial_bytes = 0 if counters is None else counters["partial_bytes_discarded"]
    return {
        "passed": bool(
            len(frames) == frame_count
            and shape_matches
            and dtypes_match
            and duration > 0.0
            and consumer_dropped == 0
            and partial_bytes == 0
        ),
        "requested_frames": frame_count,
        "received_frames": len(frames),
        "expected_shape": [1, camera.width],
        "duration_seconds": duration,
        "frames_per_second": len(frames) / duration if duration > 0.0 else 0.0,
        "consumer_dropped_frames": consumer_dropped,
        "native_counters": counters,
        "exposure_seconds": exposure,
        "criterion": "all requested FVB frames arrive with the expected shape and uint16 type, without consumer drops or discarded partial bytes",
    }


def _accumulation_check(camera, exposure, count):
    _full_frame(camera)
    camera.set_gain(2)
    single, single_duration = _acquire_timed(camera, exposure, "closed")
    single_summary = _frame_summary(single)
    camera.set_accumulations(count)
    accumulated, accumulated_duration = _acquire_timed(camera, exposure, "closed")
    accumulated_summary = _frame_summary(accumulated, dtype=np.int32, accumulations=count)
    ratio = accumulated_summary["mean_dn"] / single_summary["mean_dn"] if single_summary["mean_dn"] else None
    passed = bool(
        single_summary["passed"]
        and accumulated_summary["passed"]
        and camera.accumulations == count
        and ratio is not None
        and 2.0 <= ratio <= 4.0
        and accumulated_summary["maximum_dn"] <= accumulated_summary["theoretical_maximum_dn"]
    )
    camera.set_accumulations(1)
    return {
        "passed": passed,
        "count": count,
        "exposure_seconds": exposure,
        "single": {"duration_seconds": single_duration, "frame": single_summary},
        "accumulated": {"duration_seconds": accumulated_duration, "frame": accumulated_summary},
        "accumulated_to_single_mean_ratio": ratio,
        "criterion": "3-accumulation int32 frame has the full-frame shape and a mean between 2 and 4 times the single-frame mean",
    }


def _setpoint_persistence_check(camera, setpoint, simulated):
    source_before = None if simulated else int(camera.target_source)
    temperature_before = float(camera.temperature())
    camera.set_temperature(setpoint)
    camera.close()
    camera.open()
    source_after = None if simulated else int(camera.target_source)
    temperature_after = float(camera.temperature())
    if simulated:
        source_name = None
        # SimulatedCamera has no public target getter, so the fixture checks its retained target directly.
        target_after = float(camera._target_temperature)
        persisted = math.isclose(target_after, setpoint, rel_tol=0.0, abs_tol=0.01)
    else:
        from openst133.device import TARGET_SOURCE_PERSISTED

        target_after = float(camera.identity.target_celsius)
        persisted = source_after == TARGET_SOURCE_PERSISTED and math.isclose(
            target_after, setpoint, rel_tol=0.0, abs_tol=0.01
        )
        source_name = "persisted" if persisted else f"unexpected ({source_after})"
    return {
        "passed": bool(persisted and math.isfinite(temperature_before) and math.isfinite(temperature_after)),
        "requested_setpoint_celsius": setpoint,
        "setpoint_after_reopen_celsius": target_after,
        "temperature_before_celsius": temperature_before,
        "temperature_after_reopen_celsius": temperature_after,
        "target_source_before": source_before,
        "target_source_after": source_after,
        "target_source_after_name": source_name,
        "verification": "retained setpoint matched after close and reopen" if simulated else "reopen loaded the per-camera persisted setpoint value",
    }


def _settings_checks(camera, exposure, setpoint, simulated):
    checks = {
        "gain": _gain_checks(camera, exposure),
        "shutter": _shutter_checks(camera, exposure),
        "fvb": _geometry_check(camera, exposure, fvb=True),
        "binned_roi": _geometry_check(camera, exposure, fvb=False),
        "fvb_stream": _stream_check(camera, 0.001, 20),
        "accumulations": _accumulation_check(camera, exposure, 3),
    }
    _full_frame(camera)
    camera.set_gain(2)
    checks["setpoint_persistence"] = _setpoint_persistence_check(camera, setpoint, simulated)
    return {
        "passed": all(check["passed"] for check in checks.values()),
        "backend": "simulated" if simulated else "hardware",
        "checks": checks,
    }


def _pair_noise(reference):
    reference = np.asarray(reference, dtype=np.float64)
    if len(reference) != 2:
        raise ValueError("two reference frames are needed to estimate noise")
    return reference, float(np.std(reference[0] - reference[1]) / math.sqrt(2.0))


def _profile_ratio(difference, noise_per_pixel):
    rows, columns = difference.shape
    column_ratio = float(np.std(difference.mean(axis=0)) / (noise_per_pixel / math.sqrt(rows)))
    row_ratio = float(np.std(difference.mean(axis=1)) / (noise_per_pixel / math.sqrt(columns)))
    return column_ratio, row_ratio


# Frame means drift with detector temperature (strongly on an uncooled CCD), so a uniform level offset is reported,
# not judged; a driver fault (wrong bytes, shifted or swapped pixels) shows up as pixel-level structure instead.
def _repeat_consistency(reference, independent):
    reference, noise = _pair_noise(reference)
    _, independent_noise = _pair_noise(np.asarray(independent, dtype=np.float64)[-2:])
    difference = reference[0] - reference[1]
    difference = difference - difference.mean()
    column_ratio, row_ratio = _profile_ratio(difference, noise * math.sqrt(2.0))
    noise_ratio = noise / independent_noise
    return {
        "passed": bool(0.8 <= noise_ratio <= 1.25 and column_ratio <= 3.0 and row_ratio <= 3.0),
        "noise_dn": noise,
        "independent_noise_dn": independent_noise,
        "noise_ratio": noise_ratio,
        "level_offset_dn": float(np.mean(reference[0]) - np.mean(reference[1])),
        "column_profile_ratio": column_ratio,
        "row_profile_ratio": row_ratio,
        "criterion": "pair noise within 0.8-1.25x the noise of an independent pair (the series), and row and column profiles of "
                     "the level-corrected pair difference within 3x their noise expectation",
    }


def _pixel_consistency(reference, candidates):
    reference, noise = _pair_noise(reference)
    template = reference.mean(axis=0)
    expected = noise * math.sqrt(1.0 + 1.0 / len(reference))
    frames = []
    for candidate in np.asarray(candidates, dtype=np.float64):
        difference = candidate - template
        offset = float(difference.mean())
        difference = difference - offset
        column_ratio, row_ratio = _profile_ratio(difference, expected)
        frames.append({
            "level_offset_dn": offset,
            "level_offset_fraction": offset / float(template.mean()),
            "residual_noise_ratio": float(np.std(difference) / expected),
            "column_profile_ratio": column_ratio,
            "row_profile_ratio": row_ratio,
        })
    passed = all(0.8 <= f["residual_noise_ratio"] <= 1.25 and f["column_profile_ratio"] <= 3.0 and f["row_profile_ratio"] <= 3.0
                 for f in frames)
    return {
        "passed": bool(passed),
        "noise_dn": noise,
        "frames": frames,
        "criterion": "after removing each frame's level offset (temperature drift, reported only), residual noise within 0.8-1.25x "
                     "the pair-noise expectation and row/column profiles within 3x their noise expectation",
    }


def _spe_checks(paths, frames, exposure):
    from openst133 import spe

    checks = {}
    for name, path in paths.items():
        spe.create(path, frames[name], exposure, data_type="uint16")
        info, decoded = spe.read(path)
        expected_count = len(frames[name])
        fields = {
            "major_version_3": info.major_version == 3,
            "uint16_data_type": info.data_type == spe.SPE_UINT16 and info.pixel_size == 2,
            "shape_1340x100": (info.width, info.height) == (WIDTH, HEIGHT),
            "one_region": info.region_count == 1 and len(info.regions) == 1,
            "frame_count": info.frame_count == expected_count,
            "pixel_count": info.pixel_count == WIDTH * HEIGHT,
            "exposure": math.isclose(info.exposure_seconds, exposure, rel_tol=1e-6, abs_tol=1e-6),
            "round_trip_pixels": all(
                np.array_equal(np.asarray(actual), np.asarray(expected))
                for actual, expected in zip(decoded, frames[name])
            ),
        }
        checks[name] = {"passed": all(fields.values()), "fields": fields, "path": str(path)}
    return checks


def run_driver_checks(camera, output_dir, exposure=0.1, simulated=False, setpoint=-120.0):
    started = time.monotonic()
    camera.open()
    try:
        identity = None if simulated else camera.identity
        shape_matches = (camera.width, camera.height) == (WIDTH, HEIGHT)
        if identity is not None:
            shape_matches = shape_matches and (identity.width, identity.height) == (WIDTH, HEIGHT)
        identify = {
            "passed": bool(shape_matches),
            "width": camera.width,
            "height": camera.height,
            "identity": None if identity is None else dict(identity.__dict__),
        }
        temperature = float(camera.temperature())
        temperature_check = {
            "passed": math.isfinite(temperature),
            "value": temperature,
            "units": "degree Celsius",
        }

        single = [_acquire(camera, exposure, "normal") for _ in range(2)]
        closed = [_acquire(camera, exposure, "closed") for _ in range(2)]
        series = [_acquire(camera, exposure, "normal") for _ in range(3)]
        settings = _settings_checks(camera, 0.01, setpoint, simulated)
    finally:
        camera.close()
    frames = {"single": single, "closed": closed, "series": series}
    all_frames = np.stack(single + closed + series)
    structure = inspect_frames(all_frames)
    repeated = _repeat_consistency(single, series)
    series_match = _pixel_consistency(single, series)
    spe_paths = {name: output_dir / f"{name}.spe" for name in frames}
    spe_results = _spe_checks(spe_paths, frames, exposure)
    spe_header = {
        "passed": all(item["passed"] for item in spe_results.values()),
        "files": spe_results,
    }
    return {
        "passed": all(
            item["passed"]
            for item in (identify, temperature_check, structure, repeated, series_match, spe_header, settings)
        ),
        "duration_seconds": time.monotonic() - started,
        "shape": [HEIGHT, WIDTH],
        "identify": identify,
        "temperature": temperature_check,
        "structure": structure,
        "repeated_within_noise": repeated,
        "series_match_single": series_match,
        "spe_header": spe_header,
        "settings": settings,
        "exposure_seconds": exposure,
    }


def acquire_characterization_frames(camera, exposures, flats, spatial_count=50):
    dark = []
    flat = [] if flats else None
    for exposure in exposures:
        dark.append([
            (_acquire(camera, exposure, True), _acquire(camera, exposure, True))
        ])
        if flats:
            flat.append([
                (_acquire(camera, exposure, False), _acquire(camera, exposure, False))
            ])
    spatial_exposure = exposures[-1]
    spatial_dark = [_acquire(camera, spatial_exposure, True) for _ in range(spatial_count if flats else 0)]
    spatial_flat = [_acquire(camera, spatial_exposure, False) for _ in range(spatial_count if flats else 0)]
    return {
        "dark": dark,
        "flat": flat,
        "spatial_dark": spatial_dark,
        "spatial_flat": spatial_flat,
        "spatial_exposure_seconds": spatial_exposure,
    }


def _exposures(text):
    try:
        values = tuple(float(value) for value in text.split(","))
    except ValueError as error:
        raise argparse.ArgumentTypeError("exposures must be comma-separated seconds") from error
    if not values or any(value <= 0.0 for value in values):
        raise argparse.ArgumentTypeError("exposures must be positive")
    return values


def _setpoint(value):
    number = float(value)
    if not math.isfinite(number) or not -152.0 <= number <= -25.0:
        raise argparse.ArgumentTypeError("setpoint must be within the software-accepted -152 to -25 C encoding range")
    return number


def configure_logging(arguments):
    handler = logging.FileHandler(arguments.log_file, encoding="utf-8") if arguments.log_file else logging.StreamHandler()
    logging.basicConfig(
        level=getattr(logging, arguments.log_level),
        handlers=[handler],
        format="%(asctime)s %(name)s %(levelname)s %(message)s",
        force=True,
    )


def parse_arguments(arguments=None):
    parser = argparse.ArgumentParser(prog="openst133 check", description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--simulate", action="store_true", help="use only simulated and synthetic frames")
    mode.add_argument("--hardware", action="store_true", help="run on the connected ST-133 (writes and persists the setpoint)")
    parser.add_argument("--flats", action="store_true", help="acquire flat pairs; requires uniform illumination")
    parser.add_argument("--exposures", type=_exposures, default=(0.001, 0.01, 0.1, 0.5, 1.0))
    parser.add_argument("--output", type=Path, help="new output directory for SPE, PGM and JSON files")
    parser.add_argument("--seed", type=int, default=20261117, help="simulation seed")
    parser.add_argument(
        "--setpoint",
        type=_setpoint,
        default=-120.0,
        help="setpoint to write and leave persisted, in C (default: -120; range: -152 to -25)",
    )
    parser.add_argument(
        "--log-level",
        choices=("DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"),
        default="WARNING",
        help="logging threshold (default: WARNING)",
    )
    parser.add_argument("--log-file", help="append log records to this plain file instead of stderr")
    return parser.parse_args(arguments)


def main(arguments=None):
    arguments = parse_arguments(arguments)
    started = time.monotonic()
    try:
        configure_logging(arguments)
        ensure_real_camera_allowed(arguments.hardware) if not arguments.simulate else None
        output = arguments.output
        if output is None:
            stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
            output = Path(f"hardware-check-{stamp}")
        output.mkdir(parents=True, exist_ok=False)
        if arguments.simulate:
            from openst133.camera import SimulatedCamera

            camera = SimulatedCamera(sleep_scale=0.0, seed=arguments.seed)
        else:
            from openst133.camera import RealCamera
            camera = RealCamera()
        driver = run_driver_checks(
            camera, output, simulated=arguments.simulate, setpoint=arguments.setpoint
        )
        if arguments.simulate:
            frames = synthetic_frames(
                arguments.exposures,
                width=WIDTH,
                height=HEIGHT,
                flats=arguments.flats,
                spatial_count=50,
                seed=arguments.seed + 1,
            )
        else:
            camera.open()
            try:
                frames = acquire_characterization_frames(camera, arguments.exposures, arguments.flats)
            finally:
                camera.close()
        quantities = characterize(frames, arguments.exposures, output / "emva1288")
        characterization_report = {
            "backend": quantities["backend"],
            "emva1288": quantities["emva1288"],
            "descriptor": quantities["descriptor"],
            "dark_only": not arguments.flats,
            "quantities": {
                name: quantities[name]
                for name in ("offset", "read_noise", "dark_current", "linearity", "gain")
            },
        }
        if "fixture" in frames:
            characterization_report["synthetic_fixture"] = frames["fixture"]
        report = {
            "passed": bool(driver["passed"]),
            "mode": "simulate" if arguments.simulate else "hardware",
            "duration_seconds": time.monotonic() - started,
            "driver": driver,
            "characterization": characterization_report,
        }
    except Exception as error:
        print(f"hardware check failed: {error}", file=sys.stderr)
        return 2
    report_path = output / "report.json"
    report_path.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    print(f"{'PASS' if report['passed'] else 'FAIL'}: {report_path}")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
