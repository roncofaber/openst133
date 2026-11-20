# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT

"""Orderly shutdown of a ScopeFoundry app that uses the openst133 plugin."""

import logging

from qtpy import QtCore, QtWidgets


def close_app(app, timeout=10.0):
    """Stop measurements, disconnect hardware and delete the main window while Python is still fully running.

    Call it after app.exec_() returns, or when a test is done with an app. ScopeFoundry installs Python event filters on
    its main window and subwindows; a window still alive at interpreter exit is destroyed by PySide's exit hook, which
    calls those filters on half-deleted objects and prints an ignored RuntimeError traceback. The app's status bar and
    log file handlers are removed too, since its logger is shared by every app instance of the same class.
    """
    for measurement in app.measurements.values():
        if measurement.is_measuring():
            measurement.activation.update_value(False)
    app.on_close()
    for measurement in app.measurements.values():
        thread = getattr(measurement, "acq_thread", None)
        if thread is not None and not thread.wait(int(timeout * 1000)):
            raise RuntimeError(f"measurement {measurement.name} did not stop within {timeout} s")
    QtWidgets.QApplication.processEvents()
    for handler in list(app.log.handlers):
        if getattr(handler, "setter", None) == getattr(app, "show_status_bar_msg", None):
            app.log.removeHandler(handler)
    file_handler = getattr(app, "log_file_handler", None)
    if file_handler is not None:
        logging.getLogger().removeHandler(file_handler)
        file_handler.close()
    ui = getattr(app, "ui", None)
    if ui is not None:
        ui.deleteLater()
        QtCore.QCoreApplication.sendPostedEvents(None, QtCore.QEvent.Type.DeferredDelete)
        QtWidgets.QApplication.processEvents()
