# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT

"""Start a ScopeFoundry app with the openst133 plugin on the simulated camera.

    python simulated_app.py [--real]
"""

import sys

from ScopeFoundry import BaseMicroscopeApp

from ScopeFoundryHW.openst133 import OpenST133HW, OpenST133Measure, close_app


class OpenST133App(BaseMicroscopeApp):
    name = "openst133_app"

    def setup(self):
        hardware = self.add_hardware(OpenST133HW(self))
        hardware.settings["simulate"] = "--real" not in sys.argv
        self.add_measurement(OpenST133Measure(self))


def run(app):
    try:
        return app.exec_()
    finally:
        close_app(app)


if __name__ == "__main__":
    app = OpenST133App([a for a in sys.argv if a != "--real"])
    sys.exit(run(app))
