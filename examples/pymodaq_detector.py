# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT

"""Synchronous PyMoDAQ 2D detector skeleton with a simulated openst133 camera."""

import numpy

try:
    from pymodaq.control_modules.daq_viewer import DAQ_Viewer_base
    from pymodaq.utils.data import DataFromPlugins
    from pymodaq_data.data import DataToExport
except ImportError as error:
    DAQ_Viewer_base = object
    DataFromPlugins = None
    DataToExport = None
    _PYMODAQ_IMPORT_ERROR = error
else:
    _PYMODAQ_IMPORT_ERROR = None

from openst133.camera import SimulatedCamera


class DAQ_2DViewer_OpenST133(DAQ_Viewer_base):
    camera_factory = SimulatedCamera
    params = [
        {"title": "Exposure (s)", "name": "exposure_s", "type": "float", "value": 0.01, "min": 0.001},
        {"title": "Shutter", "name": "shutter", "type": "list", "value": "normal", "limits": ["normal", "closed"]},
    ]

    def ini_attributes(self):
        self.controller = None

    def ini_detector(self, controller=None):
        if self.is_master:
            self.controller = self.camera_factory()
        else:
            self.controller = controller
        if self.controller is None:
            return "The master detector has not supplied an openst133 controller", False
        try:
            if self.is_master:
                self.controller.open()
            self.controller.set_exposure(self.settings.child("exposure_s").value())
            self.controller.set_shutter(self.settings.child("shutter").value())
        except Exception as error:
            if self.is_master and self.controller is not None:
                self.controller.close()
            return str(error), False
        return f"openst133 {self.controller.width}x{self.controller.height} initialized", True

    def commit_settings(self, param):
        if param.name() == "exposure_s":
            self.controller.set_exposure(param.value())
        elif param.name() == "shutter":
            self.controller.set_shutter(param.value())

    def grab_data(self, Naverage=1, **kwargs):
        if DataFromPlugins is None or DataToExport is None:
            raise RuntimeError("grabbing through this plugin needs the optional pymodaq package") from _PYMODAQ_IMPORT_ERROR
        if Naverage < 1:
            raise ValueError("Naverage must be at least 1")
        frames = [self.controller.acquire() for _ in range(Naverage)]
        image = frames[0] if Naverage == 1 else numpy.mean(frames, axis=0)
        data = DataFromPlugins(name="openst133", data=[numpy.ascontiguousarray(image)], dim="Data2D")
        self.dte_signal.emit(DataToExport("openst133", data=[data]))

    def stop(self):
        if self.controller is not None:
            self.controller.abort()

    def close(self):
        if self.controller is not None and self.is_master:
            self.stop()
            self.controller.close()
        self.controller = None
