# Acquire frames

Use `ost acquire` to apply acquisition settings and save frames as SPE:

```sh
ost acquire --exposure 0.1 --out frame.spe
```

Prepare the [startup data](startup-data.md) and complete the [safety checklist](safety.md). Close WinSpec and any VM that holds the camera. With several cameras, add `--camera` as described in [Choosing a camera](cli.md#choosing-a-camera). If a setting or acquisition fails, `ost` prints the reason and writes no file.

## One frame

```sh
ost acquire --exposure 0.1 --out frame.spe
ost acquire --exposure 1 --closed --out dark.spe
ost acquire --exposure 0.3 --out frame-0.3s.spe
```

`--closed` selects the Disabled Closed shutter mode instead of Normal. Check a saved file with `ost spe-info FILE`.

## Gain, shutter modes and accumulations

```sh
ost acquire --exposure 0.1 --gain 3 --out gain3.spe
ost acquire --exposure 0.1 --shutter opened --out opened.spe
ost acquire --exposure 0.1 --accumulations 3 --out sum3.spe
```

`--gain` selects Controller Gain 1, 2 or 3. `--shutter opened` selects Disabled Opened. `--accumulations N` reads N frames and saves their per-pixel sum as one 32-bit LONG frame. The SPE records the gain, shutter mode and accumulation count. Supported values and validation status are in the [support matrix](support-matrix.md#readout-and-data).

## A series

```sh
ost acquire --exposure 0.1 --count 3 --out series.spe
```

`--count N` writes N frames into one SPE file. Add `--stream` for continuous acquisition as described in [Stream frames](stream.md). See the [acquisition support rows](support-matrix.md#readout-and-data) for accumulation restrictions.

## Exposure

`--exposure` takes seconds and rounds the request to the nearest millisecond. Add `--strict-exposure` to refuse a request that cannot be represented exactly. Otherwise an inexact request may be shortened; the result line shows both values, for example `exposure 9.984 s (requested 10 s)`, and the SPE records the effective value.

Allowed ranges, representation rules and the interpretation of zero and short requests are consolidated under [Exposure and temperature](support-matrix.md#exposure-and-temperature).

## Temperature target

`--setpoint C` requests a target after the other settings are accepted. Target ranges, cooling limits and lock status are consolidated under [Exposure and temperature](support-matrix.md#exposure-and-temperature); see [Cooling](cooling.md) for monitoring.

## Readout modes

`--fvb`, `--roi X,Y,W,H`, `--bin XxY` and `--rate 1M|100k` select the readout; see [Readout modes and ROI](modes.md).

## From other interfaces

- Python: `RealCamera` in [Python](python.md#acquire-from-the-camera).
- Viewer: [Viewer](viewer.md).
- ScopeFoundry: [ScopeFoundry](scopefoundry.md#use-the-real-camera).
