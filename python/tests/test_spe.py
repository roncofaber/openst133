# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT

import array
import os
import pathlib
import struct
import sys
import tempfile
import unittest
from unittest import mock

from openst133.spe import SpeFile, create, read

HEADER_SIZE = 4100


def spe2_bytes():
    header = bytearray((offset * 37 + 11) & 0xFF for offset in range(HEADER_SIZE))
    struct.pack_into("<H", header, 42, 3)
    struct.pack_into("<h", header, 108, 3)
    struct.pack_into("<H", header, 656, 2)
    struct.pack_into("<i", header, 1446, 2)
    struct.pack_into("<f", header, 10, 1.25)
    header[20:30] = b"01Jan2020\0"
    header[688:703] = b"WinSpec/32 2.6\0"
    struct.pack_into("<f", header, 1992, 2.5)
    return bytes(header) + struct.pack("<12H", *range(1, 13))


def spe3_bytes():
    header = bytearray(HEADER_SIZE)
    struct.pack_into("<h", header, 108, 3)
    struct.pack_into("<f", header, 1992, 3.0)
    struct.pack_into("<Q", header, 678, HEADER_SIZE + 20)
    frames = bytes(range(1, 7)) + b"\xa5" * 4 + bytes(range(11, 17)) + b"\xa5" * 4
    xml = (
        '<SpeFormat version="3.0" xmlns="http://www.princetoninstruments.com/spe/2009">'
        '<DataFormat><DataBlock type="Frame" count="2" '
        'pixelFormat="MonochromeUnsigned16" size="6" stride="10">'
        '<DataBlock type="Region" count="1" width="2" height="1" size="4" stride="4"/>'
        '<DataBlock type="Region" count="1" width="1" height="1" size="2" stride="2"/>'
        "</DataBlock></DataFormat></SpeFormat>"
    ).encode()
    return bytes(header) + frames + xml


class SpeTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="ost-spe-python-")
        self.root = pathlib.Path(self.directory.name)

    def tearDown(self):
        self.directory.cleanup()

    def write_fixture(self, name, contents):
        path = self.root / name
        path.write_bytes(contents)
        return path

    def test_spe2_metadata_and_numpy_free_pixels(self):
        source = self.write_fixture("source.spe", spe2_bytes())
        with SpeFile(source) as spe:
            info = spe.info
            self.assertEqual(info.major_version, 2)
            self.assertEqual(info.header_version, 2.5)
            self.assertEqual(info.data_type, 3)
            self.assertEqual((info.width, info.height), (3, 2))
            self.assertEqual(info.pixel_count, 6)
            self.assertEqual((info.frame_count, info.region_count), (2, 1))
            self.assertEqual(info.exposure_seconds, 1.25)
            self.assertEqual(info.date, "01Jan2020")
            self.assertEqual(info.software_version, "WinSpec/32 2.6")
            self.assertEqual(info.regions[0].size, 12)
            self.assertEqual(len(spe.header), HEADER_SIZE)
            self.assertIsNone(spe.xml_footer)
            frame = spe.frame(1)
            self.assertIsInstance(frame, array.array)
            self.assertEqual(frame.typecode, "H")
            self.assertEqual(frame.tolist(), list(range(7, 13)))
            self.assertEqual([len(frame) for frame in spe.data], [6, 6])
            with self.assertRaises(IndexError):
                spe.frame(2)
            destination = self.root / "roundtrip.spe"
            spe.write(destination)
            self.assertEqual(destination.read_bytes(), source.read_bytes())
            with self.assertRaises(OSError):
                spe.write(destination)

    def test_spe3_regions_metadata_and_round_trip(self):
        source = self.write_fixture("source3.spe", spe3_bytes())
        with SpeFile(source) as spe:
            self.assertEqual(spe.info.major_version, 3)
            self.assertEqual(spe.info.data_type, 3)
            self.assertEqual(spe.info.pixel_count, 3)
            self.assertEqual(spe.info.frame_count, 2)
            self.assertEqual(len(spe.info.regions), 2)
            self.assertEqual((spe.info.regions[0].width, spe.info.regions[0].height), (2, 1))
            self.assertEqual((spe.info.regions[1].width, spe.info.regions[1].height), (1, 1))
            self.assertIn('size="6" stride="10"', spe.xml_footer)
            frame = spe.frame(0)
            self.assertEqual(frame.typecode, "H")
            self.assertEqual(frame, array.array("H", [513, 1027, 1541]))
            destination = self.root / "roundtrip3.spe"
            spe.write(destination)
            self.assertEqual(destination.read_bytes(), source.read_bytes())

    def test_read_returns_plain_sequences_without_numpy(self):
        source = self.write_fixture("plain.spe", spe2_bytes())
        with mock.patch.dict(sys.modules, {"numpy": None}):
            metadata, frames = read(source)
        self.assertEqual(metadata.frame_count, 2)
        self.assertIsInstance(frames[0], array.array)
        self.assertEqual(frames[0].typecode, "H")
        self.assertEqual(frames[0].tolist(), list(range(1, 7)))

    def test_read_returns_shaped_numpy_frames_when_available(self):
        try:
            import numpy
        except ImportError:
            self.skipTest("numpy not installed")
        source = self.write_fixture("numpy.spe", spe2_bytes())
        metadata, frames = read(source)
        self.assertEqual(metadata.frame_count, 2)
        for index, frame in enumerate(frames):
            self.assertIsInstance(frame, numpy.ndarray)
            self.assertEqual(frame.dtype, numpy.dtype(numpy.uint16))
            self.assertEqual(frame.shape, (2, 3))
            self.assertEqual(frame.tolist(), [list(range(1 + 6 * index, 4 + 6 * index)), list(range(4 + 6 * index, 7 + 6 * index))])

    def test_read_keeps_disjoint_regions_flat(self):
        try:
            import numpy
        except ImportError:
            self.skipTest("numpy not installed")
        source = self.write_fixture("regions.spe", spe3_bytes())
        metadata, frames = read(source)
        self.assertEqual(metadata.pixel_count, 3)
        self.assertEqual(frames[0].shape, (3,))
        self.assertEqual(frames[0].tolist(), [513, 1027, 1541])

    def test_create_numpy_uint16_and_float32(self):
        try:
            import numpy
        except ImportError:
            self.skipTest("numpy not installed")
        integer_frames = [
            numpy.array([[0, 1, 65535], [42, 1234, 32768]], dtype=numpy.uint16),
            numpy.array([[9, 8, 7], [6, 5, 4]], dtype=numpy.uint16),
        ]
        integer_path = self.root / "created-u16.spe"
        create(integer_path, integer_frames, 0.125, temperature_setpoint=-80.5)
        metadata, decoded = read(integer_path)
        self.assertEqual(metadata.major_version, 3)
        self.assertEqual(metadata.data_type, 3)
        self.assertEqual((metadata.width, metadata.height, metadata.frame_count), (3, 2, 2))
        self.assertEqual(metadata.exposure_seconds, 0.125)
        self.assertEqual(metadata.temperature_setpoint, -80.5)
        for expected, actual in zip(integer_frames, decoded):
            self.assertTrue(numpy.array_equal(expected, actual))
        float_frames = [
            numpy.array([[0.0, -1.5], [2.25, 1000.0]], dtype=numpy.float32),
            numpy.array([[3.5, 4.75], [-0.125, 65535.0]], dtype=numpy.float32),
        ]
        float_path = self.root / "created-f32.spe"
        create(float_path, float_frames, 0.5)
        metadata, decoded = read(float_path)
        self.assertEqual(metadata.data_type, 0)
        self.assertEqual(metadata.exposure_seconds, 0.5)
        for expected, actual in zip(float_frames, decoded):
            self.assertTrue(numpy.array_equal(expected, actual))

    def test_create_plain_sequences_without_numpy(self):
        integer_frames = [
            [[0, 1, 65535], [42, 1234, 32768]],
            [[9, 8, 7], [6, 5, 4]],
        ]
        float_frames = [
            [[0.0, -1.5], [2.25, 1000.0]],
            [[3.5, 4.75], [-0.125, 65535.0]],
        ]
        with mock.patch.dict(sys.modules, {"numpy": None}):
            create(self.root / "plain-u16.spe", integer_frames, 0.125, temperature_setpoint=-70.25)
            integer_metadata, decoded_integers = read(self.root / "plain-u16.spe")
            create(self.root / "plain-f32.spe", float_frames, 0.5)
            float_metadata, decoded_floats = read(self.root / "plain-f32.spe")
        self.assertEqual(integer_metadata.data_type, 3)
        self.assertEqual(integer_metadata.temperature_setpoint, -70.25)
        self.assertEqual(
            decoded_integers,
            [array.array("H", [value for row in frame for value in row]) for frame in integer_frames],
        )
        self.assertEqual(float_metadata.data_type, 0)
        self.assertEqual(
            decoded_floats,
            [array.array("f", [value for row in frame for value in row]) for frame in float_frames],
        )

    def test_create_rejects_empty_and_inconsistent_frames(self):
        with mock.patch.dict(sys.modules, {"numpy": None}):
            with self.assertRaises(ValueError):
                create(self.root / "empty.spe", [], 1.0)
            with self.assertRaises(ValueError):
                create(self.root / "ragged.spe", [[[1, 2], [3]]], 1.0)
            with self.assertRaises(ValueError):
                create(self.root / "mismatch.spe", [[[1, 2]], [[3, 4], [5, 6]]], 1.0)
            with self.assertRaises(ValueError):
                create(self.root / "shape.spe", [[1, 2, 3]], 1.0)

    @unittest.skipUnless(os.environ.get("OPENST133_SPE_FIXTURE"), "optional SPE fixture not selected")
    def test_supplied_fixture(self):
        source = pathlib.Path(os.environ["OPENST133_SPE_FIXTURE"])
        _, frames = read(source)
        with SpeFile(source) as spe:
            self.assertGreater(spe.info.frame_count, 0)
            self.assertGreater(spe.info.pixel_count, 0)
            self.assertEqual(len(frames), spe.info.frame_count)
            for frame in frames:
                self.assertEqual(getattr(frame, "size", len(frame)), spe.info.pixel_count)
            print(
                "major_version={} header_version={} width={} height={} "
                "data_type={} frames={} exposure_seconds={} date={}".format(
                    spe.info.major_version,
                    spe.info.header_version,
                    spe.info.width,
                    spe.info.height,
                    spe.info.data_type,
                    spe.info.frame_count,
                    spe.info.exposure_seconds,
                    spe.info.date,
                )
            )


if __name__ == "__main__":
    unittest.main()
