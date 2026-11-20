# Developer guide

This guide covers the source layout, local builds, tests and contribution rules for openst133. For installation and camera operation, start with the [Linux quickstart](../user/quickstart.md) or [Windows setup](../user/windows.md).

## Source layout

| Path | Contents |
| --- | --- |
| `include/` | Public C headers for the camera library and SPE file support |
| `src/` | C11 library implementation and the `ost` command-line program |
| `python/` | Python package (binding, viewer, startup data, hardware check, temperature log, udev setup) and its tests |
| `integrations/` | ScopeFoundry integration |
| `examples/` | Plain-script, Bluesky, PyMoDAQ and ctypes examples with their tests |
| `tools/` | Source of the `ost temperature` command |
| `tests/` | Native unit, integration and regression tests registered with CTest |
| `packaging/` | udev rule, bundled license texts and release support |
| `pyproject.toml` | PyPI package definition; builds the C library through scikit-build-core |

The public C interfaces are in [openst133.h](../../include/openst133.h) and [openst133_spe.h](../../include/openst133_spe.h). The root [CMakeLists.txt](../../CMakeLists.txt) defines the library, CLI and native tests. Python packaging and its required dependencies are declared in [pyproject.toml](../../pyproject.toml).

## Build prerequisites

A default build and test run requires a C11 compiler, CMake 3.16 or newer, libusb 1.0 development files, Python 3.10 or newer and NumPy. `pkg-config` helps CMake locate libusb; CMake also has a direct header-and-library search fallback. The Python package build additionally uses `scikit-build-core`, which its packaging metadata declares.

On Debian or Ubuntu, the native build packages are `build-essential`, `cmake`, `pkg-config` and `libusb-1.0-0-dev`. Install Python and NumPy with the system package manager or in a virtual environment.

## Build and test

Run these commands from the repository root. Keep the no-hardware guard set for all ordinary development and test runs.

```bash
export OPENST133_NO_HARDWARE=1
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

For a multi-configuration generator, select the configuration with `cmake --build build --config Debug` and `ctest --test-dir build -C Debug --output-on-failure`.

CTest runs the native tests and the Python test suite. It passes the path of the newly built shared library to the Python tests, so an installed openst133 package is not required. Use `ctest --test-dir build -R <name> --output-on-failure` for a focused test while debugging, then rerun the complete suite before submitting the change.

To run Python discovery directly on Linux, point `OPENST133_LIBRARY` at the shared library from the same build:

```bash
OPENST133_NO_HARDWARE=1 OPENST133_LIBRARY="$PWD/build/libopenst133.so" \
  python3 -I -m unittest discover -s python/tests -t python
```

On macOS, use `libopenst133.dylib`. On Windows, use the CTest command above so CMake supplies the correct DLL path and configuration.

To build and install the package exactly as PyPI users get it, run `python -m pip install .` from the repository root, or `python -m build` to produce the sdist and wheel in `dist/`. Releases are published by pushing a `vMAJOR.MINOR.PATCH` tag: `.github/workflows/release.yml` builds the sdist and the manylinux and Windows wheels, uploads them to PyPI with trusted publishing and attaches them to a GitHub Release.

## Simulated cameras

The tests exercise camera-facing code without requiring connected hardware. Native tests drive the driver through mock transports defined in the test files and through the replay transport, which plays back recorded USB traffic. Python tests also use [SimulatedCamera](../../python/src/openst133/camera.py) to generate deterministic synthetic frames for API, viewer and integration tests.

When changing acquisition, readout, streaming, cancellation or error handling, add a deterministic simulated test that demonstrates the intended behavior and relevant failure cases. Run it first as a focused test and then as part of the complete suite. A simulation checks the software contract; it does not establish compatibility with a different camera or detector.

## Hardware safety

`OPENST133_NO_HARDWARE=1` makes the native library and Python real-camera interface refuse to open physical hardware. Simulated transports remain available under this guard. Unsetting the variable or assigning another value does not provide the same protection, so do not change it in a development shell used for tests.

New code must never send a camera command that the driver does not already use. Do not guess an operation, extend a command sequence or try an unverified operation on a camera to see what happens. If required behavior is unsupported, return an explicit unsupported result and document the limitation instead of inventing camera behavior.

Use this order for camera-facing changes:

1. Keep `OPENST133_NO_HARDWARE=1` and reproduce the relevant behavior with the simulator.
2. Add or update native and Python tests, including invalid inputs, cancellation, timeout and cleanup paths where applicable.
3. Run the complete CTest suite and the standalone Python suite without hardware.
4. Only after those checks pass, consider a separately authorized hardware check with a person responsible for the camera and follow the camera safety guidance in the [user guide](../user/README.md). Never use real hardware as the first test of a change.

## Coding and documentation conventions

- Keep native code C11-compatible with compiler extensions disabled. The build uses `-Wall -Wextra -Werror`; do not silence a warning without addressing its cause.
- Keep public declarations in `include/` and implementation details in `src/`. Preserve API and ABI compatibility unless a deliberate interface change and its migration are part of the contribution.
- Check every return status, preserve useful error information and release resources on success, failure, cancellation and timeout paths.
- Support Python 3.10 and newer. Put Python dependencies in `pyproject.toml`, use standard-library `unittest` for tests and standard-library logging in library code rather than `print`.
- Use the existing simulator and test helpers for camera-facing behavior. Tests must be deterministic, clean up their temporary files and processes, and pass with `OPENST133_NO_HARDWARE=1`.
- Start new C and Python source files with the project copyright line and `SPDX-License-Identifier: MIT`; see an adjacent source file for the exact form. Third-party dependencies require a permissive license and an entry in [THIRD_PARTY_NOTICES.md](../../THIRD_PARTY_NOTICES.md).
- Do not commit build output, startup data, camera serial numbers or laboratory data. Keep changes focused and avoid unrelated refactoring.
- Write each Markdown paragraph or list item as one unwrapped line. Do not use em dashes or emojis. Use relative links that resolve to files included in the public source tree.
- Run the build and both test commands for code changes, and record the actual result. Clearly distinguish simulated verification from hardware observation.

The project is distributed under the [MIT License](../../LICENSE).
