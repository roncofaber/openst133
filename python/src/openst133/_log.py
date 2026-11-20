# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT

import logging


_package_logger = logging.getLogger("openst133")
if not any(type(handler) is logging.NullHandler for handler in _package_logger.handlers):
    _package_logger.addHandler(logging.NullHandler())

camera = logging.getLogger("openst133.camera")
viewer = logging.getLogger("openst133.viewer")
