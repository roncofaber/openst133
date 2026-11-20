# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT

import builtins
import contextlib
import importlib.util
import io
import json
import math
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import numpy as np

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from openst133.characterization import (
    analyze_fallback,
    characterize,
    synthetic_frames,
    write_emva_dataset,
)


EXPOSURES = (0.001, 0.005, 0.02, 0.1, 0.4)


class CharacterizationTests(unittest.TestCase):
    def test_fallback_recovers_known_gain_read_noise_and_dark_current(self):
        frames = synthetic_frames(
            EXPOSURES,
            width=96,
            height=64,
            gain_e_per_dn=2.0,
            read_noise_dn=4.0,
            offset_dn=100.0,
            dark_current_dn_s=8.0,
            flats=True,
            seed=41,
        )
        result = analyze_fallback(frames, EXPOSURES)
        self.assertAlmostEqual(result["offset"]["value"], 100.0, delta=0.2)
        self.assertAlmostEqual(
            result["read_noise"]["value"], 4.0, delta=4.0 * 0.05
        )
        self.assertAlmostEqual(
            result["dark_current"]["value"], 8.0, delta=8.0 * 0.05
        )
        self.assertAlmostEqual(
            result["gain"]["electrons_per_dn"], 2.0, delta=2.0 * 0.05
        )
        self.assertAlmostEqual(
            result["gain"]["dn_per_electron"], 0.5, delta=0.5 * 0.05
        )
        self.assertEqual(result["gain"]["backend"], "fallback")

    def test_dark_only_reports_gain_and_linearity_not_measured(self):
        frames = synthetic_frames(EXPOSURES, width=32, height=24, flats=False, seed=4)
        result = analyze_fallback(frames, EXPOSURES)
        self.assertEqual(result["gain"]["status"], "not_measured")
        self.assertEqual(result["linearity"]["status"], "not_measured")
        self.assertIn("uniform", result["gain"]["reason"])

    def test_read_noise_uses_pair_difference_without_fixed_pattern(self):
        first = np.array([[103.0, 105.0], [107.0, 109.0]])
        second = first + np.array([[2.0, 2.0], [2.0, 4.0]])
        frames = {
            "dark": [[(first, second)]],
            "flat": None,
            "spatial_dark": [],
            "spatial_flat": [],
        }
        result = analyze_fallback(frames, (0.001,))
        expected = np.std(np.array([2.0, 2.0, 2.0, 4.0])) / math.sqrt(2.0)
        self.assertAlmostEqual(result["read_noise"]["value"], expected)

    def test_every_quantity_cites_method_and_source(self):
        frames = synthetic_frames(EXPOSURES[:3], width=24, height=16, flats=False, seed=9)
        result = analyze_fallback(frames, EXPOSURES[:3])
        for name in ("offset", "read_noise", "dark_current", "linearity", "gain"):
            self.assertTrue(result[name]["method"], name)
            self.assertTrue(result[name]["source"].startswith("https://"), name)

    def test_missing_emva_uses_labelled_fallback(self):
        frames = synthetic_frames(EXPOSURES, width=32, height=24, flats=True, seed=7)
        original_import = builtins.__import__

        def without_emva(name, *args, **kwargs):
            if name.startswith("emva1288"):
                raise ImportError("simulated missing optional dependency")
            return original_import(name, *args, **kwargs)

        with tempfile.TemporaryDirectory() as temporary, mock.patch(
            "builtins.__import__", side_effect=without_emva
        ):
            result = characterize(frames, EXPOSURES, Path(temporary))
        self.assertEqual(result["backend"], "fallback")
        self.assertEqual(result["gain"]["backend"], "fallback")
        self.assertIn("not installed", result["emva1288"])

    @unittest.skipUnless(importlib.util.find_spec("emva1288"), "emva1288 not installed")
    def test_emva_adapter_does_not_write_parser_logs_to_stderr(self):
        frames = synthetic_frames(EXPOSURES, width=32, height=24, flats=True, seed=10)
        stderr = io.StringIO()
        with tempfile.TemporaryDirectory() as temporary, contextlib.redirect_stderr(stderr):
            characterize(frames, EXPOSURES, Path(temporary))
        self.assertEqual(stderr.getvalue(), "")

    @unittest.skipUnless(importlib.util.find_spec("emva1288"), "emva1288 not installed")
    def test_emva_backend_recovers_known_gain(self):
        frames = synthetic_frames(
            EXPOSURES,
            width=96,
            height=64,
            gain_e_per_dn=2.0,
            read_noise_dn=4.0,
            offset_dn=100.0,
            dark_current_dn_s=8.0,
            flats=True,
            spatial_count=4,
            seed=42,
        )
        with tempfile.TemporaryDirectory() as temporary:
            descriptor = write_emva_dataset(Path(temporary), frames, EXPOSURES)
            result = characterize(frames, EXPOSURES, Path(temporary))
            self.assertTrue(descriptor.is_file())
            self.assertEqual(result["backend"], "emva1288")
            self.assertEqual(result["gain"]["backend"], "emva1288")
            self.assertAlmostEqual(
                result["gain"]["electrons_per_dn"], 2.0, delta=2.0 * 0.05
            )
            self.assertAlmostEqual(
                result["read_noise"]["value"], 4.0, delta=4.0 * 0.05
            )
            Path(temporary, "report.json").write_text(json.dumps(result))


if __name__ == "__main__":
    unittest.main()
