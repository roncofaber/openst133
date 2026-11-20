# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT

import json
import pathlib
import tempfile
import unittest
from unittest import mock

import numpy

from openst133 import export, spe

try:
    import h5py
except ImportError:
    h5py = None
import tifffile

METADATA = {"roi": (200, 0, 800, 100), "binning": (2, 100), "controller_gain": 2, "shutter": "closed"}


class ExportTest(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.directory = pathlib.Path(directory.name)
        self.spectra = [numpy.arange(400, dtype=numpy.uint16).reshape(1, 400) + index for index in range(2)]
        self.image = numpy.arange(60, dtype=numpy.uint16).reshape(6, 10)

    def test_extension_selects_the_format(self):
        self.assertEqual(export.file_format("a.H5"), "HDF5")
        self.assertEqual(export.file_format("a.tiff"), "TIFF")
        with self.assertRaisesRegex(ValueError, "unknown file type .png"):
            export.file_format("a.png")
        with self.assertRaisesRegex(ValueError, "unknown file type"):
            export.save_frames(self.directory / "a.png", [self.image], 0.1)

    def test_missing_extension_comes_from_the_chosen_type(self):
        self.assertEqual(export.with_extension("/data/frame", "HDF5"), "/data/frame.h5")
        self.assertEqual(export.with_extension("/data/frame", "TIFF (*.tif *.tiff)"), "/data/frame.tif")
        self.assertEqual(export.with_extension("/data/frame", "CSV (spectra) (*.csv)"), "/data/frame.csv")
        self.assertEqual(export.with_extension("/data/frame.spe", "HDF5"), "/data/frame.spe")
        self.assertEqual(export.with_extension("/data/frame", "unknown"), "/data/frame")

    def test_shared_metadata_and_effective_exposure(self):
        class Camera:
            effective_exposure = 9.984

            def spe_metadata(self):
                return {"requested_exposure_seconds": 10.0, "controller_gain": 2}

            def temperature(self):
                return -99.987

        metadata = export.acquisition_metadata(Camera())
        self.assertEqual(metadata, {"requested_exposure_seconds": 10.0, "controller_gain": 2, "temperature_celsius": -99.99})
        self.assertEqual(export.effective_exposure(Camera(), 10.0), 9.984)
        self.assertEqual(export.effective_exposure(object(), 10.0), 10.0)
        self.assertEqual(export.acquisition_metadata(object()), {})

    def test_spe_receives_only_spe_fields(self):
        with mock.patch("openst133.export.write_spe") as write:
            export.save_frames(self.directory / "b.spe", [self.image], 0.1, {**METADATA, "exposure_seconds": 0.1, "software_version": "x"})
        self.assertEqual(write.call_args.kwargs["metadata"], METADATA)

    def test_spe_is_written_by_the_spe_writer(self):
        with mock.patch("openst133.export.write_spe") as write:
            export.save_frames(self.directory / "a.spe", [self.image], 0.1, METADATA)
        write.assert_called_once_with(self.directory / "a.spe", [self.image], 0.1, metadata=METADATA)

    @unittest.skipIf(h5py is None, "h5py not installed")
    def test_hdf5_keeps_frames_dtype_and_metadata(self):
        path = self.directory / "spectra.h5"
        export.save_frames(path, self.spectra, 0.25, METADATA)
        with h5py.File(path, "r") as handle:
            frames = handle["frames"][...]
            self.assertEqual((frames.shape, frames.dtype), ((2, 1, 400), numpy.uint16))
            numpy.testing.assert_array_equal(frames[1, 0], self.spectra[1][0])
            self.assertEqual(handle.attrs["exposure_seconds"], 0.25)
            self.assertEqual(list(handle.attrs["roi"]), [200, 0, 800, 100])
            self.assertEqual(handle.attrs["shutter"], "closed")
            self.assertTrue(handle.attrs["software"].startswith("openst133 "))
            numpy.testing.assert_array_equal(handle["sensor_column"][:3], [200, 202, 204])

    def test_tiff_keeps_frames_and_metadata(self):
        path = self.directory / "image.tif"
        export.save_frames(path, [self.image, self.image + 1], 0.5, METADATA)
        with tifffile.TiffFile(path) as handle:
            stack = handle.asarray()
            description = json.loads(handle.pages[0].description)
        self.assertEqual((stack.shape, stack.dtype), ((2, 6, 10), numpy.uint16))
        numpy.testing.assert_array_equal(stack[1], self.image + 1)
        self.assertEqual(description["exposure_seconds"], 0.5)
        self.assertEqual(description["binning"], [2, 100])

    def test_csv_writes_spectra_by_sensor_column(self):
        path = self.directory / "spectra.csv"
        export.save_frames(path, self.spectra, 0.1, METADATA)
        lines = path.read_text().splitlines()
        self.assertIn('# roi: [200, 0, 800, 100]', lines)
        header = lines.index("sensor_column,frame_0,frame_1")
        self.assertEqual(lines[header + 1], "200,0,1")
        self.assertEqual(lines[-1], "998,399,400")
        with self.assertRaisesRegex(ValueError, "CSV holds spectra"):
            export.save_frames(self.directory / "image.csv", [self.image], 0.1)
        self.assertFalse((self.directory / "image.csv").exists())

    def test_version_is_unknown_without_an_installation(self):
        missing = export.package_metadata.PackageNotFoundError("openst133")
        with mock.patch.object(export.package_metadata, "version", side_effect=missing):
            self.assertEqual(export.software_version(), "unknown")

    def test_existing_files_are_never_overwritten(self):
        for name in ("a.h5", "a.tif", "a.csv"):
            path = self.directory / name
            path.write_text("keep")
            with self.subTest(name=name), self.assertRaises(FileExistsError):
                export.save_frames(path, self.spectra, 0.1)
            self.assertEqual(path.read_text(), "keep")

    def test_hdf5_without_h5py_names_the_alternatives(self):
        with mock.patch.dict("sys.modules", {"h5py": None}):
            with self.assertRaisesRegex(ValueError, "save as TIFF or SPE"):
                export.save_frames(self.directory / "a.h5", self.spectra, 0.1)
        self.assertFalse((self.directory / "a.h5").exists())

    def test_spe_round_trip_through_save_frames(self):
        path = self.directory / "a.spe"
        export.save_frames(path, [self.image], 0.1, {"roi": (0, 0, 10, 6), "binning": (1, 1)})
        info, frames = spe.read(path)
        self.assertEqual((info.width, info.height), (10, 6))
        numpy.testing.assert_array_equal(numpy.asarray(frames[0]).reshape(6, 10), self.image)


if __name__ == "__main__":
    unittest.main()
