# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT

"""Live view and single acquisition for the ST-133 camera, saved as ScopeFoundry H5 with optional SPE export."""

import time

import numpy
from ScopeFoundry import Measurement

from openst133.camera import AcquisitionAborted, CameraError, CameraUnavailable
from openst133.export import save_frames, software_version


class OpenST133Measure(Measurement):
    """mode "live" shows frames until stopped and saves nothing; "single" acquires n_frames and saves them."""

    name = "openst133_acquire"

    def setup(self):
        self.settings.New("mode", str, initial="live", choices=("live", "single"), description="live: continuous view, no file. single: acquire n_frames and save.")
        self.settings.New("n_frames", int, initial=1, vmin=1, vmax=100000, description="Frames in a single acquisition.")
        self.settings.New("save_h5", bool, initial=True, description="Write a ScopeFoundry H5 file after a single acquisition.")
        self.settings.New("save_spe", bool, initial=False, description="Also export the frames as an SPE file next to the H5 file (needs the openst133 library).")
        self.settings.New("wavelength_start", float, initial=0.0, unit="nm", description="Wavelength of the first output column; stored only when the step is not zero.")
        self.settings.New("wavelength_step", float, initial=0.0, unit="nm", description="Wavelength step per output column; 0 means no wavelength axis.")
        self.settings.New("last_saved", str, initial="", ro=True)
        self.add_operation("Save last frame", self.save_last_frame)
        self.last_frame = None
        self.last_frame_settings = None
        self.readout_used = None
        self.last_temperature = float("nan")
        self.frames = []
        self.temperatures = []
        self.frame_times = []
        self.saved_paths = []
        self.error = None
        self.not_ready = None

    def setup_figure(self):
        from .measure_ui import AcquirePanel

        self.panel = AcquirePanel(self)
        self.ui = self.panel

    @property
    def hw(self):
        return self.app.hardware["openst133"]

    def pre_run(self):
        self.error = None
        self.frames = []
        self.temperatures = []
        self.frame_times = []
        self.saved_paths = []
        self.not_ready = None
        hw = self.hw
        if hw.camera is None:
            hw.settings["connected"] = True
        try:
            if hw.camera is None:
                raise CameraUnavailable(f"cannot open the camera: {hw.connect_error or 'see the log'}")
            if "acquisition" in hw.unsupported:
                raise CameraUnavailable(hw.unsupported["acquisition"])
            hw.configure_readout()
        except (CameraError, ValueError) as error:
            # A refusal is reported in the panel and the log rather than raised as an unhandled exception.
            self.not_ready = str(error)
            self.log.warning(f"acquisition not started: {error}")
            panel = getattr(self, "panel", None)
            if panel is not None:
                panel.status.setText(f"not started: {error}")

    def run(self):
        if getattr(self, "not_ready", None):
            return
        hw = self.hw
        live = self.settings["mode"] == "live"
        total = None if live else self.settings["n_frames"]
        self.exposure_used = hw.settings["exposure"]
        self.readout_used = hw.readout_selection()
        self.metadata_used = {}
        self.acquired_utc = int(time.time())
        try:
            index = 0
            if live:
                # A setting changed during the stream cannot reach the running camera: restart the stream with it.
                restart = True
                while restart:
                    restart = False
                    self.readout_used = hw.readout_selection()
                    frames = hw.stream()
                    try:
                        for frame in frames:
                            self._record_frame(frame)
                            if self.interrupt_measurement_called:
                                break
                            if hw.needs_restart():
                                restart = True
                                break
                    finally:
                        frames.close()
                return
            while not self.interrupt_measurement_called and (total is None or index < total):
                frame = hw.acquire()
                temperature = hw._read_temperature()
                self._record_frame(frame)
                self.last_temperature = temperature
                if not live:
                    self.frames.append(frame)
                    self.temperatures.append(temperature)
                    self.frame_times.append(self.last_frame_time)
                    self.set_progress(100.0 * (index + 1) / total)
                index += 1
        except AcquisitionAborted:
            self.interrupt_measurement_called = True
        except CameraError as error:
            self.error = str(error)
            self.log.error(f"acquisition failed: {error}")
            raise

    def _record_frame(self, frame):
        """Keep a frame with the exposure and settings the hardware recorded when its acquisition started."""
        hw = self.hw
        self.exposure_used, self.metadata_used = hw.acquired_exposure, dict(hw.acquired_metadata)
        self.last_frame = frame
        self.last_frame_time = time.time()
        self.last_frame_settings = (self.exposure_used, self.readout_used, self.metadata_used)

    def interrupt(self):
        self.interrupt_measurement_called = True
        self.hw.abort()

    def post_run(self):
        # ScopeFoundry clears interrupt_measurement_called when the thread ends, before post_run; end_state keeps it.
        if not getattr(self, "not_ready", None) and self.settings["mode"] == "single" and self.frames and getattr(self, "end_state", None) == "stop_success":
            self.save()

    def update_display(self):
        shown = (id(self.last_frame), self.last_frame_settings)
        if self.last_frame is not None and shown != getattr(self, "_shown", None):
            self._shown = shown
            try:
                self.panel.show_frame(self.last_frame, self.last_frame_settings[1])
            except RuntimeError:
                # A display refresh can arrive after the window was closed and its Qt objects deleted.
                pass

    DATASET_ATTRIBUTES = {
        "frames": {"dimensions": "frame, row, column", "unit": "counts"},
        "spectra": {"dimensions": "frame, column", "unit": "counts", "description": "frames summed over rows"},
        "temperature": {"dimensions": "frame", "unit": "C", "description": "sensor temperature after each frame"},
        "frame_time_utc": {"dimensions": "frame", "unit": "s", "description": "Unix time when each frame arrived"},
        "sensor_column": {"dimensions": "column", "description": "first sensor column of each output column"},
        "sensor_row": {"dimensions": "row", "description": "first sensor row of each output row"},
        "wavelength": {"dimensions": "column", "unit": "nm", "description": "start + step * column, user supplied"},
    }

    def data_arrays(self, frames, temperatures, times=None):
        stack = numpy.stack(frames)
        data = {
            "frames": stack,
            "spectra": stack.sum(axis=1, dtype=numpy.float64),
            "temperature": numpy.asarray(temperatures, dtype=float),
        }
        if times is not None and len(times) == len(frames):
            data["frame_time_utc"] = numpy.asarray(times, dtype=float)
        x, y, _, _, x_binning, y_binning = getattr(self, "readout_used", None) or (0, 0, 0, 0, 1, 1)
        data["sensor_column"] = x + numpy.arange(stack.shape[2]) * x_binning
        data["sensor_row"] = y + numpy.arange(stack.shape[1]) * y_binning
        step = self.settings["wavelength_step"]
        if step != 0.0:
            data["wavelength"] = self.settings["wavelength_start"] + step * numpy.arange(stack.shape[2])
        return data

    def acquisition_metadata(self, temperatures):
        """openst133's acquisition metadata for the saved frames, with the effective exposure, acquisition time,
        software version and the mean of the per-frame temperatures."""
        metadata = dict(getattr(self, "metadata_used", None) or {})
        metadata["exposure_seconds"] = float(self.exposure_used)
        metadata["acquired_utc"] = int(getattr(self, "acquired_utc", None) or time.time())
        metadata["software_version"] = f"openst133 {software_version()}"
        valid = numpy.asarray(temperatures, dtype=float)
        valid = valid[numpy.isfinite(valid)]
        if valid.size:
            metadata["temperature_celsius"] = round(float(valid.mean()), 2)
        return metadata

    def save(self, frames=None, temperatures=None, times=None):
        """Write H5 (default) and SPE (optional) and return the written paths."""
        frames = self.frames if frames is None else frames
        temperatures = self.temperatures if temperatures is None else temperatures
        times = self.frame_times if times is None else times
        metadata = self.new_dataset_metadata()
        acquisition = self.acquisition_metadata(temperatures)
        written = []
        if self.settings["save_h5"]:
            group = self.open_new_h5_file(dataset_metadata=metadata)
            try:
                for key, value in self.data_arrays(frames, temperatures, times).items():
                    dataset = group.create_dataset(key, data=value, compression="gzip" if key == "frames" else None)
                    for name, text in self.DATASET_ATTRIBUTES.get(key, {}).items():
                        dataset.attrs[name] = text
                group.attrs["acquisition_record"] = (
                    "These attributes describe the saved frames as acquired; /hardware settings are the values when "
                    "the file was written and can differ for a frame saved later."
                )
                group.attrs["exposure_time"] = float(self.exposure_used)
                valid_temperatures = numpy.asarray(temperatures, dtype=float)
                valid_temperatures = valid_temperatures[numpy.isfinite(valid_temperatures)]
                group.attrs["temperature_mean"] = float(valid_temperatures.mean()) if valid_temperatures.size else float("nan")
                group.attrs["n_frames"] = len(frames)
                for key, value in acquisition.items():
                    group.attrs[key] = list(value) if isinstance(value, tuple) else value
            finally:
                self.close_h5_file()
            written.append(metadata.get_file_path(".h5"))
        if self.settings["save_spe"]:
            written.append(self.export_spe(metadata.get_file_path(".spe"), frames, acquisition))
        self.saved_paths = written
        self.settings["last_saved"] = ", ".join(str(path) for path in written)
        return written

    def export_spe(self, path, frames, acquisition):
        stack = [numpy.clip(numpy.rint(frame), 0, 65535).astype(numpy.uint16) for frame in frames]
        save_frames(path, stack, acquisition["exposure_seconds"], acquisition)
        return path

    def save_last_frame(self):
        """Save the most recent frame (also from live view) with the current save options."""
        if self.last_frame is None:
            self.log.warning("no frame to save yet")
            return []
        self.exposure_used, self.readout_used, self.metadata_used = self.last_frame_settings
        self.acquired_utc = int(time.time())
        return self.save([self.last_frame], [self.last_temperature], [getattr(self, "last_frame_time", time.time())])
