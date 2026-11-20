# Command reference

Run `ost --help`, `ost COMMAND --help` or `openst133 COMMAND --help` to display the command-line options summarized here.

`ost` is the native C command line. `openst133` runs the Python tools and is equivalent to `python -m openst133`.

## `ost`

`list`, `info`, `spe-info` and `spe-dump` send no camera commands. `temperature` and `acquire` send camera traffic.

### `ost list [--json]`

List each ST-133 interface as vendor ID, product ID, bus, address and USB port path (`-` when unknown), without opening it. Lines are sorted by bus and port path, so their order stays the same while cameras remain in the same sockets; the line number, counted from 0, is the `--camera` index.

```text
0x0bd7 0xa010 1 12 1-4.2
```

### Choosing a camera

`info`, `temperature` and `acquire` use the only attached ST-133. With several attached, they refuse to guess and list the choices; pass `--camera` with one of:

| Selector | Example | Meaning |
| --- | --- | --- |
| `INDEX` | `--camera 1` | Line number in `ost list`, from 0 |
| `BUS-PORT[.PORT]` | `--camera 1-4.2` | USB port path: the physical socket, kept across reconnection |
| `BUS:ADDR` | `--camera 1:12` | Bus and current address; the address changes after reconnection |

openst133 does not name cameras by serial number, so the port path is the stable way to select one camera: keep each camera in the same socket. The target saved by `--set-target` is stored per port path, or per bus and address when the path is unknown, so it follows the socket if cameras are swapped.

### `ost info [--json] [--camera SELECTOR]`

Open the interface and print its cached descriptor information.

```text
vendor id: 0x0bd7
product id: 0xa010
bus: 1
address: 12
port path: 1-4.2
```

### `ost spe-info [--json] FILE`

Print normalized SPE metadata:

```text
major version: 3
header version: 3
data type: uint16
width: 1340
height: 100
frames: 1
regions: 1
pixels per frame: 134000
bytes per pixel: 2
exposure seconds: 0.0010000000474974513
date: 17Nov2026
software version: openst133 0.1.0
```

### `ost spe-dump [--json] [--frame INDEX] FILE`

Print one frame as `index,value` CSV, or JSON. `INDEX` is zero-based and defaults to 0.

### `ost temperature`

Open the ST-133, identify it and print its temperature. See [Cooling](cooling.md).

| Option | Effect |
| --- | --- |
| `--open-target CELSIUS` | Override the target written at open, from -152 to -25 C |
| `--set-target CELSIUS` | Write and save the target after the temperature read |
| `--json` | Machine-readable output |
| `--camera SELECTOR` | Camera to use when several are attached ([Choosing a camera](#choosing-a-camera)) |

### `ost acquire`

Acquire from the ST-133 and save SPE. Requires `--exposure` and `--out`. See [Acquire frames](acquire.md), [Stream frames](stream.md) and [Readout modes and ROI](modes.md).

| Option | Effect |
| --- | --- |
| `--exposure SECONDS` | Exposure, rounded to the nearest millisecond |
| `--out FILE.spe` | Output file |
| `--strict-exposure` | Refuse an exposure that would be shortened |
| `--count N` | N frames in one SPE, default 1 |
| `--stream` | Stream frames; with `--count N` save N, without it run until Ctrl-C and save the last |
| `--closed` | Disabled Closed shutter instead of Normal, equivalent to `--shutter closed` |
| `--shutter MODE` | `normal`, `closed`, `opened` (Disabled Opened) or `preopen` (refused because External Sync is unsupported) |
| `--gain N` | Controller gain 1, 2 or 3 |
| `--accumulations N` | Frames to read and sum into one 32-bit LONG SPE frame |
| `--setpoint C` | Requested target in degrees Celsius |
| `--fvb` | Full vertical binning of the region, or of the whole sensor without `--roi`; add `--bin Nx1` for horizontal binning |
| `--roi X,Y,W,H` | Region of interest in sensor pixels |
| `--bin XxY` | Horizontal and vertical binning; with `--fvb` only `Nx1` or `Nx<rows>` |
| `--rate 1M\|100k` | Readout rate |
| `--camera SELECTOR` | Camera to use when several are attached ([Choosing a camera](#choosing-a-camera)) |

## `openst133`

| Command | Options | Effect |
| --- | --- | --- |
| `openst133 list` | | List interfaces as `bus:address vid:pid port-path`, for example `001:012 0bd7:a010 1-4.2`, in `--camera` index order |
| `openst133 view` | `--simulate`, `--camera SELECTOR`, `--close-after N`, `--log-level`, `--log-file` | Open the [viewer](viewer.md) |
| `openst133 acquire` | `--simulate`, `--camera SELECTOR`, `--exposure S`, `--frames N`, `--out FILE`, `--log-level`, `--log-file` | Acquire and save as SPE, HDF5, TIFF or CSV (spectra), chosen by the `--out` extension |
| `openst133 startup-data [DIR]` | `--dat FILE`, `--dll FILE`, `--detector 98`, `--output FILE` | Build startup data from `PI133B.DAT` and `PICHIPDB.dll`, given as a folder or individually ([Startup data](startup-data.md)) |
| `openst133 setup [DIR]` | `--dat FILE`, `--dll FILE`, `--skip-udev` | Guided first-time setup: finds the camera, installs the udev rule on Linux, confirms USB access without sending camera commands and builds startup data |
| `openst133 setup-udev` | `--print` | Linux: install the udev rule for non-root camera access, using sudo once |
| `openst133 check` | `--simulate` or `--hardware`, `--output DIR`, `--flats`, `--exposures`, `--setpoint C` (default -120, persisted), `--seed`, `--log-level`, `--log-file` | Run the driver check and characterize the camera ([Hardware check](hardware-check.md)) |
| `openst133 temperature-log` | `--simulate` or `--hardware`, `--setpoint C`, `--interval S`, `--count N`, `--lock-tolerance`, `--lock-hold`, `--wait-ready`, `--settle-time`, `--plot`, `--output FILE` | Log a cooldown to CSV ([Cooling](cooling.md#log-a-cooldown)) |

`--camera` takes the selectors in [Choosing a camera](#choosing-a-camera) and is required with several cameras. `--log-level` and `--log-file` are described in [Logging](logging.md).

