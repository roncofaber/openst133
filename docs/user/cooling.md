# Cooling

Read the detector temperature and request a target with `ost temperature`:

```sh
ost temperature
ost temperature --json
```

These commands open the camera and can write a -120 C target during startup. Complete the [safety checklist](safety.md) first. openst133 does not fill a dewar, control coolant or decide when a detector is safe to warm; follow the controller and detector manuals and your site's cryogenic-safety procedure.

Cooled operation and temperature-lock behavior have not been validated. A target request is not proof that the detector reached that temperature or is stable enough for quantitative data. See the [feature support matrix](support-matrix.md) for validation limits.

## Read the temperature

`ost temperature` runs startup, which needs the [startup data](startup-data.md), and reads the temperature. Plain output reports the detector, measured temperature and stored target. `--json` returns the same information in fields including `detector`, `detector_id`, `width`, `height`, `celsius`, `target_celsius` and `target_written`.

## Set a target

```sh
ost temperature --set-target -100
ost acquire --setpoint -100 --exposure 0.1 --out frame.spe
```

`--set-target`, `--open-target` and acquisition `--setpoint` values from -152 to -25 C in half-degree steps. These are software-accepted encoding ranges, not validated or universally safe detector operating ranges; use a target appropriate to your detector and its manual.

`--set-target` saves the target for later opens. `--open-target C` on `ost temperature` overrides the target written at open for one run. Acquisition `--setpoint C` requests a target after the other settings are accepted.

## Log a cooldown

`openst133 temperature-log` reads temperature at a fixed interval, writes CSV and can request a target. Try its simulated cooling curve first:

```sh
openst133 temperature-log --simulate --setpoint -120 --interval 1 --count 10 --output simulated-cooling.csv
```

The simulated detector starts at 20 C and approaches the target with a 600-second time constant. It does not model a physical camera or validate cooling.

For an intentional hardware run, close applications and VMs that hold the camera and choose a new output file:

```sh
openst133 temperature-log --hardware --setpoint -120 --interval 30 --lock-tolerance 2 --lock-hold 300 --plot --output cooldown.csv
```

To wait for a configurable readiness criterion, add `--wait-ready` and optionally `--settle-time`:

```sh
openst133 temperature-log --hardware --setpoint -120 --interval 30 --lock-tolerance 2 --lock-hold 300 --wait-ready --settle-time 1200 --output ready.csv
```

This example requires 300 continuous seconds within 2 C of the target followed by another 1200 seconds within that tolerance. An excursion restarts the hold and settling interval. `--wait-ready` requires `--setpoint`; if `--count` ends first, the command reports that readiness was not reached and exits 2. Without `--count`, it runs until readiness or Ctrl-C. `--settle-time` defaults to 0 seconds and has no effect without `--wait-ready`.

- Real-camera runs need `--hardware`, and the tool refuses to run while a QEMU VM appears to hold the camera.
- Omit `--setpoint` to monitor without changing the target. Stopping the logger leaves the current target in place.
- Without `--count` or `--wait-ready`, the logger runs until Ctrl-C and exits 130 after cleanup. Other errors exit 2.
- `--plot` needs matplotlib and a display; leave it out on headless machines.
- Existing output files are never overwritten.

## Read the CSV

| Column | Meaning |
| --- | --- |
| `timestamp_utc` | Time of the read |
| `elapsed_s` | Seconds since the start |
| `temperature_c` | Measured temperature |
| `setpoint_c` | Requested target, empty without `--setpoint` |
| `lock_state` | `unknown`, `unlocked` or `locked` |

`locked` is a host software judgment, not a camera status bit: temperature stayed within `--lock-tolerance` degrees (default 2) of the target for `--lock-hold` seconds (default 60). Without `--setpoint`, the state stays `unknown`.

The ST-133 Controller Manual Version 3.B, pages 17-18 and 37, describes a separate hardware TEMP LOCK indication and recommends additional settling before quantitative measurements. Neither `locked` nor `--wait-ready` reads or reproduces hardware lock, and software readiness does not establish detector safety or measurement quality.
