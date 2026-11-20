# Other control stacks

Run `python examples/acquire.py` and the other `examples/` scripts to put openst133 behind a plain script, a Bluesky/ophyd detector, a PyMoDAQ 2D detector or a foreign-language C call. The examples use the Python package and `SimulatedCamera`; Bluesky, ophyd and PyMoDAQ are not openst133 dependencies. Install optional frameworks in your own environment. Keep `OPENST133_NO_HARDWARE=1` in their environment while testing simulation.

## Plain acquisition

`examples/acquire.py` opens one camera, sets a 10 ms exposure, acquires one frame and writes it. Use `.npy` for a NumPy file or `.spe` for SPE through `openst133.spe.create`:

```sh
OPENST133_NO_HARDWARE=1 python examples/acquire.py frame.npy
```

The required surface is `open()`/`close()`, `set_exposure(seconds)` and `acquire()`, which returns a row-major `(height, width)` NumPy array. Use the object as a context manager to guarantee `close()`. The SPE branch also reads the effective exposure so the file records the requested duration after driver encoding; this is not an independent measurement of physical integration time. Replace `SimulatedCamera` with `RealCamera` ([Acquire from the camera](python.md#acquire-from-the-camera)) and prepare its [startup data](startup-data.md) only when moving to hardware.

## Bluesky and ophyd

`examples/bluesky_detector.py` defines `OpenST133Detector`, a plain object implementing Bluesky's `name`, `trigger`, `read` and `describe` protocols. It does not need to inherit an ophyd `Device`. `trigger()` submits `camera.acquire()` to a single-worker executor and returns `ophyd.DeviceStatus`; the future stores the frame before completing or failing that status. `read()` publishes `st133_image` as a NumPy value with a Unix timestamp, and `describe()` publishes the same key with `source: openst133`, `dtype: array`, shape and NumPy dtype.

```python
from bluesky import RunEngine
from bluesky.plans import count
from bluesky_detector import OpenST133Detector

RE = RunEngine()
with OpenST133Detector(name="st133") as detector:
    RE(count([detector], num=3))
```

The openst133 calls it needs are `open`, `set_exposure`, blocking `acquire`, `abort` and `close`. A trigger refuses to overlap the previous one. `close()` aborts, joins the executor and closes the camera. Passing a configured `RealCamera` instead of the default simulated camera leaves event-document handling unchanged. Write document storage and visualization subscriptions separately; a `RunEngine` alone does not save data. Ophyd's [`DeviceStatus`](https://blueskyproject.io/ophyd/user_v1/explanations/status.html) and Bluesky's [hardware interface](https://blueskyproject.io/bluesky/main/hardware.html) describe those contracts.

## PyMoDAQ

`examples/pymodaq_detector.py` contains the synchronous `DAQ_2DViewer_OpenST133` skeleton. Its `camera_factory` defaults to `SimulatedCamera`; change that attribute to `RealCamera` for a hardware installation. For an instrument plugin, place it according to PyMoDAQ's `daq_2Dviewer_OpenST133.py` and `DAQ_2DViewer_OpenST133` naming convention rather than treating this standalone example as an installed package.

| Plugin call | openst133 behavior |
| --- | --- |
| `ini_detector(controller)` | Master opens `camera_factory()`; slave uses the shared controller. Both apply exposure and shutter |
| `commit_settings(param)` | `set_exposure()` for `exposure_s`, `set_shutter()` for `shutter` |
| `grab_data(Naverage)` | One blocking `acquire()` per average; mean of frames for averages above one |
| Completed acquisition | `dte_signal.emit(DataToExport(... DataFromPlugins(dim="Data2D") ...))` |
| `stop()` | `abort()` |
| `close()` | Master calls `abort()` and `close()`; every instance releases its controller reference without a slave closing shared hardware |

This skeleton uses synchronous acquisition, so do not call `grab_data()` on a GUI thread for long exposures. It intentionally provides no timers or live mode; implement asynchronous framework behavior in your application rather than blocking its GUI thread. PyMoDAQ's [instrument plugin guide](https://pymodaq.cnrs.fr/en/latest/developer_folder/instrument_plugins.html) describes controller sharing, settings and data export; the example keeps those framework objects optional so its openst133 side can be tested without the GUI stack.

## Calling the C library from another language

Load the shared library and declare every signature from `include/openst133.h` and `include/openst133_spe.h`. Do not reuse a status string for a different API family: `ost_camera_*` and `ost_spe_*` return different status enums. Pointer sizes and structure layouts must match the bitness and C ABI of the loaded library.

`examples/ctypes_spe.py` declares `ost_spe_create` with `ctypes`, creates a simulated frame, keeps its contiguous little-endian `uint16` buffer alive across the call and writes SPE through the C library. Set `OPENST133_LIBRARY` to a built `libopenst133.so`, `libopenst133.dylib` or `openst133.dll`, or pass `--library`:

```sh
OPENST133_NO_HARDWARE=1 OPENST133_LIBRARY=/path/to/libopenst133.so python examples/ctypes_spe.py frame.spe
```

The simulated frame comes from the Python camera; `ost_spe_create` is the actual C call. The installed C library has no simulated camera backend that can be selected at run time; native tests use mock and replay transports instead. A hardware acquisition uses these additional public C calls:

| Step | C surface |
| --- | --- |
| Discover and choose | `ost_list_devices(NULL, &devices, &count)`, `ost_select_device`, then `ost_free_devices` after opening |
| Open | `ost_camera_open(NULL, &info, &camera)` runs the camera startup; `ost_open` and `ost_close` check USB access without camera commands |
| Configure | `ost_camera_set_exposure(camera, milliseconds)`, optionally shutter/readout setters |
| Size the buffer | `ost_camera_get_readout`; `(width/x_binning) * (height/y_binning) * 2` bytes for one ordinary frame |
| Acquire | `ost_camera_acquire(camera, buffer, capacity, &acquired_size)`; use 4 bytes per accumulated pixel when accumulations exceed one |
| Save and clean up | `ost_spe_create` or its metadata variant, then `ost_camera_close` |

Treat every nonzero return as an error. Call `ost_camera_last_error(camera)` on the same thread and before the next camera call, or `ost_camera_open_error()` after a failed open. `ost_spe_create` does not take ownership of the pixel buffer. The array returned by `ost_list_devices` belongs to the C allocator and must be released by `ost_free_devices`.

### LabVIEW Call Library Function note

Match the LabVIEW process bitness to `openst133.dll`; a 64-bit LabVIEW cannot load a 32-bit DLL or vice versa. On the Call Library Function Node, select the DLL, enter the exported `ost_*` name and use **C** calling convention. Map `int` returns to signed 32-bit integers, `size_t` and pointers to the pointer-sized integer for that process, and `double` to DBL. For output scalars such as `size_t *acquired_size`, pass a pointer to value. For `ost_camera_acquire`, preallocate a U16 array for ordinary frames or I32 for accumulated frames, size it from the current readout and pass its array data pointer, not a LabVIEW array handle. Retain clusters for `ost_device_info`, `ost_camera_readout` and `ost_sensor_geometry` in the exact public C field order. Let `ost_free_devices` and `ost_camera_close` release C-owned resources; do not free those pointers with LabVIEW. See NI's [Call Library Function Node configuration](https://www.ni.com/docs/en-US/bundle/labview/page/configuring-the-call-library-function-node-dialog-box.html) and [node details](https://www.ni.com/docs/en-US/bundle/labview/page/call-library-function-node-details.html).

`OPENST133_NO_HARDWARE=1` forbids the C library's real USB backend and there is no native simulated LabVIEW camera. Use the Python examples for the simulated acquisition tests, and make the hardware and startup-data requirements explicit in any LabVIEW wrapper. No LabVIEW VI is supplied or tested here.

## Verification without the frameworks

The focused test suite uses small simulated frames, an ophyd status stand-in, PyMoDAQ data/signal stand-ins and a mock C SPE function:

```sh
OPENST133_NO_HARDWARE=1 PYTHONPATH="$PWD/python/src:$PWD/examples" python3 -m unittest discover -s examples/tests -v
```

It checks the NPY round trip, Bluesky protocol documents and asynchronous completion, PyMoDAQ configuration/data/stop/close flow, ctypes argument layout and native error propagation. Passing those tests does not establish compatibility with a particular framework release or test a real camera.
