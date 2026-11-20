# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT

"""EMVA 1288 interchange and documented camera characterization formulas."""

import math
import logging
import warnings
from importlib import metadata
from pathlib import Path

import numpy as np


MICROMANAGER = "https://micro-manager.org/Measuring_camera_specifications"
EMVA_DOCUMENTATION = "https://emva1288.readthedocs.io/en/latest/"
SATURATION_DN = 65535.0


def _quantity(status, method, source, **values):
    return {"status": status, "method": method, "source": source, **values}


def _pair(condition):
    if len(condition) != 1 or len(condition[0]) != 2:
        raise ValueError("each condition must contain one pair of frames")
    return condition[0]


def _pair_mean(condition):
    pair = _pair(condition)
    return float(np.mean(np.asarray(pair[0], dtype=np.float64)) + np.mean(np.asarray(pair[1], dtype=np.float64))) / 2.0


def _linear_fit(x, y):
    x = np.asarray(x, dtype=np.float64)
    y = np.asarray(y, dtype=np.float64)
    slope, intercept = np.polyfit(x, y, 1)
    fitted = slope * x + intercept
    return float(slope), float(intercept), fitted


def _not_measured(method, source, reason, **values):
    return _quantity("not_measured", method, source, reason=reason, **values)


def analyze_fallback(frames, exposures):
    exposures = tuple(float(value) for value in exposures)
    dark = frames["dark"]
    if len(dark) != len(exposures):
        raise ValueError("each exposure needs one dark frame pair")
    if not exposures:
        raise ValueError("at least one exposure is required")

    shortest = int(np.argmin(exposures))
    offset_value = _pair_mean(dark[shortest])
    difference = np.asarray(_pair(dark[shortest])[0], dtype=np.float64) - np.asarray(
        _pair(dark[shortest])[1], dtype=np.float64
    )
    read_noise_value = float(np.std(difference) / math.sqrt(2.0))

    offset = _quantity(
        "measured",
        "Mean of both shortest-exposure closed-shutter frames.",
        MICROMANAGER,
        value=offset_value,
        units="DN",
        exposure_seconds=exposures[shortest],
        backend="fallback",
    )
    read_noise = _quantity(
        "measured",
        "Population standard deviation of shortest-exposure dark pair D1-D2 divided by sqrt(2).",
        MICROMANAGER,
        value=read_noise_value,
        units="DN",
        exposure_seconds=exposures[shortest],
        backend="fallback",
    )

    dark_method = "Least-squares slope of dark-frame mean versus exposure time."
    if len(set(exposures)) < 2:
        dark_current = _not_measured(
            dark_method, EMVA_DOCUMENTATION, "At least two distinct dark exposures are required.", units="DN/s"
        )
    else:
        dark_means = [_pair_mean(pair) for pair in dark]
        slope, intercept, fitted = _linear_fit(exposures, dark_means)
        dark_current = _quantity(
            "measured",
            dark_method, EMVA_DOCUMENTATION,
            value=slope,
            units="DN/s",
            intercept_dn=intercept,
            exposure_seconds=list(exposures),
            dark_mean_dn=dark_means,
            residuals_dn=(np.asarray(dark_means) - fitted).tolist(),
            backend="fallback",
        )

    gain_method = "Median of dark-corrected mean divided by dark-pair difference variance divided by two, over unsaturated points whose pair variance exceeds four times the read-noise variance."
    linearity_method = "Least-squares fit of dark-corrected flat mean versus exposure, with minimum and maximum deviations from the fit."
    flat = frames.get("flat")
    if not flat:
        gain = _not_measured(
            gain_method,
            MICROMANAGER,
            "Gain requires frame pairs under uniform illumination; use --flats only with a uniform light source.",
            electrons_per_dn=None,
            dn_per_electron=None,
        )
        linearity = _not_measured(
            linearity_method,
            MICROMANAGER,
            "Optical response linearity requires uniform-illumination flat pairs.",
            minimum_percent=None,
            maximum_percent=None,
        )
        return {
            "offset": offset,
            "read_noise": read_noise,
            "dark_current": dark_current,
            "linearity": linearity,
            "gain": gain,
        }

    if len(flat) != len(exposures):
        raise ValueError("each flat exposure needs one frame pair")
    flat_means = np.asarray([_pair_mean(pair) for pair in flat])
    corrected = flat_means - np.asarray([_pair_mean(pair) for pair in dark])
    variances = np.asarray(
        [
            np.var(np.asarray(_pair(condition)[0], dtype=np.float64) - np.asarray(_pair(condition)[1], dtype=np.float64)) / 2.0
            for condition in flat
        ]
    )
    maxima = np.asarray([max(np.max(_pair(condition)[0]), np.max(_pair(condition)[1])) for condition in flat])
    linear_selection = (corrected > 0.0) & (maxima < SATURATION_DN)
    if np.count_nonzero(linear_selection) < 2:
        linearity = _not_measured(
            linearity_method,
            MICROMANAGER,
            "Fewer than two positive, unsaturated flat levels are available.",
            minimum_percent=None,
            maximum_percent=None,
        )
    else:
        x = np.asarray(exposures)[linear_selection]
        y = corrected[linear_selection]
        slope, intercept, fitted = _linear_fit(x, y)
        deviation = (y - fitted) / fitted * 100.0
        linearity = _quantity(
            "measured",
            linearity_method,
            MICROMANAGER,
            minimum_percent=float(np.min(deviation)),
            maximum_percent=float(np.max(deviation)),
            fit_slope_dn_per_second=slope,
            fit_intercept_dn=intercept,
            exposure_seconds=x.tolist(),
            corrected_mean_dn=y.tolist(),
            backend="fallback",
        )

    selection = (
        linear_selection
        & (maxima < 0.70 * SATURATION_DN)
        & (variances > 4.0 * read_noise_value**2)
        & (variances > 0.0)
    )
    if not np.any(selection):
        gain = _not_measured(
            gain_method,
            MICROMANAGER,
            "No unsaturated flat pair lies above the read-noise-dominated region of the photon transfer curve.",
            electrons_per_dn=None,
            dn_per_electron=None,
        )
    else:
        samples = corrected[selection] / variances[selection]
        electrons_per_dn = float(np.median(samples))
        gain = _quantity(
            "measured",
            gain_method,
            MICROMANAGER,
            electrons_per_dn=electrons_per_dn,
            dn_per_electron=1.0 / electrons_per_dn,
            exposure_seconds=np.asarray(exposures)[selection].tolist(),
            corrected_mean_dn=corrected[selection].tolist(),
            temporal_variance_dn2=variances[selection].tolist(),
            sample_electrons_per_dn=samples.tolist(),
            backend="fallback",
        )
    return {
        "offset": offset,
        "read_noise": read_noise,
        "dark_current": dark_current,
        "linearity": linearity,
        "gain": gain,
    }


def synthetic_frames(
    exposures,
    *,
    width=64,
    height=48,
    gain_e_per_dn=2.0,
    read_noise_dn=4.0,
    offset_dn=100.0,
    dark_current_dn_s=8.0,
    illumination_e_per_s=160000.0,
    flats=False,
    spatial_count=4,
    seed=1234,
):
    rng = np.random.default_rng(seed)
    exposures = tuple(float(value) for value in exposures)

    def frame(exposure, illuminated):
        dark_electrons = dark_current_dn_s * gain_e_per_dn * exposure
        light_electrons = illumination_e_per_s * exposure if illuminated else 0.0
        electrons = rng.poisson(dark_electrons + light_electrons, size=(height, width))
        values = offset_dn + electrons / gain_e_per_dn + rng.normal(
            0.0, read_noise_dn, size=(height, width)
        )
        return np.clip(np.rint(values), 0, 65535).astype(np.uint16)

    dark = [[(frame(exposure, False), frame(exposure, False))] for exposure in exposures]
    flat = (
        [[(frame(exposure, True), frame(exposure, True))] for exposure in exposures]
        if flats
        else None
    )
    spatial_exposure = exposures[-1]
    spatial_dark = [frame(spatial_exposure, False) for _ in range(spatial_count if flats else 0)]
    spatial_flat = [frame(spatial_exposure, True) for _ in range(spatial_count if flats else 0)]
    return {
        "dark": dark,
        "flat": flat,
        "spatial_dark": spatial_dark,
        "spatial_flat": spatial_flat,
        "spatial_exposure_seconds": spatial_exposure,
        "fixture": {
            "gain_e_per_dn": gain_e_per_dn,
            "read_noise_dn": read_noise_dn,
            "offset_dn": offset_dn,
            "dark_current_dn_s": dark_current_dn_s,
            "seed": seed,
        },
    }


def _write_pgm(path, frame):
    values = np.asarray(frame, dtype=np.uint16)
    if values.ndim != 2:
        raise ValueError("EMVA images must be two-dimensional")
    header = f"P5\n{values.shape[1]} {values.shape[0]}\n65535\n".encode("ascii")
    path.write_bytes(header + values.astype(">u2", copy=False).tobytes())


def write_emva_dataset(directory, frames, exposures):
    directory = Path(directory)
    images = directory / "images"
    images.mkdir(parents=True, exist_ok=True)
    exposures = tuple(float(value) for value in exposures)
    if frames["dark"]:
        height, width = np.asarray(frames["dark"][0][0][0]).shape
    else:
        raise ValueError("dark frames are required")
    lines = [
        "v 4.0",
        f"n 16 {width} {height}",
        "# Bright photon fields are exposure-proportional relative units, not radiometric photon counts.",
        "# Temporal sets contain two frames. Spatial sets contain more than two frames at one exposure.",
    ]

    def add_set(prefix, exposure, image_list, photons=None):
        if prefix == "b":
            lines.append(f"b {exposure * 1e9:.9f} {photons:.9f}")
        else:
            lines.append(f"d {exposure * 1e9:.9f}")
        for name, image in image_list:
            relative = f"images\\{name}.pgm"
            _write_pgm(images / f"{name}.pgm", image)
            lines.append(f"i {relative}")

    for index, exposure in enumerate(exposures):
        dark_images = [(f"d_{index:03d}_{number}", image) for number, image in enumerate(frames["dark"][index][0], 1)]
        if frames.get("flat"):
            flat_images = [(f"b_{index:03d}_{number}", image) for number, image in enumerate(frames["flat"][index][0], 1)]
            add_set("b", exposure, flat_images, exposure * 1e6)
        add_set("d", exposure, dark_images)
    if frames.get("flat"):
        exposure = float(frames["spatial_exposure_seconds"])
        bright_images = [(f"b_s_{index:03d}", image) for index, image in enumerate(frames["spatial_flat"])]
        dark_images = [(f"d_s_{index:03d}", image) for index, image in enumerate(frames["spatial_dark"])]
        add_set("b", exposure, bright_images, exposure * 1e6)
        add_set("d", exposure, dark_images)
    descriptor = directory / "EMVA1288_Data.txt"
    descriptor.write_text("\n".join(lines) + "\n", encoding="ascii")
    return descriptor


def _emva_number(results, name):
    value = getattr(results, name)
    if callable(value):
        value = value()
    if isinstance(value, dict):
        value = value.get("Value")
    value = float(value)
    if not math.isfinite(value):
        raise ValueError(f"emva1288 returned non-finite {name}")
    return value


def characterize(frames, exposures, directory):
    directory = Path(directory)
    descriptor = write_emva_dataset(directory, frames, exposures)
    fallback = analyze_fallback(frames, exposures)
    if not frames.get("flat"):
        return {
            **fallback,
            "backend": "fallback",
            "emva1288": "not applicable without uniform-illumination flat pairs",
            "descriptor": str(descriptor),
        }
    try:
        from emva1288 import process

        version = metadata.version("emva1288")
        previous_logging_disable = logging.root.manager.disable
        with warnings.catch_warnings():
            warnings.filterwarnings("ignore", category=ResourceWarning, module=r"emva1288(\..*)?$")
            logging.disable(logging.INFO)
            try:
                parser = process.ParseEmvaDescriptorFile(str(descriptor))
                loaded = process.LoadImageData(parser.images)
                transformed = process.Data1288(loaded.data)
                results = process.Results1288(transformed.data)
            finally:
                logging.disable(previous_logging_disable)
        system_gain = _emva_number(results, "K")
        if system_gain <= 0.0:
            raise ValueError("emva1288 returned a non-positive system gain")
        emva_dark_current = _emva_number(results, "u_I_mean_DN")
    except ImportError:
        return {
            **fallback,
            "backend": "fallback",
            "emva1288": "not installed; documented pair-difference fallback used",
            "descriptor": str(descriptor),
        }
    except Exception as error:
        return {
            **fallback,
            "backend": "fallback",
            "emva1288": f"processing failed; documented fallback used: {type(error).__name__}: {error}",
            "descriptor": str(descriptor),
        }

    gain = dict(fallback["gain"])
    gain.update(
        {
            "status": "measured",
            "method": "EMVA 1288 release 4.0 system-gain fit of dark-corrected temporal variance versus mean; inverse_K is electrons per DN.",
            "source": EMVA_DOCUMENTATION,
            "electrons_per_dn": 1.0 / system_gain,
            "dn_per_electron": system_gain,
            "backend": "emva1288",
        }
    )
    gain.pop("reason", None)
    dark_current = dict(fallback["dark_current"])
    dark_current.update(
        {
            "status": "measured",
            "method": "EMVA 1288 release 4.0 u_I_mean_DN least-squares slope of mean dark signal versus exposure.",
            "source": EMVA_DOCUMENTATION,
            "value": emva_dark_current,
            "backend": "emva1288",
        }
    )
    dark_current.pop("reason", None)
    return {
        **fallback,
        "gain": gain,
        "dark_current": dark_current,
        "backend": "emva1288",
        "emva1288": f"emva1288 {version}, EMVA 1288 standard release 4.0",
        "descriptor": str(descriptor),
    }
