# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT

"""Camera viewer: display, acquisition and SPE saving logic, with a thin Tkinter shell."""

import queue
import sys
import threading
import xml.etree.ElementTree as element_tree

import numpy

from ._log import viewer as logger
from .camera import AcquisitionAborted, CameraError, CameraUnavailable, make_camera
from .export import FILE_TYPES, acquisition_metadata, effective_exposure, save_frames, software_version, with_extension
from .spe import SpeFile, create as write_spe


def display_range(frame, low_percentile=1.0, high_percentile=99.8):
    low, high = numpy.percentile(frame, (low_percentile, high_percentile))
    if high <= low:
        high = low + 1.0
    return float(low), float(high)


def _heat(level):
    red = numpy.clip(level * 3.0, 0, 1)
    green = numpy.clip(level * 3.0 - 1.0, 0, 1)
    blue = numpy.clip(level * 3.0 - 2.0, 0, 1)
    return numpy.stack([red, green, blue], axis=-1)


READOUT_RATES = {"1 MHz": 1000000, "100 kHz": 100000, "1M": 1000000, "100k": 100000}

def _anchored(*colors):
    anchors = numpy.asarray(colors, dtype=numpy.float64) / 255.0
    positions = numpy.linspace(0.0, 1.0, len(colors))

    def colormap(level):
        return numpy.stack([numpy.interp(level, positions, anchors[:, channel]) for channel in range(3)], axis=-1)

    return colormap


COLORMAPS = {
    "gray": lambda level: numpy.repeat(level[..., None], 3, axis=-1),
    "heat": _heat,
    "viridis": _anchored(
        (68, 1, 84), (71, 44, 122), (59, 81, 139), (44, 113, 142), (33, 144, 141),
        (39, 173, 129), (92, 200, 99), (170, 220, 50), (253, 231, 37),
    ),
    "inferno": _anchored(
        (0, 0, 4), (31, 12, 72), (85, 15, 109), (136, 34, 106), (186, 54, 85),
        (227, 89, 51), (249, 140, 10), (249, 201, 50), (252, 255, 164),
    ),
    "magma": _anchored(
        (0, 0, 4), (28, 16, 68), (79, 18, 123), (129, 37, 129), (181, 54, 122),
        (229, 80, 100), (251, 135, 97), (254, 194, 135), (252, 253, 191),
    ),
}
PROJECT_URL = "https://github.com/roncofaber/openst133"


def divisors(number):
    return [factor for factor in range(1, number + 1) if number % factor == 0]


def preview_spectrum(frame, frame_geometry, geometry):
    """Sum the rows of an image frame in software to preview an FVB readout of geometry, or None when the frame does
    not cover that region at a compatible binning. On-chip FVB adds the bias once rather than once per row."""
    fx, fy, fwidth, fheight, fx_binning, fy_binning = frame_geometry
    x, y, width, height, x_binning, _ = geometry
    covered = fx <= x and x + width <= fx + fwidth and fy <= y and y + height <= fy + fheight
    aligned = (x - fx) % fx_binning == 0 and x_binning % fx_binning == 0 and (y - fy) % fy_binning == 0
    if not (covered and aligned and height % fy_binning == 0):
        return None
    rows = numpy.asarray(frame, dtype=numpy.float64)[
        (y - fy) // fy_binning:(y - fy + height) // fy_binning, (x - fx) // fx_binning:(x - fx + width) // fx_binning
    ].sum(axis=0)
    return rows.reshape(-1, x_binning // fx_binning).sum(axis=1)[None, :]


def readout_geometry(sensor_width, sensor_height, custom, roi, binning, fvb):
    """Return (x, y, width, height, x_binning, y_binning) for the viewer's readout controls.

    Raises ValueError naming the field to fix, so nothing reaches the camera until the combination is valid.
    """
    if custom:
        try:
            x, y, width, height = (int(str(value).strip()) for value in roi)
        except ValueError:
            raise ValueError("region x, y, w and h must be whole numbers") from None
        if x < 0 or y < 0 or width <= 0 or height <= 0:
            raise ValueError("region needs x, y >= 0 and w, h > 0")
        if x + width > sensor_width:
            raise ValueError(f"x + w = {x + width} exceeds the sensor width {sensor_width}")
        if y + height > sensor_height:
            raise ValueError(f"y + h = {y + height} exceeds the sensor height {sensor_height}")
    else:
        x, y, width, height = 0, 0, sensor_width, sensor_height
    fields = binning[:1] if fvb else binning
    try:
        factors = [int(str(value).strip()) for value in fields]
    except ValueError:
        raise ValueError("bin x and y must be whole numbers") from None
    if any(factor <= 0 for factor in factors):
        raise ValueError("bin x and y must be at least 1")
    x_binning = factors[0]
    y_binning = height if fvb else factors[1]
    if width % x_binning:
        raise ValueError(f"w = {width} is not divisible by bin x = {x_binning}")
    if height % y_binning:
        raise ValueError(f"h = {height} is not divisible by bin y = {y_binning}")
    return x, y, width, height, x_binning, y_binning


def describe_readout(geometry):
    _, _, width, height, x_binning, y_binning = geometry
    columns, rows = width // x_binning, height // y_binning
    return f"{columns} x {rows} spectrum" if rows == 1 else f"{columns} x {rows} image"


def to_rgb(frame, vmin=None, vmax=None, colormap="gray"):
    if vmin is None or vmax is None:
        vmin, vmax = display_range(frame)
    level = numpy.clip((numpy.asarray(frame, dtype=numpy.float64) - vmin) / (vmax - vmin), 0.0, 1.0)
    return (COLORMAPS[colormap](level) * 255.0 + 0.5).astype(numpy.uint8)


def to_ppm(rgb):
    height, width, _ = rgb.shape
    return b"P6 %d %d 255\n" % (width, height) + rgb.tobytes()


OUTSIDE_READOUT = (32, 32, 32)


def sensor_view(rgb, geometry, sensor_width, sensor_height, view_width, view_height):
    """Draw a readout's RGB pixels where they sit on the sensor, each stretched over its bin, on a whole-sensor view.

    View pixels outside the readout region are dark grey.
    """
    x, y, width, height, x_binning, y_binning = geometry
    columns = numpy.arange(view_width) * sensor_width // view_width
    rows = numpy.arange(view_height) * sensor_height // view_height
    frame_columns = numpy.clip((columns - x) // x_binning, 0, rgb.shape[1] - 1)
    frame_rows = numpy.clip((rows - y) // y_binning, 0, rgb.shape[0] - 1)
    view = rgb[frame_rows[:, None], frame_columns[None, :]]
    inside = ((rows >= y) & (rows < y + height))[:, None] & ((columns >= x) & (columns < x + width))[None, :]
    view[~inside] = OUTSIDE_READOUT
    return numpy.ascontiguousarray(view)


def frame_geometry(frame, metadata, sensor_width, sensor_height):
    """The (x, y, width, height, x_binning, y_binning) a frame was read with, or the whole sensor when unknown.

    Metadata that does not match the frame's shape is ignored, so the frame is stretched over the whole view.
    """
    try:
        x, y, width, height = (int(value) for value in metadata["roi"])
        x_binning, y_binning = (int(value) for value in metadata["binning"])
        if frame.shape == (height // y_binning, width // x_binning):
            return x, y, width, height, x_binning, y_binning
    except (KeyError, TypeError, ValueError, ZeroDivisionError):
        pass
    rows, columns = frame.shape
    return 0, 0, sensor_width, sensor_height, max(1, sensor_width // columns), max(1, sensor_height // rows)


def display_stride(width, max_width=1100):
    return max(1, -(-width // max_width))


def parse_exposure(text):
    try:
        value = float(text)
    except (TypeError, ValueError):
        raise ValueError(f"not a number: {text!r}") from None
    if not value > 0.0 or value == float("inf"):
        raise ValueError("exposure must be a positive number of seconds")
    return value


def format_temperature(celsius):
    return f"{celsius:+.1f} C"


def _local_name(element):
    return element.tag.rsplit("}", 1)[-1]


def _wavelength_mapping(xml_footer, pixel_count):
    fallback = numpy.arange(pixel_count, dtype=numpy.float64)
    try:
        root = element_tree.fromstring(xml_footer)
        calibration_ids = {
            calibration_id
            for block in root.iter()
            if _local_name(block) == "DataBlock"
            for calibration_id in block.attrib.get("calibrations", "").split()
        }
        for mapping in root.iter():
            if (
                _local_name(mapping) != "WavelengthMapping"
                or mapping.attrib.get("id") not in calibration_ids
            ):
                continue
            for child in mapping:
                if _local_name(child) == "Wavelength":
                    values = numpy.asarray([float(value) for value in child.text.split(",")])
                    if len(values) == pixel_count and numpy.isfinite(values).all():
                        return values, "nm"
    except (AttributeError, TypeError, ValueError, element_tree.ParseError):
        pass
    return fallback, "pixel"


def spectrum_axis(source, pixel_count=None):
    if isinstance(source, SpeFile):
        count = source.info.width if pixel_count is None else pixel_count
        return _wavelength_mapping(source.xml_footer, count)
    with SpeFile(source) as spe_file:
        count = spe_file.info.width if pixel_count is None else pixel_count
        return _wavelength_mapping(spe_file.xml_footer, count)


SPECTRUM_LEFT = 64
SPECTRUM_RIGHT_MARGIN = 20


def spectrum_x_values(count, x_axis=None, columns=None):
    """Plot x values and unit: wavelengths when a finite axis matches the spectrum, otherwise sensor columns
    (or point indexes when the columns are unknown)."""
    if x_axis is not None:
        x_values = numpy.asarray(x_axis)
        if x_values.shape == (count,) and numpy.isfinite(x_values).all():
            return x_values, "nm"
    if columns is not None and len(columns) == count:
        return numpy.asarray(columns, dtype=numpy.float64), "pixel"
    return numpy.arange(count, dtype=numpy.float64), "pixel"


def spectrum_positions(count, x_axis, width, columns=None):
    """Canvas x coordinate of every spectrum point, as draw_spectrum places them."""
    x_values, _ = spectrum_x_values(count, x_axis, columns)
    left, right = SPECTRUM_LEFT, width - SPECTRUM_RIGHT_MARGIN
    x_low = float(x_values[0])
    x_span = (float(x_values[-1]) - x_low) or 1.0
    return left + (x_values - x_low) * (right - left) / x_span


def _whole_bins(start, length, binning, limit):
    length = max(binning, -(-length // binning) * binning)
    length = min(length, limit // binning * binning)
    return max(0, min(start, limit - length)), length


def selection_region(start, end, view_size, sensor_size, binning, fvb):
    """Sensor region (x, y, width, height) for a rectangle dragged on the whole-sensor view, or None for a click.

    The region grows to whole bins and stays inside the sensor; with FVB any height is valid.
    """
    (view_width, view_height), (sensor_width, sensor_height) = view_size, sensor_size
    x0, x1 = sorted(min(max(value, 0), view_width) for value in (start[0], end[0]))
    y0, y1 = sorted(min(max(value, 0), view_height) for value in (start[1], end[1]))
    if x1 - x0 < 3 and y1 - y0 < 3:
        return None
    sensor_x0 = int(x0 * sensor_width // view_width)
    sensor_y0 = int(y0 * sensor_height // view_height)
    sensor_x1 = max(sensor_x0 + 1, -(-int(x1 * sensor_width) // view_width))
    sensor_y1 = max(sensor_y0 + 1, -(-int(y1 * sensor_height) // view_height))
    x, width = _whole_bins(sensor_x0, sensor_x1 - sensor_x0, binning[0], sensor_width)
    y, height = _whole_bins(sensor_y0, sensor_y1 - sensor_y0, 1 if fvb else binning[1], sensor_height)
    return x, y, width, height


HANDLE_MARGIN = 6
HANDLE_CURSORS = {
    "move": "fleur",
    "n": "sb_v_double_arrow",
    "s": "sb_v_double_arrow",
    "e": "sb_h_double_arrow",
    "w": "sb_h_double_arrow",
    "nw": "top_left_corner",
    "ne": "top_right_corner",
    "sw": "bottom_left_corner",
    "se": "bottom_right_corner",
    None: "crosshair",
}


def region_box(region, view_size, sensor_size):
    """View rectangle (x0, y0, x1, y1) of a sensor region on the whole-sensor view."""
    x, y, width, height = region
    (view_width, view_height), (sensor_width, sensor_height) = view_size, sensor_size
    return (
        x * view_width / sensor_width,
        y * view_height / sensor_height,
        (x + width) * view_width / sensor_width,
        (y + height) * view_height / sensor_height,
    )


def box_handle(point, box, view_size, margin=HANDLE_MARGIN):
    """The part of the region box a press grabs: a corner or edge to resize, "move" inside, None to draw a new one.

    Inside a box that covers the whole view there is nowhere to move, so a press there draws a new region.
    """
    px, py = point
    x0, y0, x1, y1 = box
    if not (x0 - margin <= px <= x1 + margin and y0 - margin <= py <= y1 + margin):
        return None
    vertical = "n" if abs(py - y0) <= margin else "s" if abs(py - y1) <= margin else ""
    horizontal = "w" if abs(px - x0) <= margin else "e" if abs(px - x1) <= margin else ""
    if vertical or horizontal:
        return vertical + horizontal
    covers_view = x0 <= 0 and y0 <= 0 and x1 >= view_size[0] and y1 >= view_size[1]
    return None if covers_view else "move"


def adjust_box(box, handle, dx, dy, view_size):
    """The region box while it is moved or resized by (dx, dy), kept inside the view."""
    x0, y0, x1, y1 = box
    view_width, view_height = view_size
    if handle == "move":
        dx = min(max(dx, -x0), view_width - x1)
        dy = min(max(dy, -y0), view_height - y1)
        return x0 + dx, y0 + dy, x1 + dx, y1 + dy
    if "w" in handle:
        x0 = min(max(x0 + dx, 0), x1 - 1)
    if "e" in handle:
        x1 = max(min(x1 + dx, view_width), x0 + 1)
    if "n" in handle:
        y0 = min(max(y0 + dy, 0), y1 - 1)
    if "s" in handle:
        y1 = max(min(y1 + dy, view_height), y0 + 1)
    return x0, y0, x1, y1


def moved_region(region, dx, dy, view_size, sensor_size):
    """Shift a sensor region by a view drag, keeping its exact size and staying inside the sensor."""
    x, y, width, height = region
    (view_width, view_height), (sensor_width, sensor_height) = view_size, sensor_size
    x = min(max(x + round(dx * sensor_width / view_width), 0), sensor_width - width)
    y = min(max(y + round(dy * sensor_height / view_height), 0), sensor_height - height)
    return x, y, width, height


def _resized_span(start, end, moved_start, moved_end, binning, limit):
    length = max(binning, round((end - start) / binning) * binning)
    length = min(length, limit // binning * binning)
    if moved_start and not moved_end:
        return _whole_bins(end - length, length, binning, limit)
    return _whole_bins(start, length, binning, limit)


def resized_region(region, handle, box, view_size, sensor_size, binning, fvb):
    """Sensor region after dragging the edges named by handle to box; untouched edges stay exactly where they were
    and the size snaps to whole bins anchored at the opposite edge."""
    x, y, width, height = region
    (view_width, view_height), (sensor_width, sensor_height) = view_size, sensor_size
    left, top, right, bottom = x, y, x + width, y + height
    if "w" in handle:
        left = round(box[0] * sensor_width / view_width)
    if "e" in handle:
        right = round(box[2] * sensor_width / view_width)
    if "n" in handle:
        top = round(box[1] * sensor_height / view_height)
    if "s" in handle:
        bottom = round(box[3] * sensor_height / view_height)
    left, right = min(max(left, 0), sensor_width - 1), min(max(right, 1), sensor_width)
    top, bottom = min(max(top, 0), sensor_height - 1), min(max(bottom, 1), sensor_height)
    right, bottom = max(right, left + 1), max(bottom, top + 1)
    x, width = _resized_span(left, right, "w" in handle, "e" in handle, binning[0], sensor_width)
    y, height = _resized_span(top, bottom, "n" in handle, "s" in handle, 1 if fvb else binning[1], sensor_height)
    return x, y, width, height


def spectrum_selection(start_x, end_x, positions, geometry):
    """Sensor region for a horizontal drag across a plotted spectrum, keeping the rows the spectrum sums."""
    if abs(end_x - start_x) < 3:
        return None
    positions = numpy.asarray(positions)
    low, high = sorted((start_x, end_x))
    first = int(numpy.abs(positions - low).argmin())
    last = int(numpy.abs(positions - high).argmin())
    x, y, _, height, x_binning, _ = geometry
    return x + first * x_binning, y, (last - first + 1) * x_binning, height


def draw_spectrum(canvas, values, x_axis=None, columns=None):
    values = numpy.asarray(values)
    if values.ndim != 1 or not len(values):
        raise ValueError("a spectrum needs a nonempty one-dimensional frame")
    width = max(640, int(canvas.cget("width")))
    height = max(320, int(canvas.cget("height")))
    canvas.config(width=width, height=height)
    x_values, unit = spectrum_x_values(len(values), x_axis, columns)
    positions = spectrum_positions(len(values), x_axis, width, columns)
    left, right, top, bottom = SPECTRUM_LEFT, width - SPECTRUM_RIGHT_MARGIN, 24, height - 46
    low, high = float(numpy.min(values)), float(numpy.max(values))
    if high <= low:
        high = low + 1.0
    x_low, x_high = float(x_values[0]), float(x_values[-1])
    x_span = (x_high - x_low) or 1.0
    stride = display_stride(len(values), (right - left) * 2)
    points = []
    for index in range(0, len(values), stride):
        x = positions[index]
        y = bottom - (float(values[index]) - low) * (bottom - top) / (high - low)
        points.extend((x, y))
    canvas.delete("all")
    canvas.create_rectangle(0, 0, width, height, fill="black", outline="")
    for tick in range(5):
        fraction = tick / 4
        x = left + fraction * (right - left)
        y = bottom - fraction * (bottom - top)
        canvas.create_line(x, top, x, bottom, fill="#303030")
        canvas.create_line(left, y, right, y, fill="#303030")
        canvas.create_text(x, bottom + 15, text=f"{x_low + fraction * x_span:.6g}", fill="#dddddd")
        canvas.create_text(left - 8, y, text=f"{low + fraction * (high - low):.6g}", anchor="e", fill="#dddddd")
    if len(points) >= 4:
        canvas.create_line(*points, fill="#8ab4f8", width=2)
    label = "Wavelength (nm)" if unit == "nm" else "Sensor column"
    canvas.create_text((left + right) / 2, height - 12, text=label, fill="#ffffff")
    return unit


class AcquisitionController:
    """Runs single or continuous acquisitions on a worker thread and reports through a queue.

    Events are tuples: ("frame", array), ("error", message), ("done", None).
    """

    def __init__(self, camera):
        self.camera = camera
        self.events = queue.Queue()
        self.last_frame = None
        self.last_frame_exposure = None
        self.last_frame_metadata = None
        self.last_exposure = None
        self._thread = None
        self._stop = threading.Event()
        self._lock = threading.Lock()

    @property
    def running(self):
        thread = self._thread
        return thread is not None and thread.is_alive()

    def set_exposure(self, seconds):
        self.camera.set_exposure(seconds)
        self.last_exposure = effective_exposure(self.camera, seconds)

    def start(self, continuous):
        with self._lock:
            if self.running:
                raise RuntimeError("an acquisition is already running")
            self._stop.clear()
            logger.debug("acquisition started mode=%s", "continuous" if continuous else "single")
            self._thread = threading.Thread(target=self._run, args=(continuous, self.last_exposure), daemon=True)
            self._thread.start()

    def stop(self):
        self._stop.set()
        self.camera.abort()

    def join(self, timeout=None):
        thread = self._thread
        if thread is not None:
            thread.join(timeout)

    def _metadata(self):
        return acquisition_metadata(self.camera)

    def _run(self, continuous, exposure):
        metadata = self._metadata()
        try:
            if continuous:
                frames = self.camera.stream()
                try:
                    for frame in frames:
                        logger.debug("acquisition frame received")
                        self.last_frame, self.last_frame_exposure = frame, exposure
                        self.last_frame_metadata = metadata
                        self.events.put(("frame", frame))
                        if self._stop.is_set():
                            break
                finally:
                    frames.close()
                    self.last_frame_metadata = self._metadata()
            else:
                frame = self.camera.acquire()
                logger.debug("acquisition frame received")
                self.last_frame, self.last_frame_exposure = frame, exposure
                self.last_frame_metadata = metadata
                self.events.put(("frame", frame))
        except AcquisitionAborted:
            logger.info("acquisition aborted")
        except CameraError as error:
            logger.error("acquisition failed: %s", error)
            self.events.put(("error", str(error)))
        finally:
            logger.debug("acquisition worker finished")
            self.events.put(("done", None))

    def drain(self):
        events = []
        while True:
            try:
                events.append(self.events.get_nowait())
            except queue.Empty:
                return events

    def save(self, path):
        if self.last_frame is None:
            raise ValueError("no frame to save yet")
        save_frames(path, [self.last_frame], self.last_frame_exposure or 0.0, metadata=self.last_frame_metadata)


def acquire_to_file(camera, exposure, path, frames=1):
    logger.debug("acquisition started exposure_s=%s frame_count=%d", exposure, frames)
    with camera:
        camera.set_exposure(exposure)
        exposure_used = effective_exposure(camera, exposure)
        metadata = acquisition_metadata(camera)
        data = [camera.acquire() for _ in range(frames)]
    save_frames(path, data, exposure_used, metadata=metadata)
    logger.info("acquisition saved frame_count=%d path=%s", len(data), path)
    return data


class ViewerApp:
    POLL_MS = 50
    TEMPERATURE_MS = 1000
    VIEW_HEIGHT = 320
    VIEW_MARGIN = 12
    MIN_VIEW_WIDTH = 670

    def __init__(self, root, camera, close_after=None):
        import tkinter
        from tkinter import filedialog, messagebox

        self._tk = tkinter
        self._filedialog = filedialog
        self._messagebox = messagebox
        self.root = root
        self.camera = camera
        self.controller = AcquisitionController(camera)
        self.frames_shown = 0
        self.colormap = tkinter.StringVar(value="gray")
        self.exposure_text = tkinter.StringVar(value="0.1")
        self.shutter_text = tkinter.StringVar(value="normal")
        self.gain_text = tkinter.StringVar(value="2")
        self.accumulations_text = tkinter.StringVar(value="1")
        self.setpoint_text = tkinter.StringVar(value="")
        self.rate_text = tkinter.StringVar(value="1 MHz")
        self.region_text = tkinter.StringVar(value="full")
        self.fvb = tkinter.BooleanVar(value=False)
        self.roi_text = [
            tkinter.StringVar(value=str(value)) for value in (0, 0, camera.width, camera.height)
        ]
        self.binning_text = [tkinter.StringVar(value="1"), tkinter.StringVar(value="1")]
        self.output_text = tkinter.StringVar(value="")
        self.temperature_text = tkinter.StringVar(value="-")
        self.status_text = tkinter.StringVar(value="idle")
        self._image = None
        self.last_display = None
        self.last_unit = None
        root.title("openst133 viewer")
        self._build()
        root.protocol("WM_DELETE_WINDOW", self.close)
        self.apply_exposure()
        self._poll()
        self._poll_temperature()
        if close_after is not None:
            root.after(int(close_after * 1000), self.close)

    def _build(self):
        """Lay the window out as a fixed grid so changing values and messages never move or resize anything."""
        tk = self._tk
        self._build_menu()
        controls = tk.Frame(self.root)
        controls.grid(row=0, column=0, sticky="w", padx=6, pady=(6, 2))

        def row(index, title):
            frame = tk.Frame(controls)
            frame.grid(row=index, column=0, sticky="w", pady=2)
            tk.Label(frame, text=title, width=12, anchor="e").pack(side=tk.LEFT, padx=(0, 4))
            return frame

        def label(parent, text, gap=12):
            tk.Label(parent, text=text).pack(side=tk.LEFT, padx=(gap, 2))

        def menu(parent, variable, values, width, command=None):
            widget = tk.OptionMenu(parent, variable, *values, command=command)
            widget.config(width=width, anchor="w")
            widget.pack(side=tk.LEFT)
            return widget

        acquisition = row(0, "Exposure (s)")
        entry = tk.Entry(acquisition, textvariable=self.exposure_text, width=8)
        entry.pack(side=tk.LEFT)
        self._bind_commit(entry, self.apply_exposure)
        self.buttons = {}
        for index, (name, command) in enumerate((
            ("Acquire", self.acquire_once),
            ("Continuous", self.acquire_continuous),
            ("Stop", self.stop),
            ("Save...", self.save),
        )):
            button = tk.Button(acquisition, text=name, command=command, width=10)
            button.pack(side=tk.LEFT, padx=(12 if index == 0 else 2, 0))
            self.buttons[name] = button
        label(acquisition, "Temperature", gap=24)
        tk.Label(acquisition, textvariable=self.temperature_text, width=9, anchor="w").pack(side=tk.LEFT)

        settings = row(1, "Shutter")
        menu(settings, self.shutter_text, ("normal", "closed", "opened"), 7, self.apply_shutter)
        label(settings, "Gain")
        menu(settings, self.gain_text, ("1", "2", "3"), 2, self.apply_gain)
        label(settings, "Accumulations")
        menu(settings, self.accumulations_text, ("1", "3", "10"), 3, self.apply_accumulations)
        label(settings, "Setpoint (C)")
        setpoint = tk.Spinbox(
            settings, textvariable=self.setpoint_text, from_=-152, to=-25, increment=0.5, width=7,
            command=self.apply_setpoint,
        )
        self.setpoint_text.set("")
        setpoint.pack(side=tk.LEFT)
        self._bind_commit(setpoint, self.apply_setpoint)
        label(settings, "Readout")
        self.rate_menu = menu(settings, self.rate_text, ("1 MHz", "100 kHz"), 7, self.apply_rate)

        readout = row(2, "Region")
        for value, text in (("full", "Full sensor"), ("custom", "Custom")):
            tk.Radiobutton(
                readout, text=text, value=value, variable=self.region_text, command=self.apply_readout,
            ).pack(side=tk.LEFT)
        self.readout_fields = {"roi": [], "bin": []}
        limits = (
            (0, self.camera.width - 1), (0, self.camera.height - 1), (1, self.camera.width), (1, self.camera.height)
        )
        for name, variable, (low, high) in zip(("x", "y", "w", "h"), self.roi_text, limits):
            label(readout, name, gap=6)
            field = tk.Spinbox(
                readout, textvariable=variable, from_=low, to=high, increment=1, width=5, command=self.apply_readout
            )
            field.pack(side=tk.LEFT)
            self._bind_commit(field, self.apply_readout)
            self.readout_fields["roi"].append(field)
        label(readout, "Bin x", gap=16)
        for name, variable in zip(("x", "y"), self.binning_text):
            if name == "y":
                label(readout, "y", gap=6)
            field = tk.Spinbox(readout, textvariable=variable, values=("1",), width=4, command=self.apply_readout)
            field.pack(side=tk.LEFT)
            self._bind_commit(field, self.apply_readout)
            self.readout_fields["bin"].append(field)
        tk.Checkbutton(readout, text="Sum rows (FVB)", variable=self.fvb, command=self.apply_readout).pack(
            side=tk.LEFT, padx=(12, 2)
        )
        self.output_label = tk.Label(readout, textvariable=self.output_text, width=44, anchor="w")
        self.output_label.pack(side=tk.LEFT, padx=(8, 0))
        for variable in (self.region_text, self.fvb, *self.roi_text, *self.binning_text):
            variable.trace_add("write", lambda *unused: self._update_readout_fields())
        self._update_readout_fields()

        controls.update_idletasks()
        self.canvas = tk.Canvas(
            self.root,
            width=max(controls.winfo_reqwidth(), self.MIN_VIEW_WIDTH + 2 * self.VIEW_MARGIN),
            height=self.VIEW_HEIGHT + 2 * self.VIEW_MARGIN,
            bg=self.root.cget("bg"),
            highlightthickness=0,
            cursor="crosshair",
        )
        self.canvas.grid(row=1, column=0, padx=6, pady=4)
        self._draw_no_frame()
        self.canvas.bind("<ButtonPress-1>", self._drag_start)
        self.canvas.bind("<B1-Motion>", self._drag_move)
        self.canvas.bind("<Motion>", self._hover)
        self.canvas.bind("<ButtonRelease-1>", self._drag_end)
        self.canvas.bind("<Double-Button-1>", lambda event: self.reset_region())
        tk.Label(self.root, textvariable=self.status_text, anchor="w", width=1).grid(
            row=2, column=0, sticky="ew", padx=6, pady=(0, 4)
        )
        self.root.update_idletasks()
        self.root.geometry(f"{self.root.winfo_reqwidth()}x{self.root.winfo_reqheight()}")
        self.root.resizable(False, False)

    def _build_menu(self):
        tk = self._tk
        bar = tk.Menu(self.root, tearoff=False)
        file_menu = tk.Menu(bar, tearoff=False)
        file_menu.add_command(label="Save frame as...", accelerator="Ctrl+S", command=self.save)
        file_menu.add_separator()
        file_menu.add_command(label="Quit", accelerator="Ctrl+Q", command=self.close)
        bar.add_cascade(label="File", menu=file_menu)
        acquisition = tk.Menu(bar, tearoff=False)
        acquisition.add_command(label="Acquire", accelerator="Ctrl+Return", command=self.acquire_once)
        acquisition.add_command(label="Continuous", accelerator="Ctrl+Shift+Return", command=self.acquire_continuous)
        acquisition.add_command(label="Stop", accelerator="Esc", command=self.stop)
        acquisition.add_separator()
        acquisition.add_command(label="Full sensor", command=self.reset_region)
        bar.add_cascade(label="Acquisition", menu=acquisition)
        view = tk.Menu(bar, tearoff=False)
        for name in COLORMAPS:
            view.add_radiobutton(label=name, value=name, variable=self.colormap)
        bar.add_cascade(label="Colors", menu=view)
        self.colormap.trace_add("write", lambda *unused: self._rerender())
        help_menu = tk.Menu(bar, tearoff=False)
        help_menu.add_command(label="Project page on GitHub", command=self.open_project_page)
        help_menu.add_command(label="About openst133", command=self.about)
        bar.add_cascade(label="Help", menu=help_menu)
        self.root.config(menu=bar)
        self.acquisition_menu = acquisition
        for sequence, action in (
            ("<Control-s>", self.save),
            ("<Control-q>", self.close),
            ("<Control-Return>", self.acquire_once),
            ("<Control-Shift-Return>", self.acquire_continuous),
            ("<Escape>", self.stop),
        ):
            self.root.bind(sequence, lambda event, action=action: action())

    def open_project_page(self):
        import webbrowser

        webbrowser.open(PROJECT_URL)

    def about(self):
        self._messagebox.showinfo(
            "About openst133",
            f"openst133 {software_version()}\n"
            "Open driver and viewer for Princeton Instruments ST-133 USB camera controllers.\n"
            f"MIT license. {PROJECT_URL}",
        )

    TYPING_PAUSE_MS = 600
    NON_EDITING_KEYS = {"Return", "KP_Enter", "Tab", "ISO_Left_Tab", "Up", "Down", "Left", "Right", "Home", "End"}

    def _bind_commit(self, widget, apply):
        """Apply a typed value on Return, on leaving the field, or once typing pauses."""
        pending = {}

        def commit(event=None):
            if pending.get("after") is not None:
                widget.after_cancel(pending.pop("after"))
            apply()

        def typed(event):
            if event.keysym in self.NON_EDITING_KEYS or event.keysym.startswith(("Shift", "Control", "Alt")):
                return
            if pending.get("after") is not None:
                widget.after_cancel(pending["after"])
            pending["after"] = widget.after(self.TYPING_PAUSE_MS, commit)

        widget.bind("<Return>", commit)
        widget.bind("<FocusOut>", commit)
        widget.bind("<KeyRelease>", typed, add="+")

    def _change(self, label, value, apply):
        """Apply a setting now; during an acquisition, queue it, stop a stream and resume it once applied.

        Queued settings are applied from _poll after the worker reports done, so the Tk thread never makes a native
        call that would wait for a running acquisition.
        """
        applied = self.__dict__.setdefault("_applied", {})
        pending = self.__dict__.setdefault("_pending", {})
        if applied.get(label) == value and label not in pending:
            return True
        if not self.controller.running:
            if apply():
                applied[label] = value
                return True
            return False
        pending[label] = (value, apply)
        if self.__dict__.get("_continuous"):
            self._resume = True
            self.controller.stop()
        self.status_text.set(f"applying {label}...")
        return True

    def _apply_pending(self):
        pending, self._pending = self.__dict__.get("_pending", {}), {}
        applied = self.__dict__.setdefault("_applied", {})
        ok = True
        for label, (value, apply) in pending.items():
            if apply():
                applied[label] = value
            else:
                ok = False
        resume, self._resume = self.__dict__.get("_resume", False), False
        if resume and ok:
            self._start(True)
        elif "buttons" in self.__dict__:
            self._update_buttons()

    def apply_exposure(self):
        return self._change("exposure", self.exposure_text.get().strip(), self._apply_exposure)

    def _apply_exposure(self):
        try:
            self.controller.set_exposure(parse_exposure(self.exposure_text.get()))
        except (ValueError, CameraError) as error:
            self.status_text.set(str(error))
            return False
        return True

    def apply_shutter(self, selected=None):
        return self._change("shutter", self.shutter_text.get(), self._apply_shutter)

    def _apply_shutter(self):
        try:
            self.camera.set_shutter(self.shutter_text.get())
        except CameraError as error:
            self.status_text.set(str(error))
            return False
        self.status_text.set(f"shutter: {self.shutter_text.get()}")
        return True

    def apply_gain(self, selected=None):
        return self._change("gain", self.gain_text.get(), self._apply_gain)

    def _apply_gain(self):
        try:
            self.camera.set_gain(int(self.gain_text.get()))
        except CameraError as error:
            self.status_text.set(str(error))
            return False
        self.status_text.set(f"gain: {self.gain_text.get()}")
        return True

    def apply_accumulations(self, selected=None):
        return self._change("accumulations", self.accumulations_text.get(), self._apply_accumulations)

    def _apply_accumulations(self):
        try:
            self.camera.set_accumulations(int(self.accumulations_text.get()))
        except CameraError as error:
            self.status_text.set(str(error))
            return False
        self.status_text.set(f"accumulations: {self.accumulations_text.get()}")
        return True

    def apply_setpoint(self):
        if not self.setpoint_text.get().strip():
            return True
        return self._change("setpoint", self.setpoint_text.get().strip(), self._apply_setpoint)

    def _apply_setpoint(self):
        try:
            self.camera.set_temperature(float(self.setpoint_text.get()))
        except (ValueError, CameraError) as error:
            self.status_text.set(str(error))
            return False
        self.status_text.set(f"setpoint: {self.setpoint_text.get()} C")
        return True

    def apply_rate(self, selected=None):
        if READOUT_RATES[self.rate_text.get()] != READOUT_RATES["1 MHz"] and "region_text" in self.__dict__:
            try:
                full = self._full_unbinned(self._geometry(), self.camera.width, self.camera.height)
            except ValueError:
                full = False
            if not full:
                self.status_text.set("100 kHz reads only the full unbinned frame: select Region Full sensor, Bin 1 x 1")
                return False
        return self._change("readout rate", self.rate_text.get(), self._apply_rate)

    def _apply_rate(self):
        try:
            self.camera.set_readout_rate(READOUT_RATES[self.rate_text.get()])
        except CameraError as error:
            self.status_text.set(str(error))
            return False
        self.status_text.set(f"readout rate: {self.rate_text.get()}")
        return True

    def _update_bin_choices(self):
        """Let the bin arrows step only through factors that divide the region's width and height."""
        try:
            custom = self.region_text.get() == "custom"
            width = int(self.roi_text[2].get()) if custom else self.camera.width
            height = int(self.roi_text[3].get()) if custom else self.camera.height
        except ValueError:
            return
        if width <= 0 or height <= 0:
            return
        for field, variable, extent in zip(self.readout_fields["bin"], self.binning_text, (width, height)):
            choices = tuple(str(factor) for factor in divisors(extent))
            if field.cget("values").split() != list(choices):
                current = variable.get()
                field.config(values=choices)
                variable.set(current)

    def _geometry(self):
        return readout_geometry(
            self.camera.width,
            self.camera.height,
            self.region_text.get() == "custom",
            [variable.get() for variable in self.roi_text],
            [variable.get() for variable in self.binning_text],
            bool(self.fvb.get()),
        )

    @staticmethod
    def _full_unbinned(geometry, width, height):
        return geometry == (0, 0, width, height, 1, 1)

    def apply_readout(self, selected=None):
        """Send region, binning and FVB together; an invalid combination is reported and nothing is sent."""
        try:
            geometry = self._geometry()
        except ValueError as error:
            self.status_text.set(str(error))
            return False
        if READOUT_RATES[self.rate_text.get()] != READOUT_RATES["1 MHz"] and not self._full_unbinned(
            geometry, self.camera.width, self.camera.height
        ):
            self.status_text.set("100 kHz reads only the full unbinned frame: select Readout 1 MHz first")
            return False
        return self._change("readout", geometry, lambda: self._apply_readout(geometry))

    def _apply_readout(self, geometry):
        try:
            self.camera.set_readout(*geometry)
        except (ValueError, CameraError) as error:
            self.status_text.set(str(error))
            return False
        self.status_text.set(f"readout: {describe_readout(geometry)}")
        self._region = geometry[:4]
        if "canvas" in self.__dict__:
            if self.controller.running:
                self._draw_region()
            else:
                self._preview_readout(geometry)
        return True

    def _update_readout_fields(self):
        """Refresh field states, the output preview and the 100 kHz entry from the current controls."""
        try:
            geometry = self._geometry()
        except ValueError as error:
            geometry = None
            preview, colour = str(error), "red"
        else:
            preview, colour = f"Output {describe_readout(geometry)}", "black"
        if "output_text" in self.__dict__:
            self.output_text.set(preview)
        widgets = self.__dict__.get("readout_fields")
        if not widgets:
            return
        tk = self._tk
        self.output_label.config(fg=colour)
        custom = self.region_text.get() == "custom"
        for field in widgets["roi"]:
            field.config(state=tk.NORMAL if custom else tk.DISABLED)
        self._update_bin_choices()
        widgets["bin"][1].config(state=tk.DISABLED if self.fvb.get() else tk.NORMAL)
        slow_allowed = geometry is not None and self._full_unbinned(geometry, self.camera.width, self.camera.height)
        self.rate_menu["menu"].entryconfig("100 kHz", state=tk.NORMAL if slow_allowed else tk.DISABLED)

    def acquire_once(self):
        self._start(False)

    def acquire_continuous(self):
        self._start(True)

    def _start(self, continuous):
        if self.controller.running or not self.apply_exposure():
            return
        self._continuous = continuous
        self.controller.start(continuous)
        self.status_text.set("acquiring (continuous)" if continuous else "acquiring")
        if "buttons" in self.__dict__:
            self._update_buttons()

    def stop(self):
        self._resume = False
        self.controller.stop()

    def save(self):
        file_type = self._tk.StringVar(value=FILE_TYPES[0][0])
        path = self._filedialog.asksaveasfilename(filetypes=FILE_TYPES, typevariable=file_type)
        if path:
            self.save_to(with_extension(path, file_type.get()))

    def save_to(self, path):
        try:
            self.controller.save(path)
        except (OSError, ValueError) as error:
            self.status_text.set(f"save failed: {error}")
        else:
            self.status_text.set(f"saved {path}")

    def _update_buttons(self):
        running = self.controller.running
        state = {True: self._tk.DISABLED, False: self._tk.NORMAL}
        for name in ("Acquire", "Continuous"):
            self.buttons[name].config(state=state[running])
        self.buttons["Stop"].config(state=state[not running])
        menu = self.__dict__.get("acquisition_menu")
        if menu is not None:
            for name in ("Acquire", "Continuous"):
                menu.entryconfig(name, state=state[running])
            menu.entryconfig("Stop", state=state[not running])

    def show(self, frame, x_axis=None):
        geometry = frame_geometry(frame, self.controller.last_frame_metadata, self.camera.width, self.camera.height)
        self._last_shown = (frame, x_axis, geometry)
        self._render(frame, x_axis, geometry)
        self.frames_shown += 1

    def _rerender(self):
        """Draw the last frame again, for example after a colormap change."""
        last = self.__dict__.get("_last_shown")
        if last is not None:
            self._render(*last)

    def _render(self, frame, x_axis, geometry):
        self.canvas.delete("all")
        if frame.ndim == 2 and frame.shape[0] == 1:
            columns = geometry[0] + numpy.arange(frame.shape[1]) * geometry[4]
            self.last_unit = draw_spectrum(self.canvas, frame[0], x_axis, columns)
            self.last_display = "spectrum"
            self._image = None
            positions = spectrum_positions(frame.shape[1], x_axis, int(self.canvas.cget("width")), columns)
            self._display = ("spectrum", geometry, positions)
        else:
            self._display = ("image", geometry, None)
            rgb = sensor_view(
                to_rgb(frame, colormap=self.colormap.get()),
                geometry,
                self.camera.width,
                self.camera.height,
                *self._view_size(),
            )
            self._image = self._tk.PhotoImage(data=to_ppm(rgb), format="PPM")
            self.canvas.create_image(self.VIEW_MARGIN, self.VIEW_MARGIN, anchor="nw", image=self._image)
            self.last_display = "image"
            self.last_unit = None
        self._draw_region()
        self._draw_selection()

    def _preview_readout(self, geometry):
        """Redraw at once after a readout change while idle: keep an image under the new window, preview a spectrum
        from the last image, or show the empty view the next acquisition will fill."""
        if "canvas" not in self.__dict__:
            return
        last = self.__dict__.get("_last_shown")
        spectrum = geometry[3] // geometry[5] == 1
        last_is_image = last is not None and last[0].shape[0] > 1
        if spectrum and last_is_image:
            values = preview_spectrum(last[0], last[2], geometry)
            if values is not None:
                self._render(values, None, geometry)
                self.status_text.set("preview: rows of the last image summed in software; Acquire for a real FVB readout")
                return
        if not spectrum and last_is_image:
            self._render(*last)
            return
        if last is None:
            self._draw_no_frame(geometry)
            return
        self.canvas.delete("all")
        width, height = self._view_size()
        margin = self.VIEW_MARGIN
        if spectrum:
            self.canvas.create_rectangle(0, 0, width + 2 * margin, height + 2 * margin, fill="black", outline="")
            self.last_display, self._display = "spectrum", None
        else:
            self.canvas.create_rectangle(
                margin, margin, margin + width, margin + height, fill="#%02x%02x%02x" % OUTSIDE_READOUT, outline=""
            )
            self.last_display, self._display = "image", ("image", geometry, None)
            self._draw_region()
        self.canvas.create_text(
            margin + width // 2, margin + height // 2, text=f"Acquire to read the {describe_readout(geometry)}",
            fill="#a0a0a0",
        )

    def _draw_no_frame(self, geometry=None):
        """Black imaging area before the first frame, with the selected window outlined so it can still be dragged."""
        self.canvas.delete("all")
        margin = self.VIEW_MARGIN
        width, height = self._view_size()
        self.canvas.create_rectangle(margin, margin, margin + width, margin + height, fill="black", outline="")
        self.canvas.create_text(margin + width // 2, margin + height // 2, text="No frame available yet", fill="#a0a0a0")
        if geometry is not None:
            self.last_display, self._display = "image", ("image", geometry, None)
            self._draw_region()

    def _view_size(self):
        """Size of the whole-sensor image, which sits inside a margin where a drag can start beyond the sensor edge."""
        margin = 2 * self.VIEW_MARGIN
        return int(self.canvas.cget("width")) - margin, int(self.canvas.cget("height")) - margin

    def _view_point(self, event):
        offset = self.VIEW_MARGIN if self.last_display == "image" else 0
        return event.x - offset, event.y - offset

    def _active_region(self):
        return self.__dict__.get("_region") or (0, 0, self.camera.width, self.camera.height)

    def _active_box(self):
        return region_box(self._active_region(), self._view_size(), (self.camera.width, self.camera.height))

    def _full_sensor(self):
        return tuple(self._active_region()) == (0, 0, self.camera.width, self.camera.height)

    def _handle_at(self, x, y):
        if self.last_display != "image" or self._full_sensor():
            return None
        return box_handle((x, y), self._active_box(), self._view_size())

    def _hover(self, event):
        if self.__dict__.get("_drag") is None:
            self.canvas.config(cursor=HANDLE_CURSORS[self._handle_at(*self._view_point(event))])

    def _drag_start(self, event):
        x, y = self._view_point(event)
        handle = self._handle_at(x, y)
        box = self._active_box() if handle else (x, y, x, y)
        self._drag = {"handle": handle, "start": (x, y), "box": box, "current": box}

    def _drag_move(self, event):
        drag = self.__dict__.get("_drag")
        if drag is None:
            return
        handle, (x, y), (to_x, to_y) = drag["handle"], drag["start"], self._view_point(event)
        if handle is None:
            drag["current"] = (x, y, to_x, to_y)
        else:
            drag["current"] = adjust_box(drag["box"], handle, to_x - x, to_y - y, self._view_size())
        self._draw_selection()

    def _drag_end(self, event):
        drag, self._drag = self.__dict__.get("_drag"), None
        self.canvas.delete("selection")
        if drag is None:
            return
        handle, (x, y), (to_x, to_y) = drag["handle"], drag["start"], self._view_point(event)
        if handle is None:
            self.select_region((x, y), (to_x, to_y))
        elif handle == "move":
            self.move_region(to_x - x, to_y - y)
        else:
            self.resize_region(handle, adjust_box(drag["box"], handle, to_x - x, to_y - y, self._view_size()))

    def _draw_selection(self):
        self.canvas.delete("selection")
        drag = self.__dict__.get("_drag")
        if drag is None:
            return
        x0, y0, x1, y1 = drag["current"]
        if self.last_display == "spectrum":
            y0, y1 = 0, int(self.canvas.cget("height"))
        else:
            width, height = self._view_size()
            x0, x1 = (min(max(value, 0), width) + self.VIEW_MARGIN for value in (x0, x1))
            y0, y1 = (min(max(value, 0), height) + self.VIEW_MARGIN for value in (y0, y1))
        self.canvas.create_rectangle(x0, y0, x1, y1, outline="yellow", dash=(4, 2), tags="selection")

    def _draw_region(self):
        self.canvas.delete("region")
        if self.last_display != "image" or self._full_sensor():
            return
        margin, size = self.VIEW_MARGIN, 3
        x0, y0, x1, y1 = (value + margin for value in self._active_box())
        self.canvas.create_rectangle(x0, y0, x1 - 1, y1 - 1, outline="#00d0ff", width=2, tags="region")
        for x, y in ((x0, y0), (x1, y0), (x0, y1), (x1, y1)):
            self.canvas.create_rectangle(
                x - size, y - size, x + size, y + size, fill="#00d0ff", outline="", tags="region"
            )

    def _binning_fields(self):
        try:
            return tuple(max(1, int(variable.get())) for variable in self.binning_text)
        except ValueError:
            return 1, 1

    def _set_region(self, region):
        if region is None or tuple(region) == tuple(self._active_region()):
            return False
        self.region_text.set("custom")
        for variable, value in zip(self.roi_text, region):
            variable.set(str(value))
        return self.apply_readout()

    def move_region(self, dx, dy):
        """Move the active window by a drag of (dx, dy) view pixels, keeping its size."""
        sensor = (self.camera.width, self.camera.height)
        return self._set_region(moved_region(self._active_region(), dx, dy, self._view_size(), sensor))

    def resize_region(self, handle, box):
        """Resize the active window to the dragged box; handle names the edges that moved."""
        sensor = (self.camera.width, self.camera.height)
        binning, fvb = self._binning_fields(), bool(self.fvb.get())
        region = resized_region(self._active_region(), handle, box, self._view_size(), sensor, binning, fvb)
        return self._set_region(region)

    def select_region(self, start, end):
        """Turn a drag on the display into a custom region: a rectangle on an image, a column range on a spectrum."""
        display = self.__dict__.get("_display")
        if display is None:
            return False
        kind, geometry, positions = display
        if kind == "spectrum":
            region = spectrum_selection(start[0], end[0], positions, geometry)
        else:
            region = selection_region(
                start,
                end,
                self._view_size(),
                (self.camera.width, self.camera.height),
                self._binning_fields(),
                bool(self.fvb.get()),
            )
        return self._set_region(region)

    def reset_region(self):
        self.region_text.set("full")
        return self.apply_readout()

    def _poll(self):
        for kind, payload in self.controller.drain():
            if kind == "frame":
                self.show(payload)
                self.status_text.set(f"frame {self.frames_shown}")
            elif kind == "error":
                self.status_text.set(f"error: {payload}")
            elif kind == "done":
                self._apply_pending()
        self._poll_id = self.root.after(self.POLL_MS, self._poll)

    def _poll_temperature(self):
        if not self.controller.running:
            try:
                self.temperature_text.set(format_temperature(self.camera.temperature()))
            except CameraError as error:
                self.temperature_text.set("n/a")
                self.status_text.set(str(error))
        self.root.after(self.TEMPERATURE_MS, self._poll_temperature)

    def close(self):
        self.controller.stop()
        self.controller.join()
        try:
            self.camera.close()
        finally:
            self.root.destroy()


def run_view(simulate, close_after=None, selector=None):
    camera = make_camera(simulate, selector)
    try:
        camera.open()
    except CameraUnavailable as error:
        print(f"openst133 view: {error}", file=sys.stderr)
        return 2
    try:
        import tkinter

        root = tkinter.Tk()
    except Exception as error:
        logger.error("cannot open a window: %s", error)
        camera.close()
        print(f"openst133 view: cannot open a window: {error}", file=sys.stderr)
        return 1
    ViewerApp(root, camera, close_after=close_after)
    root.mainloop()
    return 0


def run_acquire(simulate, exposure, path, frames=1, selector=None):
    camera = make_camera(simulate, selector)
    try:
        acquire_to_file(camera, exposure, path, frames)
    except CameraUnavailable as error:
        print(f"openst133 acquire: {error}", file=sys.stderr)
        return 2
    except (CameraError, OSError, ValueError) as error:
        logger.error("acquire failed: %s", error)
        print(f"openst133 acquire: {error}", file=sys.stderr)
        return 1
    print(f"wrote {frames} frame(s) to {path}")
    return 0
