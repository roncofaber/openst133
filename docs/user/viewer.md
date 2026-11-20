# Viewer

Display live camera frames and save them as SPE, HDF5, TIFF or CSV with `openst133 view`, or try the interface without hardware:

```sh
openst133 view --simulate
```

The viewer needs Tk (Debian or Ubuntu package `python3-tk`) and a graphical session.

## Open the camera

Complete the [safety checklist](safety.md), prepare the [startup data](startup-data.md) and close applications and VMs that hold the camera. Then run:

```sh
openst133 view
```

Opening the camera sends hardware commands. `--log-level` and `--log-file` are described in [Logging](logging.md); `--close-after N` closes the window after N seconds.

## Controls

Settings apply as soon as you choose them: dropdowns on selection, arrow buttons and arrow keys on each step, and typed values once you pause typing for about half a second, press Return or leave the field. During **Continuous**, a change briefly stops the stream, applies the setting and resumes it.

| Control | Effect |
| --- | --- |
| **Exposure (s)** | Exposure in seconds, above 0 and up to 60 |
| **Acquire** | Acquire one frame |
| **Continuous** / **Stop** | Stream frames until stopped ([Stream frames](stream.md)) |
| **Save...** | Save the last displayed frame as SPE, HDF5, TIFF or CSV ([Saving](#saving)) |
| **Temperature** | Show the current reading |
| **Shutter** | `normal`, `closed` (Disabled Closed) or `opened` (Disabled Opened) |
| **Gain** | Controller gain 1, 2 or 3 |
| **Accumulations** | Sum 1, 3 or 10 frames into one 32-bit frame (single Acquire only) |
| **Setpoint (C)** | Request a target from -152 to -25 C in 0.5 C steps ([Cooling](cooling.md)) |
| **Readout** | Digitization speed: `1 MHz` (fast) or `100 kHz` (slower, lower read noise); `100 kHz` is greyed out unless the region is the full sensor without binning |
| **Region** | **Full sensor**, or **Custom** with the **x**, **y**, **w**, **h** rectangle in sensor pixels from the top-left corner; the arrows step by one pixel |
| **Bin x**, **Bin y** | Combine x by y neighbouring pixels; the arrows step only through factors that divide the region's width and height |
| **Sum rows (FVB)** | Sum every row of the region into one spectrum, optionally with horizontal binning; **Bin y** is ignored |

The image shows the active window as a cyan outline with corner handles, hidden while the region is the full sensor. Drag inside it to move it, drag an edge or corner to resize it, or drag outside it to draw a new one; the cursor changes to show which. During any of these drags the pointer can leave the image, even the window, and the region stops at the sensor edge, so pulling past the edge gives a full-width or full-height window; a drag can also start in the narrow border around the image. On an FVB spectrum, drag across the columns to keep, and the rows it sums stay the same. Every change snaps to whole bins, stays inside the sensor and is applied when you release the button, restarting a running stream. Double-click the display to return to **Full sensor**.

Region, binning and **Sum rows** combine freely, matching the `ost acquire` flags in [Readout modes and ROI](modes.md). The label after **Sum rows** previews the output size, such as `Output 400 x 1 spectrum`; when the values do not fit it names the field to fix in red and nothing is sent to the camera.

One-row readouts such as FVB appear as line plots. Other readouts appear as images on a view of the whole 1340 x 100 sensor: each readout pixel is drawn where it sits on the chip, stretched over its bin, and the area outside the region is dark grey. While nothing is acquiring, a readout change redraws at once: an image keeps its data under the new window, and ticking **Sum rows** previews the spectrum from the last image, marked as a preview in the status line. During **Continuous** the stream restarts with the new readout. FVB saturates much sooner than a full frame; see [Data quality](data-quality.md#binning-sums-charge-and-can-saturate-earlier). Library refusals appear in the status line. When the display falls behind during **Continuous**, the oldest queued frame is dropped. Closing the window stops acquisition and closes the camera.

## Menus and shortcuts

| Menu | Items |
| --- | --- |
| **File** | **Save frame as...** (Ctrl+S), **Quit** (Ctrl+Q) |
| **Acquisition** | **Acquire** (Ctrl+Return), **Continuous** (Ctrl+Shift+Return), **Stop** (Esc), **Full sensor** |
| **Colors** | Display colormap: gray, heat, viridis, inferno or magma; the current frame redraws at once |
| **Help** | **Project page on GitHub**, **About openst133** |

## Saving

The file extension selects the format; a name typed without one gets the extension of the file type chosen in the dialog. Every format refuses to overwrite an existing file.

| Extension | Format | Contents |
| --- | --- | --- |
| `.spe` | SPE 3.0 (2.2 for 32-bit accumulations) | Frames and the acquisition settings, readable by WinSpec, LightField and `openst133.spe` |
| `.h5`, `.hdf5` | HDF5 | Dataset `frames` (frame, row, column) in its original integer type, the settings as file attributes, and `sensor_column` for spectra |
| `.tif`, `.tiff` | TIFF | A 16-bit (or 32-bit) image stack, with the settings as JSON in the image description |
| `.csv` | CSV | Spectra only: one `sensor_column` column and one column per frame, with the settings as `#` comment lines |

Every format records the same acquisition metadata: the effective exposure the camera used (a request that cannot be encoded exactly, such as 10 s, is stored as the 9.984 s actually applied), the requested exposure as typed, readout region and binning, gain, shutter, readout rate, accumulations, detector, setpoint, the sensor temperature when the acquisition started and the software version. `openst133 acquire --out` accepts the same extensions. HDF5 uses `h5py`, which installs with openst133 on every Python version that has an `h5py` build; on a newer Python, saving `.h5` asks for TIFF or SPE instead.

## Hardware validation

The Tk viewer has been exercised with the supported USB camera on Linux. Individual controls have not been separately validated; see the [feature support matrix](support-matrix.md).
