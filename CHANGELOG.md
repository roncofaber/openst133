# Changelog

All notable changes to openst133 are recorded here. Versions follow semantic versioning and release tags use the form `vMAJOR.MINOR.PATCH`.

## [Unreleased]

## [0.1.0] - 2026-11-20

Initial MIT-licensed release of openst133 for supported Princeton Instruments ST-133 controllers.

### Included

- Native C11 library and `ost` command for discovery, identification, temperature access, setpoint writes, acquisition, streaming and SPE output.
- One PyPI package, `openst133`, with manylinux and Windows wheels that bundle the native library, libusb and the `ost` command, plus a source distribution.
- Python bindings, a viewer and the `openst133` command: discovery, acquisition, startup-data generation, `setup-udev` for one-step Linux USB access, the hardware check (`check`) and temperature logging (`temperature-log`).
- Full-frame, region, binning and full-vertical-binning acquisition; every setting is accepted within its allowed range and unsupported combinations are refused with an error.
- SPE 2.x and 3.0 reading and writing, including acquisition metadata and timing provenance fields.
- Deterministic simulated camera support and generic PCAP or usbmon replay for development and automated tests.
- Guided `openst133 setup` and `openst133 startup-data`, which build private startup data from the user's own `PI133B.DAT` and `PICHIPDB.dll`.
- The viewer and `openst133 acquire` save SPE, HDF5, TIFF or CSV, chosen by the file extension.
- Linux and Windows setup guides; Windows uses the built-in WinUSB driver, with no third-party driver tool. ScopeFoundry integration and integration examples.
- A no-hardware guard for development, CI and automated sessions.

### Validation limits

- Linux camera checks cover the supported Spec-10:100B configuration described in the support matrix. Other detector configurations are not established.
- Cooled operation, temperature lock, External Sync, Pre-Open shutter and simultaneous physical cameras are not validated.
- Windows wheels are built and tested in CI; Windows camera operation has not been tested on hardware.
- ScopeFoundry and third-party integration tests use simulated cameras, not physical hardware.
- Generic replay, simulation and synthetic fixtures establish software behavior only; they do not replace camera validation.
