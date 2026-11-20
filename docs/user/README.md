# openst133 user guide

Follow the guide in this order to set up the camera, acquire useful data and find the command references.

## Guide

| Step | Pages |
| --- | --- |
| Start here | [Linux quickstart](quickstart.md) or [Windows setup](windows.md) |
| Work safely | [Safety checklist](safety.md) |
| Installation reference | [Install](install.md), [Startup data](startup-data.md) |
| Configure acquisition | [Acquire frames](acquire.md) |
| Choose a readout | [Readout modes and ROI](modes.md) |
| Acquire continuously | [Stream frames](stream.md) |
| Interpret the data | [Data quality](data-quality.md) |
| Manage temperature | [Cooling](cooling.md) |
| Diagnose a problem | [Troubleshooting](troubleshooting.md), [Hardware check](hardware-check.md) |
| Use the command reference | [Command reference](cli.md) |
| Write Python software | [Python](python.md), [Logging](logging.md) |
| Display frames | [Viewer](viewer.md) |
| Connect other software | [Integrations](integrations.md), [ScopeFoundry](scopefoundry.md) |
| Check support and validation | [Feature support matrix](support-matrix.md) |

## Terms

Two commands are installed. `ost` is the native C command line for camera and SPE-file operations. `openst133` runs the Python tools and is equivalent to `python -m openst133`.

Camera commands accept every setting within its allowed range and refuse anything else; see the [feature support matrix](support-matrix.md#camera-access) for the ranges and their validation status.

## Environment variables

| Variable | Effect |
| --- | --- |
| `OPENST133_DATA` | Data root, default `~/.local/share/openst133` on Linux or `%APPDATA%\openst133` on Windows |
| `OPENST133_STARTUP_DATA` | Full path of the startup file, default `$OPENST133_DATA/firmware/st133-startup.bin` |
| `OPENST133_LIBRARY` | Use this `libopenst133` instead of the bundled library |
| `OPENST133_NO_HARDWARE` | `1` refuses every real-device open; discovery and simulation still work |
