# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT

"""Qt panel for the acquire measurement, laid out like the openst133 Tk viewer."""

import numpy
import pyqtgraph as pg
from qtpy import QtCore, QtGui, QtWidgets

from openst133.export import FILE_TYPES, save_frames, with_extension
from openst133.viewer import COLORMAPS, describe_readout as describe, display_range, divisors

TYPING_PAUSE_MS = 600
NO_FRAME = "No frame available yet"


def colormap_lut(name):
    return (COLORMAPS[name](numpy.linspace(0.0, 1.0, 256)) * 255.0 + 0.5).astype(numpy.uint8)




class SpinBox(QtWidgets.QSpinBox):
    """Arrow steps apply at once; typed values apply after a pause (see AcquirePanel._bind_spin)."""

    def stepBy(self, steps):
        super().stepBy(steps)
        self.editingFinished.emit()


class DoubleSpinBox(QtWidgets.QDoubleSpinBox):
    def stepBy(self, steps):
        super().stepBy(steps)
        self.editingFinished.emit()


class DivisorSpinBox(QtWidgets.QSpinBox):
    """Arrow steps move only through factors that divide the region extent, so every step is a valid binning."""

    def __init__(self, extent, parent=None):
        super().__init__(parent)
        self.extent = extent
        self.setRange(1, 100000)

    def stepBy(self, steps):
        choices = divisors(max(1, self.extent()))
        current = self.value()
        if steps > 0:
            larger = [factor for factor in choices if factor > current]
            target = larger[min(steps, len(larger)) - 1] if larger else current
        else:
            smaller = [factor for factor in choices if factor < current]
            target = smaller[max(-len(smaller), steps)] if smaller else current
        self.setValue(target)
        self.editingFinished.emit()


class SensorROI(pg.RectROI):
    """A window whose handles stop at the sensor edge while the pointer keeps going.

    pyqtgraph rejects a handle move that would leave maxBounds, so a fast drag past the edge would leave the window
    short of it; clamping the requested handle position instead lets the window reach the edge exactly.
    """

    def __init__(self, bounds, *args, **kwargs):
        super().__init__(*args, maxBounds=bounds, **kwargs)
        self.bounds = bounds

    def movePoint(self, handle, pos, modifiers=None, finish=True, coords="parent"):
        point = self.mapSceneToParent(pg.Point(pos)) if coords == "scene" else pg.Point(pos)
        bounds = self.bounds
        clamped = pg.Point(
            min(max(point.x(), bounds.left()), bounds.right()), min(max(point.y(), bounds.top()), bounds.bottom())
        )
        return super().movePoint(handle, clamped, modifiers, finish, coords="parent")


class DrawViewBox(pg.ViewBox):
    """A left-button drag that starts outside the window draws a new one; panning and zooming stay off."""

    def __init__(self, drawn, bounds):
        super().__init__()
        self.drawn = drawn
        self.bounds = bounds
        self.band = QtWidgets.QGraphicsRectItem()
        self.band.setPen(pg.mkPen("y", width=1, style=QtCore.Qt.DashLine))
        self.band.setZValue(1000)
        self.band.hide()
        self.addItem(self.band, ignoreBounds=True)

    def mouseDragEvent(self, event, axis=None):
        if event.button() != QtCore.Qt.LeftButton:
            event.ignore()
            return
        event.accept()
        start, end = self.mapToView(event.buttonDownPos()), self.mapToView(event.pos())
        # The pointer may leave the image during the drag; the rectangle stops at the sensor edge.
        rect = QtCore.QRectF(start, end).normalized().intersected(self.bounds)
        if event.isFinish():
            self.band.hide()
            self.drawn(rect)
        else:
            self.band.setRect(rect)
            self.band.show()


class AcquirePanel(QtWidgets.QWidget):
    """Controls bound to the hardware settings, plus an image of the whole sensor with a movable, resizable window."""

    def __init__(self, measure):
        super().__init__()
        self.measure = measure
        self.hw = measure.app.hardware["openst133"]
        self.sensor = (1340, 100)
        self._syncing = False
        self._batch = False
        self._timers = []
        self._build()
        self._sync_region()
        self._update_output()
        self._update_buttons()
        self.show_no_frame()

    def _label(self, layout, text, gap=10):
        label = QtWidgets.QLabel(text)
        label.setContentsMargins(gap, 0, 2, 0)
        layout.addWidget(label)
        return label

    def _row(self, grid, index, title):
        title_label = QtWidgets.QLabel(title)
        title_label.setAlignment(QtCore.Qt.AlignRight | QtCore.Qt.AlignVCenter)
        title_label.setMinimumWidth(90)
        grid.addWidget(title_label, index, 0)
        row = QtWidgets.QHBoxLayout()
        row.setSpacing(2)
        grid.addLayout(row, index, 1)
        return row

    def _debounced(self, apply):
        timer = QtCore.QTimer(self)
        timer.setSingleShot(True)
        timer.setInterval(TYPING_PAUSE_MS)
        timer.timeout.connect(apply)
        self._timers.append(timer)
        return timer

    def _bind_spin(self, widget, name):
        """Show a hardware setting in a spin box; arrows apply at once and typing applies after a short pause."""
        lq = self.hw.settings.get_lq(name)
        widget.setValue(lq.val)
        widget.setKeyboardTracking(True)
        timer = self._debounced(lambda: lq.update_value(widget.value()))
        widget.valueChanged.connect(lambda value: None if self._syncing else timer.start())
        widget.editingFinished.connect(lambda: (timer.stop(), lq.update_value(widget.value())))
        lq.add_listener(lambda: self._quietly(widget.setValue, lq.val))

    def _quietly(self, setter, value):
        self._syncing = True
        try:
            setter(value)
        finally:
            self._syncing = False

    def _build(self):
        outer = QtWidgets.QVBoxLayout(self)
        grid = QtWidgets.QGridLayout()
        grid.setHorizontalSpacing(6)
        outer.addLayout(grid)
        settings = self.hw.settings

        acquisition = self._row(grid, 0, "Exposure (s)")
        self.exposure = DoubleSpinBox()
        self.exposure.setDecimals(3)
        self.exposure.setRange(0.001, 60.0)
        self.exposure.setSingleStep(0.01)
        self.exposure.setFixedWidth(90)
        self._bind_spin(self.exposure, "exposure")
        acquisition.addWidget(self.exposure)
        self.buttons = {}
        for name, action in (
            ("Acquire", self.acquire_once),
            ("Continuous", self.acquire_continuous),
            ("Stop", self.stop),
            ("Save...", self.save_as),
        ):
            button = QtWidgets.QPushButton(name)
            button.setFixedWidth(100)
            button.clicked.connect(action)
            acquisition.addWidget(button)
            self.buttons[name] = button
        acquisition.addStretch(1)

        camera = self._row(grid, 1, "Shutter")
        for name, label, width in (
            ("shutter", None, 90), ("gain", "Gain", 50), ("accumulations", "Accumulations", 55),
        ):
            if label:
                self._label(camera, label)
            combo = QtWidgets.QComboBox()
            combo.setFixedWidth(width)
            settings.get_lq(name).connect_to_widget(combo)
            camera.addWidget(combo)
        self._label(camera, "Readout")
        self.rate = QtWidgets.QComboBox()
        self.rate.setFixedWidth(90)
        settings.get_lq("readout_rate").connect_to_widget(self.rate)
        camera.addWidget(self.rate)
        camera.addStretch(1)

        cooling = self._row(grid, 2, "Setpoint (C)")
        self.setpoint = DoubleSpinBox()
        self.setpoint.setRange(-152.0, -25.0)
        self.setpoint.setSingleStep(0.5)
        self.setpoint.setDecimals(1)
        self.setpoint.setFixedWidth(80)
        self._bind_spin(self.setpoint, "temperature_setpoint")
        cooling.addWidget(self.setpoint)
        self._label(cooling, "Temperature", gap=16)
        self.temperature = QtWidgets.QLabel()
        self.temperature.setFixedWidth(70)
        settings.get_lq("temperature").connect_to_widget(self.temperature)
        cooling.addWidget(self.temperature)
        self._label(cooling, "Colors")
        self.colormap = QtWidgets.QComboBox()
        self.colormap.addItems(list(COLORMAPS))
        self.colormap.currentTextChanged.connect(lambda name: self.image.setLookupTable(colormap_lut(name)))
        cooling.addWidget(self.colormap)
        cooling.addStretch(1)

        readout = self._row(grid, 3, "Region")
        self.full = QtWidgets.QRadioButton("Full sensor")
        self.custom = QtWidgets.QRadioButton("Custom")
        self.full.toggled.connect(lambda checked: checked and self.reset_region())
        self.custom.toggled.connect(lambda checked: self._update_fields())
        readout.addWidget(self.full)
        readout.addWidget(self.custom)
        self.region = []
        for name, (low, high) in zip("xywh", ((0, 1339), (0, 99), (1, 1340), (1, 100))):
            self._label(readout, name, gap=6)
            box = SpinBox()
            box.setRange(low, high)
            box.setFixedWidth(65)
            box.setKeyboardTracking(True)
            timer = self._debounced(self._apply_region_fields)
            box.valueChanged.connect(lambda value, timer=timer: None if self._syncing else timer.start())
            box.editingFinished.connect(lambda timer=timer: (timer.stop(), self._apply_region_fields()))
            readout.addWidget(box)
            self.region.append(box)
        readout.addStretch(1)
        readout = self._row(grid, 4, "Bin x")
        self.bin_x = DivisorSpinBox(lambda: self.region[2].value())
        self.bin_y = DivisorSpinBox(lambda: self.region[3].value())
        readout.addWidget(self.bin_x)
        self._label(readout, "y", gap=6)
        readout.addWidget(self.bin_y)
        for box, name in ((self.bin_x, "bin_x"), (self.bin_y, "bin_y")):
            box.setFixedWidth(60)
            self._bind_spin(box, name)
        self.fvb = QtWidgets.QCheckBox("Sum rows (FVB)")
        settings.get_lq("full_vertical_binning").connect_to_widget(self.fvb)
        readout.addWidget(self.fvb)
        self.output = QtWidgets.QLabel()
        self.output.setFixedWidth(320)
        self.output.setContentsMargins(10, 0, 0, 0)
        readout.addWidget(self.output)
        readout.addStretch(1)

        self.graphics = pg.GraphicsLayoutWidget()
        self.graphics.setBackground(self.palette().color(QtGui.QPalette.Window))
        self.view = self.graphics.addPlot(viewBox=DrawViewBox(self._drawn, QtCore.QRectF(0, 0, *self.sensor)))
        self.view.invertY(True)
        self.view.setMouseEnabled(False, False)
        self.view.hideButtons()
        self.view.setMenuEnabled(False)
        self.view.getViewBox().setBackgroundColor(self.palette().color(QtGui.QPalette.Window))
        width, height = self.sensor
        self.view.setRange(xRange=(0, width), yRange=(0, height), padding=0)
        self.view.getViewBox().setCursor(QtCore.Qt.CrossCursor)
        self.view.setLabel("bottom", "Sensor column")
        self.view.setLabel("left", "Sensor row")
        self.outside = QtWidgets.QGraphicsRectItem(0, 0, width, height)
        self.outside.setBrush(pg.mkBrush(32, 32, 32))
        self.outside.setPen(pg.mkPen(None))
        self.view.addItem(self.outside)
        self.image = pg.ImageItem()
        self.image.setLookupTable(colormap_lut("gray"))
        self.view.addItem(self.image)
        self.window = SensorROI(
            QtCore.QRectF(0, 0, width, height), (0, 0), (width, height), pen=pg.mkPen("#00d0ff", width=2),
            snapSize=1.0, translateSnap=True, scaleSnap=True,
        )
        diagonal, antidiagonal = QtCore.Qt.SizeFDiagCursor, QtCore.Qt.SizeBDiagCursor
        vertical, horizontal = QtCore.Qt.SizeVerCursor, QtCore.Qt.SizeHorCursor
        self.window.getHandles()[0].setCursor(diagonal)
        for position, anchor, cursor in (
            ((0, 0), (1, 1), diagonal), ((1, 0), (0, 1), antidiagonal), ((0, 1), (1, 0), antidiagonal),
            ((0.5, 0), (0.5, 1), vertical), ((0.5, 1), (0.5, 0), vertical),
            ((0, 0.5), (1, 0.5), horizontal), ((1, 0.5), (0, 0.5), horizontal),
        ):
            self.window.addScaleHandle(position, anchor).setCursor(cursor)
        self.window.setCursor(QtCore.Qt.SizeAllCursor)
        self.window.sigRegionChangeFinished.connect(self._window_moved)
        self.view.addItem(self.window)
        self.message = pg.TextItem(NO_FRAME, color="#a0a0a0", anchor=(0.5, 0.5))
        self.message.setPos(width / 2, height / 2)
        self.view.addItem(self.message)
        self.view.scene().sigMouseClicked.connect(self._clicked)
        self.spectrum = self.graphics.addPlot(row=1, col=0)
        self.spectrum.setLabel("bottom", "Sensor column")
        self.spectrum_curve = self.spectrum.plot(pen=pg.mkPen("#8ab4f8", width=2))
        self.spectrum.hide()
        outer.addWidget(self.graphics, 1)
        self.status = QtWidgets.QLabel("idle")
        outer.addWidget(self.status)

        for name in ("roi_x0", "roi_x1", "roi_y0", "roi_y1", "bin_x", "bin_y", "full_vertical_binning"):
            settings.get_lq(name).add_listener(self._readout_changed)
        self.measure.activation.add_listener(lambda active: self._update_buttons(), argtype=(bool,))

    def _readout_changed(self):
        if self._batch:
            return
        self._sync_region()
        self._update_output()

    def selection(self):
        return self.hw.readout_selection()

    def _sync_region(self):
        settings = self.hw.settings
        x, y = settings["roi_x0"], settings["roi_y0"]
        w, h = settings["roi_x1"] - x, settings["roi_y1"] - y
        full = (x, y, w, h) == (0, 0, *self.sensor)
        self._syncing = True
        try:
            for box, value in zip(self.region, (x, y, w, h)):
                box.setValue(value)
            if full and not self.custom.isChecked():
                self.full.setChecked(True)
            elif not full:
                self.custom.setChecked(True)
            self.window.setPos((x, y), update=False, finish=False)
            self.window.setSize((w, h), update=True, finish=False)
            # On the full sensor the window is the whole view: hide it so any drag draws a new one.
            self.window.setVisible(not full)
        finally:
            self._syncing = False
        self._update_fields()

    def _update_fields(self):
        custom = self.custom.isChecked()
        for box in self.region:
            box.setEnabled(custom)
        self.bin_y.setEnabled(not self.fvb.isChecked())

    def _update_output(self):
        try:
            text, colour = f"Output {describe(self.selection())}", "black"
        except ValueError as error:
            text, colour = str(error), "red"
        self.output.setText(text)
        self.output.setStyleSheet(f"color: {colour}")

    def _set_region(self, x, y, w, h):
        settings = self.hw.settings
        width, height = self.sensor
        x, y = max(0, min(int(x), width - 1)), max(0, min(int(y), height - 1))
        w, h = max(1, min(int(w), width - x)), max(1, min(int(h), height - y))
        bx = settings["bin_x"]
        by = 1 if settings["full_vertical_binning"] else settings["bin_y"]
        w, h = max(bx, w // bx * bx), max(by, h // by * by)
        self._batch = True
        try:
            for name, value in (("roi_x0", x), ("roi_y0", y), ("roi_x1", x + w), ("roi_y1", y + h)):
                settings[name] = value
        finally:
            self._batch = False
        self._readout_changed()

    def _apply_region_fields(self):
        if self._syncing or not self.custom.isChecked():
            return
        self._set_region(*(box.value() for box in self.region))

    def _window_moved(self):
        if self._syncing:
            return
        (x, y), (w, h) = self.window.pos(), self.window.size()
        self.custom.setChecked(True)
        self._set_region(round(x), round(y), round(w), round(h))

    def _drawn(self, rect):
        rect = rect.normalized()
        if rect.width() < 2 and rect.height() < 2:
            return
        self.custom.setChecked(True)
        x0, y0 = max(0.0, rect.left()), max(0.0, rect.top())
        x1, y1 = min(float(self.sensor[0]), rect.right()), min(float(self.sensor[1]), rect.bottom())
        self._set_region(int(x0), int(y0), round(x1 - int(x0)), round(y1 - int(y0)))

    def _clicked(self, event):
        if event.double():
            self.reset_region()

    def reset_region(self):
        if not self.full.isChecked():
            self.full.setChecked(True)
            return
        self._set_region(0, 0, *self.sensor)

    def _update_buttons(self):
        running = bool(self.measure.activation.val)
        self.buttons["Acquire"].setEnabled(not running)
        self.buttons["Continuous"].setEnabled(not running)
        self.buttons["Stop"].setEnabled(running)

    def acquire_once(self):
        self.measure.settings["mode"] = "single"
        self.measure.settings["n_frames"] = 1
        self.measure.activation.update_value(True)

    def acquire_continuous(self):
        self.measure.settings["mode"] = "live"
        self.measure.activation.update_value(True)

    def stop(self):
        self.measure.activation.update_value(False)

    def save_as(self):
        if self.measure.last_frame is None:
            self.status.setText("no frame to save yet")
            return
        filters = ";;".join(f"{name} ({pattern})" for name, pattern in FILE_TYPES)
        path, chosen = QtWidgets.QFileDialog.getSaveFileName(self, "Save frame", "", filters)
        if path:
            self.save_to(with_extension(path, chosen))

    def save_to(self, path):
        measure = self.measure
        exposure, readout, settings = measure.last_frame_settings
        measure.exposure_used, measure.readout_used, measure.metadata_used = exposure, readout, settings
        metadata = measure.acquisition_metadata([measure.last_temperature])
        try:
            save_frames(path, [measure.last_frame], exposure, metadata)
        except (OSError, ValueError) as error:
            self.status.setText(f"save failed: {error}")
            return False
        self.status.setText(f"saved {path}")
        return True

    def show_no_frame(self):
        self.outside.setBrush(pg.mkBrush("k"))
        self.image.clear()
        self.message.show()
        self.spectrum.hide()
        self.view.show()

    def show_frame(self, frame, selection):
        """Draw an image where it sits on the sensor, each binned pixel stretched over its bin, or plot a spectrum."""
        x, y, width, height, x_binning, _ = selection
        frame = numpy.asarray(frame, dtype=float)
        self.outside.setBrush(pg.mkBrush(32, 32, 32))
        self.message.hide()
        if frame.shape[0] == 1:
            self.view.hide()
            self.spectrum.show()
            self.spectrum_curve.setData(x + numpy.arange(frame.shape[1]) * x_binning, frame[0])
        else:
            self.spectrum.hide()
            self.view.show()
            self.image.setImage(frame.T, levels=display_range(frame))
            self.image.setRect(QtCore.QRectF(x, y, width, height))
        self.status.setText(f"frame: {describe(selection)}")
