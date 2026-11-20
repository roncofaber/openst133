# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT

import os
import re
import subprocess
import sys
from pathlib import Path

import pytest

EXAMPLE = Path(__file__).resolve().parents[1] / "examples" / "simulated_app.py"

# Closes the window of the example app through ScopeFoundry's confirm dialog (answered Yes) while a measurement runs.
DRIVER = r"""
import importlib.util, sys, time
from qtpy import QtCore, QtWidgets

spec = importlib.util.spec_from_file_location("simulated_app", sys.argv[1])
example = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = example
spec.loader.exec_module(example)
mode, sim_speed = sys.argv[2], float(sys.argv[3])

QtWidgets.QMessageBox.question = staticmethod(lambda *args, **kwargs: QtWidgets.QMessageBox.Yes)
app = example.OpenST133App([])
app.settings["save_dir"] = "."
hw = app.hardware["openst133"]
measure = app.measurements["openst133_acquire"]
hw.settings["sim_speed"] = sim_speed
hw.settings["exposure"] = 0.05
hw.settings["connected"] = True
measure.settings["mode"] = mode
measure.settings["n_frames"] = 1000

def close_while_measuring():
    print("measuring at close:", measure.is_measuring(), flush=True)
    app.ui.close()

measure.start()
QtCore.QTimer.singleShot(800, close_while_measuring)
code = example.run(app)
thread = measure.acq_thread
print("after close:", hw.camera is None, measure.is_measuring(), thread is not None and thread.isRunning(), flush=True)
sys.exit(code)
"""

ALLOWED_STDERR = (
    re.compile(r"INFO:"),
    # The documented install has no IPython; docs/user/scopefoundry.md describes this fallback notice.
    re.compile(r"WARNING:root:ScopeFoundry unable to import iPython console, using pyqtgraph\.console instead\."),
    # Printed by Qt's offscreen platform plugin when a window is shown; a desktop session does not print it.
    re.compile(r"This plugin does not support propagateSizeHints\(\)$"),
)


@pytest.mark.parametrize("mode,sim_speed", [("live", 0.0), ("live", 1.0), ("single", 1.0)])
def test_simulated_app_closed_during_acquisition_exits_cleanly(tmp_path, mode, sim_speed):
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen", OPENST133_NO_HARDWARE="1")
    result = subprocess.run(
        [sys.executable, "-c", DRIVER, str(EXAMPLE), mode, str(sim_speed)],
        cwd=tmp_path, env=env, capture_output=True, text=True, timeout=120,
    )
    unexpected = [line for line in result.stderr.splitlines() if not any(p.match(line) for p in ALLOWED_STDERR)]
    assert result.returncode == 0, result.stderr
    assert unexpected == [], "\n".join(unexpected)
    assert "measuring at close: True" in result.stdout
    assert "after close: True False False" in result.stdout
    assert not list(tmp_path.rglob("*.h5"))
