# Troubleshooting

Match a symptom or error message to the bounded checks below, and see the [feature support matrix](support-matrix.md) for supported configurations. Stop acquisition and follow the [safety checklist](safety.md) before changing power, cabling, cooling or shutter hardware.

## `ost list` prints nothing

No supported `0bd7:a010` interface is visible to the host. Check that the ST-133 is powered, the USB cable is connected and no VM has the device passed through. On Windows, check the WinUSB binding described in [Windows setup](windows.md).

## `permission denied opening camera USB interface ...`

On Linux, run `openst133 setup-udev` once and reconnect the camera if access is still denied. The rule grants access to the logged-in local user; over SSH without a local session, your user must be in `plugdev` (create the group with `sudo groupadd plugdev` where it does not exist) and log in again after joining. Do not use `sudo` for camera commands. On Windows, bind the WinUSB driver as described in [Windows setup](windows.md#5-bind-the-built-in-winusb-driver).

## `camera USB interface ... is busy; held by pid ...`

Another process holds the camera, often WinSpec in a VM or a second `ost` or viewer process. Close that application or shut down the VM, then retry.

## `OPENST133_NO_HARDWARE=1 forbids opening real hardware`

The session blocks camera access on purpose. Unset `OPENST133_NO_HARDWARE` only in a session intended for hardware.

## `startup data file ... not found`

Generate the file from your licensed WinSpec installation ([Startup data](startup-data.md)) or point `OPENST133_STARTUP_DATA` at it. A file that fails validation is also refused.

## An exposure is refused

`ost acquire` accepts 0 to 60 s; the Python tools, viewer and ScopeFoundry need a value above 0. Values that cannot be represented exactly are shortened to the nearest representable one, and `--strict-exposure` refuses them instead. See [Exposure](acquire.md#exposure).

## `openst133 shared library not found; set OPENST133_LIBRARY to its path`

The Python package cannot find its native library. Reinstall with `python -m pip install --force-reinstall openst133`, or set `OPENST133_LIBRARY` to a matching built `libopenst133.so`, `libopenst133.dylib` or `openst133.dll`.

## The temperature reads -25.00 C

This is the top of the implemented readable range, not room temperature. The reading alone does not diagnose a cooling fault. If the detector should be cooled but reads -25 C, repeatedly loses its target or takes progressively longer to stabilize, stop quantitative acquisition and use the cooling checks below.

## FVB spectra sit at 65535

Full vertical binning sums each detector column, so the 16-bit output can saturate. Samples at 65535 are not quantitative measurements. Reduce illumination or exposure and inspect the result before continuing; see [Data quality](data-quality.md#binning-sums-charge-and-can-saturate-earlier).

## Cooling takes progressively longer, loses lock or no longer holds the target

Stop quantitative acquisitions and compare a [temperature log](cooling.md#log-a-cooldown) with earlier runs at the same target and similar ambient and cooling conditions. The logger's `locked` state is a host software threshold, not the controller's hardware TEMP LOCK indication.

The ST-133 Controller Manual Version 3.B, pages 37-39, and MicroMAX System User Manual Version 6.C, pages 114-115, list these checks:

- Confirm that the target is appropriate for the detector and that ambient and cooling conditions are within the manufacturer's operating requirements. For liquid-nitrogen cooling, follow your site's fill procedure; a requested target alone does not establish that cooling is available.
- With power off and cables safe to handle, verify that both detector-controller cable connectors and their latches or screws are secure. Resume operation only after reconnecting according to the controller manual.
- Treat progressively longer lock times, repeated loss of a previously stable lock or a target that can no longer be held as possible vacuum deterioration. Arrange vacuum assessment and repumping through qualified service; do not open the vacuum valve or detector enclosure yourself.

The manuals recommend additional thermal settling after lock before quantitative data. openst133 does not read a vacuum sensor or provide a hardware diagnosis for these symptoms; cooled lock behavior is unvalidated.

## The baseline shifts suddenly or dark levels trend unexpectedly

Stop quantitative work. Compare dark frames at the same exposure, temperature, gain, ROI, binning and readout rate. The ST-133 Controller Manual Version 3.B, page 56, and MicroMAX System User Manual Version 6.C, pages 59-60, explain that dark charge rises with exposure and temperature, so changed conditions can alter dark level and pattern without a new hardware fault.

If a sudden baseline shift or unusual dark trend remains unexplained by settings and thermal state, keep quantitative acquisitions stopped and arrange vacuum service. The manuals associate a sudden baseline change with possible humidity in the vacuum enclosure. Follow your site's liquid-nitrogen shutdown procedure and the controller manual's power precautions, then arrange repumping through qualified service rather than opening the vacuum valve. The ST-133 manual, page 38, warns not to power off the controller while liquid nitrogen remains in the dewar. openst133 cannot distinguish humidity or vacuum deterioration from another cause.

## A shutter stops after sustained use or images become overexposed or smeared

Stop the experiment and quantitative acquisitions. Verify that the physical Shutter Setting selector matches the installed shutter before further use; an incorrect setting can damage it. For a 25 mm remote-mounted shutter that suddenly stops during sustained use, the MicroMAX System User Manual Version 6.C, page 128, describes a thermal interlock: stop the experiment, let the shutter cool, typically for up to an hour, and avoid the triggering repetition rate or insert breaks between collections.

If the shutter does not recover, remains inoperative, sticks open or closed, or has a damaged leaf, arrange replacement through qualified service. The thermal-interlock guidance is specific to the named 25 mm remote-mounted shutter; do not infer that mechanism for another shutter or treat a stopped shutter as a diagnosed controller fault.

## `ost spe-dump FILE | head` prints `standard output: I/O error`

`ost` reports the closed pipe after `head` exits. The rows already shown are valid; write the complete output to a file with `ost spe-dump FILE > frame.csv` to avoid the message.
