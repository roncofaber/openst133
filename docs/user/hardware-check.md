# Hardware check

Run `openst133 check` to check the driver, save representative SPE files and characterize offset, read noise, dark current and, with uniform light, linearity and conversion gain. Start without hardware:

```sh
openst133 check --simulate --output simulated-dark-check
```

Real-camera operation uses the [startup data](startup-data.md). Before any real-camera run, complete the [safety preflight and shutdown checklist](safety.md), and see the [feature support matrix](support-matrix.md) for validation limits.

The simulated run exercises every driver check and writes the same JSON fields as a hardware run. Add `--flats` for synthetic flat frames. Simulated levels, timing, gain and noise are fixtures, not camera measurements.

## Dark-only run on the camera

Sends camera traffic and writes a temperature setpoint. Close any VM holding the camera and pick a new output folder:

```sh
openst133 check --hardware --output hardware-check-dark
```

The tool refuses to open hardware without `--hardware` and while a QEMU VM appears to hold the camera. It requests -120 C by default and leaves that target persisted for later opens. `--setpoint C` accepts software encoding values from -152 to -25 C; this is not a validated detector operating range, and cooled operation remains unvalidated. Choose a target appropriate to your detector and its manual. Restoring any previous target is the operator's responsibility because the interface has no separate live-setpoint readback.

The default dark-only sequence is designed to finish in under two minutes on the supported camera. `duration_seconds` in the report measures the complete run, including startup and characterization. Flat-field runs or longer custom exposure lists can take longer.

## Driver checks

All settings-variation checks other than the stream use 0.01 s exposures. The baseline consistency and SPE checks use 0.1 s. The report records every requested setting, frame shape, data type, exposure and acquisition duration. Frame summaries include minimum, maximum, mean, standard deviation, zero pixels and pixels at the uint16 maximum.

| Check | Operations and pass criteria | Numbers to read |
| --- | --- | --- |
| Identify and temperature | Open the 1340 x 100 camera and read a finite temperature. | Sensor and identity dimensions, temperature in C. |
| Baseline full frames | Take two Normal, two Disabled Closed and three Normal frames at 0.1 s. Reject wrong shape or type, zero or saturated frames, constant rows or columns, inconsistent pair noise and shifted or swapped pixels. | `structure`, `repeated_within_noise`, `series_match_single`. |
| SPE round trip | Write and reread the baseline frames and verify dimensions, type, exposure, frame counts and every pixel. | Per-file fields under `spe_header`. |
| Gain 1, 2 and 3 | Apply each controller gain and take a closed full frame. Each selection and frame must be accepted with the expected shape and uint16 type. This checks control and readout, not gain calibration or a required intensity ratio. | Per-gain duration and frame statistics under `gain.frames`. |
| Disabled Closed and Disabled Opened | Acquire both shutter modes at gain 2 and verify the selected modes and returned frames. Saturation is counted and reported for these diagnostic frames rather than used as a shutter-mode criterion. | Per-mode statistics and `opened_minus_closed_mean_dn`. |
| Full vertical binning | Return one uint16 spectrum with 1340 columns. | FVB ROI, binning, frame shape and statistics. |
| Binned ROI | Select sensor ROI `0,40,400,20` and bin `4x1`; require a uint16 frame shaped `(20, 100)` in Python row, column order. | ROI, binning, frame shape and statistics. |
| Short FVB stream | Stream 20 FVB spectra at 0.001 s; require all 20, the correct shapes and uint16 types, no consumer drops and no discarded partial bytes. | `duration_seconds`, `frames_per_second`, drop count and native stream counters. |
| Three accumulations | Compare one closed uint16 frame with one closed 3-accumulation int32 frame at the same gain. Require full-frame shapes and a mean ratio between 2 and 4. | Both frame summaries, durations and `accumulated_to_single_mean_ratio`. |
| Setpoint persistence | Write the requested setpoint, close, reopen and compare the resulting target. Hardware must additionally report target source `persisted`. | Requested and reopened setpoint, target source and temperatures before and after. |

The settings-variation results are under `driver.settings.checks`. The top-level `passed` is true only when all baseline and settings-variation checks pass. Characterization quantities that lack the required illumination say `not_measured` and do not by themselves fail the driver check.

## EMVA characterization

The tool takes closed-shutter pairs at 0.001, 0.01, 0.1, 0.5 and 1 s; change these with `--exposures`. These acquisitions retain the full-frame geometry.

| Quantity | Method | Unit |
| --- | --- | --- |
| Offset | Mean of both shortest dark frames | DN |
| Read noise | `std(D1 - D2) / sqrt(2)` | DN |
| Dark current | Slope of dark mean against exposure | DN/s |
| Linearity | Deviation of dark-corrected flat mean from a straight-line fit | % |
| Conversion gain | Photon transfer: median `mean / variance` over unsaturated pairs | e-/DN |

These follow Micro-Manager's [Measuring camera specifications](https://micro-manager.org/Measuring_camera_specifications). Compare with WinSpec only from pairs taken in the same session, at the same settings, temperature and light.

## Run with uniform light

Use a stable light that fills the sensor evenly without saturating it:

```sh
openst133 check --hardware --flats --exposures 0.001,0.005,0.02,0.1,0.4 --output hardware-check-flat
```

Pick a longest exposure near half of full scale: the tool also takes 50-frame dark and flat sets there. Uniform flat-field characterization has not been established by the dark-only simulation.

## Output files

The new output folder holds `report.json`, `single.spe`, `closed.spe`, `series.spe` and `emva1288/` (descriptor and PGM images). Settings-variation diagnostic frames are summarized in the JSON rather than saved as additional SPE files. `passed` in `report.json` covers the driver checks; every individual check also has its own `passed` field.

## Optional EMVA 1288 backend

If the optional [emva1288](https://emva1288.readthedocs.io/en/latest/) package (LGPL-3.0, imported at run time only) is installed and flat pairs exist, it also reports system gain `K` and dark current. Install it in a separate environment, because the extra pins older NumPy, SciPy and matplotlib versions:

```sh
python3 -m venv .venv-hw
. .venv-hw/bin/activate
python -m pip install "openst133[characterization]"
openst133 check --simulate --flats --output simulated-flat-check
```

Without it the report identifies the built-in fallback calculations above.
