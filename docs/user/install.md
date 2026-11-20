# Installation

openst133 ships as one Python package on PyPI. It contains the native C library, the `ost` command, the `openst133` command with startup-data generation, the hardware check and temperature logging, the viewer and the Python API. Follow the [Linux quickstart](quickstart.md) or the [Windows setup](windows.md) for the complete numbered path.

## Install from PyPI

```sh
python -m pip install openst133
```

Wheels are published for CPython 3.10 to 3.14 on x86_64 Linux (glibc 2.28 or newer) and 64-bit Windows. They bundle libusb, so nothing else needs to be installed. Use a virtual environment so the `ost` and `openst133` commands stay together with their library. The optional EMVA 1288 characterization backend installs with `python -m pip install "openst133[characterization]"`.

## Allow USB access

`openst133 setup --dat PI133B.DAT --dll PICHIPDB.dll` covers USB access and startup data in one guided step, and reports what is still missing. The individual pieces are:

- **Linux:** run `openst133 setup-udev` once. It installs the bundled rule as `/etc/udev/rules.d/60-openst133.rules` with sudo, reloads udev and re-evaluates connected cameras. `openst133 setup-udev --print` shows the rule if you prefer to install it yourself. Reconnect the camera if access is still denied. Do not use mode `0666`, and do not run camera commands with `sudo`.
- **Windows:** bind the camera to Windows' built-in WinUSB driver in Device Manager, as described in [Windows setup](windows.md#5-bind-the-built-in-winusb-driver). No download is required.

## Install from source

On other platforms or architectures pip builds from the source distribution, which needs a C11 compiler, CMake 3.16 or newer, pkg-config and libusb 1.0 development files. On Debian or Ubuntu:

```sh
sudo apt install build-essential cmake pkg-config libusb-1.0-0-dev python3-venv
python -m pip install openst133
```

From a source checkout, run `python -m pip install .` in the repository root instead. An editable install (`python -m pip install -e .`) also works; rerun it after changing C code.

## Build the native library alone

Use this optional route for C development. From the source root:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build
cmake --install build --prefix "$PWD/prefix"
```

The staged prefix contains `bin/ost`, `lib/libopenst133.so`, headers, pkg-config metadata and `share/openst133/udev/60-openst133.rules`. CMake does not write to `/etc`. To make a separate Python installation use this library, set `OPENST133_LIBRARY="$PWD/prefix/lib/libopenst133.so"`.

## Block the camera in automated sessions

Set `OPENST133_NO_HARDWARE=1` in scripts, CI and agent sessions that must not touch the camera. Every real-device open then fails with a message naming the variable; simulation still works. Listing with `ost list` only enumerates USB devices and is still allowed. Restart long-running sessions after changing the variable.
