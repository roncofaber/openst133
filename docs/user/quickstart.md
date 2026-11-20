# Linux quickstart

Follow this numbered path on x86_64 Linux from installation to a saved camera frame. Everything comes from one package, `pip install openst133`; no compiler, CMake or source checkout is needed.

## Before you start

- Use CPython 3.10 to 3.14 (3.15 has no wheel yet and needs a compiler; see [Install from source](install.md#install-from-source)) on x86_64 Linux with glibc 2.28 or newer (Ubuntu 20.04, Debian 11, RHEL 8 or later).
- Have the files `PI133B.DAT` and `PICHIPDB.dll` from your licensed WinSpec, usually copied from its program folder on the Windows PC; see [Startup data](startup-data.md). The startup data you generate is private and must not be committed or shared.
- Use the supported ST-133 USB 2.0 interface `0bd7:a010` with detector 98, 1340 x 100 pixels. Other detectors are refused. Keep the controller unpowered until the safety step.

## 1. Install openst133 in a virtual environment

```sh
python3 -m venv ~/openst133-env
. ~/openst133-env/bin/activate
python -m pip install openst133
```

**Success:** pip prints `Successfully installed ... openst133-0.1.0` together with its dependencies (NumPy, tifffile and, on Python 3.14 or earlier, h5py). The wheel bundles the native library, libusb and the `ost` command. **Common fix:** on Debian or Ubuntu, `python3 -m venv` needs `sudo apt install python3-venv`.

## 2. Check the installed commands

```sh
ost --help
openst133 --help
```

**Success:** `ost` lists `list`, `info`, `spe-info`, `spe-dump`, `temperature` and `acquire`; `openst133` lists `list`, `view`, `acquire`, `startup-data`, `setup`, `setup-udev`, `check` and `temperature-log`. **Common fix:** if either command is not found, activate the environment again with `. ~/openst133-env/bin/activate`.

## 3. Run the guided setup

With the two WinSpec files at hand, run:

```sh
openst133 setup --dat /path/to/PI133B.DAT --dll /path/to/PICHIPDB.dll
```

A folder containing both files works too: `openst133 setup /path/to/folder`. The setup installs the bundled udev rule (it asks for your sudo password once), builds the private startup data from the two files and, when the camera is connected, opens and closes it without sending camera commands to confirm access. It never acquires or writes camera settings, so it is safe to run before the camera is powered; it then reports the missing camera and you rerun it in step 5.

**Success:** each section reports its result (without a camera the result is `not ready yet`, which is expected until step 5), startup data is written to `~/.local/share/openst133/firmware/st133-startup.bin` with its `.sha256` sidecar, and both remain private. **Common fix:** without sudo, ask an administrator to install the output of `openst133 setup-udev --print` as `/etc/udev/rules.d/60-openst133.rules`. If startup-data generation refuses the input, follow its reported limitation; see [Startup data](startup-data.md). Never use mode `0666` or run camera commands with `sudo`.

## 4. Run the complete check without a camera

This uses simulated hardware and synthetic frames; the environment guard refuses any accidental real-device open:

```sh
OPENST133_NO_HARDWARE=1 openst133 check --simulate --output simulated-dark-check
```

**Success:** the last line is `PASS: simulated-dark-check/report.json`; the new folder contains `report.json`, three SPE files and `emva1288/`. Simulated levels, timing, gain and noise are fixtures, not camera measurements. **Common fix:** if the output folder already exists, choose a new folder name.

## 5. Complete the safety checklist and find the camera

Complete every applicable item in the [safety checklist](safety.md) before power on. Close WinSpec and any VM that holds the camera, connect and power the controller as the checklist directs, then run:

```sh
openst133 setup
```

Use an interactive session without `OPENST133_NO_HARDWARE=1` for this and the remaining hardware steps; keep the guard enabled in automation.

**Success:** the camera section lists the interface, the access section reports `access works`, and the Result section prints `ready.` followed by the first acquisition command. **Common fix:** if no camera is found, check power, cabling and VM passthrough using [Troubleshooting](troubleshooting.md#ost-list-prints-nothing); if access is denied, reconnect the camera once so the udev rule applies, then rerun `openst133 setup`.

## 6. Save and inspect a first camera frame

This sends startup traffic and a -120 C temperature target write. A target is a request, not proof of cooling.

```sh
ost acquire --exposure 0.1 --out first-camera-frame.spe
ost spe-info first-camera-frame.spe
```

**Success:** acquisition prints `acquired 1340x100 frame, exposure 0.100 s, normal shutter, gain 2, saved first-camera-frame.spe`, and SPE inspection reports version 3, uint16 data, 1340 x 100 pixels and one frame. The Python equivalent is `openst133 acquire --exposure 0.1 --out first-camera-frame.spe`. **Common fix:** if startup data is missing, rerun `openst133 setup` with the WinSpec folder; if the interface is busy, close the named application or VM rather than running with `sudo`.

## 7. Run the hardware check

Read [Hardware check](hardware-check.md) for the full sequence and report fields, then run:

```sh
openst133 check --hardware --output hardware-check-dark
```

This sends camera traffic, writes and persists a -120 C target, and performs dark acquisitions and driver checks. **Success:** the last line is `PASS: hardware-check-dark/report.json`, and top-level `passed` is true in that report. **Common fix:** the first acquisitions after the camera has been idle can drift and fail the repeat-noise check; run the check a second time before investigating. If the tool refuses to start, remove `OPENST133_NO_HARDWARE` only from this intentional hardware session and close any VM holding the camera.

Next: [Acquire frames](acquire.md), interpret [Data quality](data-quality.md), or use the [Viewer](viewer.md).
