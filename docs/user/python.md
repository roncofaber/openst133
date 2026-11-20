# Python

Use the `openst133` package for USB discovery, SPE files, camera acquisition and simulation:

```python
import openst133

for device in openst133.list_devices():
    print(f"{device.bus_number:03d}:{device.device_address:03d} {device.vendor_id:04x}:{device.product_id:04x} {device.port_path or '-'}")
```

Install the package as described in [Install](install.md). It wraps the native library with `ctypes` and searches in this order: an explicit `library_path`, `OPENST133_LIBRARY`, the bundled copy and the system loader. See the [feature support matrix](support-matrix.md) for API and validation limits.

## Discover interfaces

The discovery example above prints entries such as `001:012 0bd7:a010 1-4.2`. `openst133 list` prints the same fields. Discovery sends no camera commands. Entries are sorted by bus and USB port path, so their order is stable while cameras remain in the same sockets.

`openst133.select_device(devices, selector)` selects by zero-based index, port path (`"1-4.2"`) or bus and address (`"1:12"`), using the rules in [Choosing a camera](cli.md#choosing-a-camera).

## Read and write SPE files

```python
from openst133 import spe

info, frames = spe.read("first-frame.spe")
print(info.width, info.height, info.frame_count)
spe.create("copy.spe", frames, info.exposure_seconds)
```

Frames are NumPy arrays shaped `(height, width)`. `spe.SpeFile(path)` provides context-managed access to metadata, single frames, the raw header, the SPE 3.0 XML footer and lossless `write(path)`. Writers refuse to overwrite an existing file. Pixel values are not calibrated.

## Acquire from the camera

`RealCamera()` accepts every setting within its allowed range. Complete the [safety checklist](safety.md) and prepare the [startup data](startup-data.md) before this hardware example:

```sh
python - <<'PY'
from openst133 import spe
from openst133.camera import RealCamera

with RealCamera() as camera:
    camera.set_exposure(0.1)
    camera.set_shutter(True)
    frame = camera.acquire()
    spe.create("python-dark.spe", [frame], camera.effective_exposure)
PY
```

With several cameras attached, pass `RealCamera(selector="1-4.2")`, an index or `BUS:ADDR`, or pass `device=` with a `Device` from `list_devices()`. Without a selector or device, `open()` raises `CameraUnavailable` listing the choices rather than opening one arbitrarily.

`acquire()` returns a `uint16` array shaped by the current readout, `(100, 1340)` for full frame. `set_shutter(True)` selects Disabled Closed and `False` selects Normal. `effective_exposure` is the requested duration after driver encoding, in seconds; it is not an independent measurement of physical integration time. The Python API rounds up to whole milliseconds; see [Settings](#settings). Unsupported requests raise `CameraUnavailable` with the library's reason.

The module command provides single or repeated acquisitions from the shell:

```sh
openst133 acquire --exposure 0.1 --out python-frame.spe
```

Use `--frames N` to repeat acquisition and save all frames. The `--out` extension selects SPE, HDF5, TIFF or CSV; from Python, `openst133.export.save_frames(path, frames, exposure_seconds, metadata)` does the same ([Saving](viewer.md#saving)). Pass `openst133.export.effective_exposure(camera)` as the exposure and `openst133.export.acquisition_metadata(camera)`, taken before acquiring, as the metadata; the viewer and the ScopeFoundry plugin use the same two functions.

## Settings

| Method | Effect |
| --- | --- |
| `set_exposure(seconds)` | Positive seconds; the Python wrapper rounds up to whole milliseconds before exposure encoding |
| `set_shutter(mode)` | `"normal"`, `"closed"`, `"opened"` or `"preopen"` (refused); `True` is `"closed"`, `False` is `"normal"` |
| `set_gain(gain)` | Controller gain 1, 2 or 3 |
| `set_accumulations(count)` | Frames read and summed per `acquire()` (1, 3 or 10); above 1, `acquire()` returns `int32` sums |
| `spe_metadata()` | SPE metadata fields for current settings, including gain |
| `set_temperature(celsius)` / `temperature()` | Request a target / read temperature ([Cooling](cooling.md)) |
| `set_roi(x, y, width, height)` | Region of interest, keeping the current binning |
| `set_binning(x_binning, y_binning)` | Binning within the current region |
| `set_full_vertical_binning()` | Full vertical binning |
| `set_readout(x, y, width, height, x_binning=1, y_binning=1)` | Region and binning in one call; `y_binning=height` sums the region's rows into one spectrum |
| `set_readout_rate(rate_hz)` | `1000000` or `100000` |
| `abort()` | Stop a running acquisition |

See [Readout modes and ROI](modes.md) for the accepted combinations.

## Stream

```python
for frame in camera.stream(frames=20):
    ...
```

`stream()` without `frames` runs until its iterator is closed or `abort()` is called. Streaming supports every supported geometry, but accumulations above one cannot be streamed. If the consumer falls behind, the oldest queued frame is dropped and counted in `camera.stream_dropped`; `camera.stream_stats` holds native counters after the stream ends. See [Stream frames](stream.md).

## Simulated camera

`SimulatedCamera` from `openst133.camera` implements the same interface without hardware:

```sh
openst133 acquire --simulate --exposure 0.1 --out simulated-frame.spe
```

`openst133 view --simulate` uses the same simulated camera in the viewer.

## Hardware validation

The viewer has been exercised with the supported camera on Linux. Other direct Python camera operations have not been validated on hardware; see the [feature support matrix](support-matrix.md).
