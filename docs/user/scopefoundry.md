# ScopeFoundry

Add openst133 hardware control and acquisition to a [ScopeFoundry](https://www.scopefoundry.org) app with the `openst133-scopefoundry` package in `integrations/scopefoundry/`.

## Install

The plugin is not on PyPI; install it from the source. Use Python 3.13 or 3.14: ScopeFoundry 2.3 needs 3.13 or newer, and its `h5py` dependency has no 3.15 build yet. Clone the repository and create an environment; these examples use `uv` and `.venv-scopefoundry` (`python3.13 -m venv .venv-scopefoundry` works too):

```sh
git clone https://github.com/roncofaber/openst133
cd openst133
uv venv --python 3.13 --seed .venv-scopefoundry
.venv-scopefoundry/bin/python -m pip install . './integrations/scopefoundry[gui,test]'
```

The package installs as `ScopeFoundryHW.openst133`. Its `gui` extra adds PySide6, pyqtgraph and matplotlib; `test` adds pytest. Both packages also work as editable installs (`pip install -e`). Then run the included demo app, on the simulated camera by default:

```sh
.venv-scopefoundry/bin/python integrations/scopefoundry/examples/simulated_app.py
```

## Add it to an app

```python
import sys

from ScopeFoundry import BaseMicroscopeApp
from ScopeFoundryHW.openst133 import OpenST133HW, OpenST133Measure, close_app

class App(BaseMicroscopeApp):
    name = "my_app"
    def setup(self):
        self.add_hardware(OpenST133HW(self))
        self.add_measurement(OpenST133Measure(self))

def run(app):
    try:
        return app.exec_()
    finally:
        close_app(app)

if __name__ == "__main__":
    sys.exit(run(App(sys.argv)))
```

`close_app` stops running measurements, disconnects the camera and deletes the main window before Python exits. Without it, ScopeFoundry can print an ignored `RuntimeError` traceback about deleted Qt objects during shutdown.

Tick `connected` on the openst133 hardware, or press **Acquire** or **Continuous**, which connect it first. A camera that cannot be opened (busy, missing permission or startup data) unticks `connected`, logs one line with the reason and shows it in the panel's status line. The documented install omits IPython, so ScopeFoundry warns that it cannot import the IPython console and falls back to `pyqtgraph.console`; this warning does not prevent simulated acquisition or saving.

## Use the real camera

Complete the [safety checklist](safety.md), prepare the [startup data](startup-data.md) and close applications and VMs that hold the camera. Then run:

```sh
.venv-scopefoundry/bin/python integrations/scopefoundry/examples/simulated_app.py --real
```

`--real` clears the `simulate` setting before connecting, and ticking `connected` sends hardware commands. On connect, `temperature_setpoint` shows the target the camera opened with instead of overwriting it, and the other camera settings are sent. Unsupported settings are disabled with their reason as a tooltip and listed in `unavailable`.

The example app has been run with the supported camera on Linux (connect, live view, temperature); see the [feature support matrix](support-matrix.md).

## Settings

| Setting | Meaning |
| --- | --- |
| `simulate` | Simulated camera (default) or USB; set before connecting |
| `exposure` | Seconds; the [exposure rules](acquire.md#exposure) apply |
| `temperature` | Read once per second while connected |
| `temperature_setpoint` | Target, -152 to -25 C in 0.5 C steps ([Cooling](cooling.md)); on the simulator it retargets the cooling curve |
| `shutter` | `normal`, `closed` (Disabled Closed) or `opened` (Disabled Opened) |
| `gain` | Controller gain 1, 2 or 3 |
| `accumulations` | 1, 3 or 10 frames summed into one 32-bit frame; above 1, `single` mode only |
| `readout_rate` | `1 MHz` or `100 kHz` (full unbinned frame only) |
| `roi_x0`, `roi_x1`, `roi_y0`, `roi_y1` | Region of interest, end exclusive |
| `bin_x`, `bin_y` | Binning; the ROI size must divide by the binning factors |
| `full_vertical_binning` | Sum every row of the ROI into one spectrum; `bin_x` still applies and `bin_y` is ignored |
| `unavailable` | Operations the driver does not support |

See [Readout modes and ROI](modes.md). The plugin does not crop or bin on the host.

## Measurement panel

The `openst133_acquire` panel has the same controls as the [viewer](viewer.md): **Exposure (s)**, **Acquire**, **Continuous**, **Stop** and **Save...**; shutter, gain, accumulations and readout; setpoint, temperature and colormap; **Full sensor** or **Custom** region with x, y, w and h; bin x and y, whose arrows step only through factors that divide the region; **Sum rows (FVB)**; and an output-size preview that turns red and names the field to fix when the values do not fit. Arrow steps apply at once and typed values apply after a short pause.

Region dragging works as in the [viewer](viewer.md#controls): drag inside the cyan window to move it, a corner or edge handle to resize it, or outside it to draw a new one; the pointer may leave the image and the window stops at the sensor edge. Double-click to return to the full sensor. FVB readouts appear as a spectrum against sensor column. During **Continuous**, a changed setting restarts the stream with it. **Acquire** takes one frame and saves it according to `save_h5` and `save_spe`; **Save...** writes the last frame as SPE, HDF5, TIFF or CSV, chosen by extension.

## Measurement

`openst133_acquire` has two modes:

- `live`: stream and display the latest frame; nothing is saved unless you press **Save...** in the panel or run the `Save last frame` operation.
- `single`: acquire `n_frames` and save them.

The output is a ScopeFoundry H5 file, named by the app's `data_fname_format` in `save_dir`, with `/app`, `/hardware/openst133` and `/measurement/openst133_acquire` groups and all settings as attributes. It also contains:

| Item | Shape | Content |
| --- | --- | --- |
| `frames` | (n_frames, rows, columns) | uint16 frames (int32 with accumulations); FVB has one row |
| `spectra` | (n_frames, columns) | Frames summed over rows |
| `temperature` | (n_frames) | Temperature per frame in C; NaN when not read |
| `frame_time_utc` | (n_frames) | Unix time when each frame arrived |
| `sensor_column`, `sensor_row` | (columns), (rows) | First sensor column or row of each output column or row |
| `wavelength` | (columns) | Only when `wavelength_step` is nonzero: start + step * column |

Each dataset carries `dimensions` and, where it applies, `unit` and `description` attributes. The measurement group's attributes are the acquisition record, the same fields every openst133 front end saves ([Saving](viewer.md#saving)): `exposure_seconds` (also `exposure_time`) is the effective exposure the camera used and `requested_exposure_seconds` the value you asked for, plus readout, gain, shutter, rate, accumulations, detector, setpoint, mean temperature, acquisition time and software version. The `/hardware` settings are the values when the file was written, so for a frame saved later the measurement attributes are the ones that describe it.

`save_spe` also writes an SPE file with the same base name and is off by default.

## Tests without hardware

From the repository root:

```sh
OPENST133_NO_HARDWARE=1 QT_QPA_PLATFORM=offscreen .venv-scopefoundry/bin/python -I -m pytest -q integrations/scopefoundry/tests
```

The tests use the simulated camera and a fake real camera, with Qt offscreen and the hardware guard enabled. They do not establish compatibility with a real camera or every ScopeFoundry release.
