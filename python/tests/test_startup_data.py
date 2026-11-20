#!/usr/bin/env python3
# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT
"""Tests for the installed-source startup builder using synthetic resources."""

import contextlib
import hashlib
import io
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from openst133 import startup as subject

ROOT = Path(__file__).resolve().parents[2]
EXPECTED_TABLE_HASHES = (
    "80952fd03d410501b977ed3cfa5f3f437fcf6a9c92c8e268b9c88c45dc0ad786",
    "531871bc62bd658f6c4a488f9b343aad66c88f2e790deb84636398754d132d9e",
    "9232dfdc05eecf3e73941521e776f05c40d027aa70ed66166d317d6a994acdf9",
    "6e36c2aca0d702a455357778db7724d40a7a7207ca195d892658ba47711bde4f",
    "d7a27641a3c4a89c849b3745647fa055d589fa34e7873f02d58238319aa29929",
    "be78dba51637a56f01df49923166bb4b8d8dd257da51242eb3a29aa5a21c7810",
    "a9914a6e175c59d931ae3d3453735520a3251c3efab3df4344040aec08084fd2",
    "e14dbc08569cf681d03debc271d865491e063a21ba92b5b7fd8030ca9ff9fefa",
)


def detector_geometry():
    return subject.DetectorGeometry(98, "EEV 100x1340B", 100, 1340, 1356)


def information_record(detector_id, rows=2, columns=3, column_multiplier=1, row_multiplier=1):
    slots = [0] * 32
    slots[0] = detector_id
    slots[1] = rows
    slots[2] = columns
    slots[5] = 8
    slots[6] = 8
    slots[8] = column_multiplier
    slots[9] = row_multiplier
    return struct.pack("<32H", *slots)


def catalog_data(stride, prefix=bytes(16)):
    data = bytearray(prefix)
    for detector_id, name in ((98, b"target"), (7, b"other")):
        record = bytearray(stride)
        struct.pack_into("<H", record, 0, detector_id)
        record[2:2 + len(name)] = name
        record[2 + len(name)] = 0
        data.extend(record)
    while len(data) % 8:
        data.append(0)
    data.extend(information_record(7))
    data.extend(information_record(55, column_multiplier=0, row_multiplier=0))
    data.extend(information_record(98, rows=100, columns=1340))
    while len(data) % 8:
        data.append(0)
    for detector_id in (55, 98, 7):
        record = bytearray(80)
        struct.pack_into("<H", record, 0, detector_id)
        data.extend(record)
    return bytes(data)


def relocation_block(page, entries):
    entries = list(entries) + [0] * (len(entries) % 2)
    return struct.pack("<II%dH" % len(entries), page, 8 + 2 * len(entries), *entries)


def catalog_fixture(strides=(34,), optional_magic=0x10B, trailing=b"", prefix=bytes(16), relocations=None):
    chunks = [(b".data\0\0\0", catalog_data(stride, prefix) + trailing) for stride in strides]
    if relocations is not None:
        chunks.append((b".reloc\0\0", relocations))
    pe_offset = 0x80
    optional_size = 0xE0
    optional_offset = pe_offset + 24
    section_table = optional_offset + optional_size
    header_size = 0x200
    data = bytearray(header_size)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, pe_offset)
    data[pe_offset:pe_offset + 4] = b"PE\0\0"
    struct.pack_into("<H", data, pe_offset + 4, 0x14C)
    struct.pack_into("<H", data, pe_offset + 6, len(chunks))
    struct.pack_into("<H", data, pe_offset + 20, optional_size)
    struct.pack_into("<H", data, optional_offset, optional_magic)
    struct.pack_into("<I", data, optional_offset + 92, 16)
    raw_offset = header_size
    for index, (name, chunk) in enumerate(chunks):
        entry = section_table + index * 40
        virtual_address = 0x10000 * (index + 1)
        data[entry:entry + 8] = name
        struct.pack_into("<IIII", data, entry + 8, len(chunk), virtual_address, len(chunk), raw_offset)
        if name == b".reloc\0\0":
            struct.pack_into("<II", data, optional_offset + 96 + 5 * 8, virtual_address, len(chunk))
        data.extend(chunk)
        raw_offset += len(chunk)
    return bytes(data)


def pointer_prefix():
    prefix = bytearray(48)
    struct.pack_into("<I", prefix, 12, 0x10003223)
    return bytes(prefix)


def synthetic_resource():
    data = bytearray(struct.pack("<hhhHHH", -3, 0, 1, 0, 1, 1))
    data.extend(struct.pack("<HH", 98, 2))
    for selector in (11, 6):
        data.extend(struct.pack("<8H", selector, *range(7)))
    data.extend(struct.pack("<8H", *range(6, 14)))
    data.extend(struct.pack("<11H", *range(14, 25)))
    struct.pack_into("<H", data, 6, len(data) - 8)
    data.extend(struct.pack("<H", 24))
    for pattern_id in range(24):
        data.extend(struct.pack("<Hhh", pattern_id, 1, 1))
        data.append(pattern_id + 1)
        data.extend(struct.pack("<h", 2048))
    return bytes(data)


class ResourceStructureTests(unittest.TestCase):
    def test_unknown_version_or_controller_kind_is_rejected(self):
        for offset, value in ((0, -4), (4, 2)):
            with self.subTest(offset=offset):
                data = bytearray(struct.pack("<hhhHH", -3, 0, 1, 0, 0))
                struct.pack_into("<h", data, offset, value)
                with self.assertRaisesRegex(subject.StartupDataError, "version-3 kind-1"):
                    subject.parse_resource(data)

    def test_unknown_detector_id_is_unsupported(self):
        with self.assertRaisesRegex(subject.StartupDataError, "detector ID 99.*detector 98"):
            subject.build_tables(b"resource", b"database", detector_id=99)

    def test_duplicate_speed_selector_is_ambiguous(self):
        group = {
            "detectors": [98],
            "speeds": [(11, [0] * 7), (11, [1] * 7)],
            "common": [0] * 8,
            "flags": [0] * 11,
        }
        with self.assertRaisesRegex(subject.StartupDataError, "speed selector 11.*ambiguous"):
            subject.setup_table([group], {}, 98, 11)

    def test_repeated_detector_in_one_group_is_rejected(self):
        group = {"detectors": [98, 98], "speeds": [], "common": [0] * 8, "flags": [0] * 11}
        with self.assertRaisesRegex(subject.StartupDataError, "detector ID 98.*repeated"):
            subject.setup_table([group], {}, 98, 11)


class CatalogStructureTests(unittest.TestCase):
    def test_both_name_strides_select_geometry_by_detector_id(self):
        for stride in (34, 98):
            with self.subTest(stride=stride):
                geometry = subject.read_detector(catalog_fixture((stride,)), 98)
                self.assertEqual(98, geometry.detector_id)
                self.assertEqual("target", geometry.name)
                self.assertEqual(100, geometry.rows)
                self.assertEqual(1340, geometry.columns)
                self.assertEqual(1356, geometry.serial_positions)
                self.assertEqual(8, geometry.leading_serial)
                self.assertEqual(8, geometry.trailing_serial)

    def test_unselected_zero_multipliers_do_not_invalidate_catalog(self):
        data = catalog_fixture()
        self.assertEqual(1340, subject.read_detector(data, 98).columns)
        with self.assertRaisesRegex(subject.StartupDataError, "detector ID 55.*geometry"):
            subject.read_detector(data, 55)

    def test_trailing_section_data_does_not_change_layout(self):
        data = catalog_fixture(trailing=b"opaque trailing section data")
        self.assertEqual(1340, subject.read_detector(data, 98).columns)

    def test_multiple_complete_catalog_layouts_are_ambiguous(self):
        with self.assertRaisesRegex(subject.StartupDataError, "ambiguous"):
            subject.read_detector(catalog_fixture((34, 34)), 98)

    def test_unknown_name_stride_is_unsupported(self):
        with self.assertRaisesRegex(subject.StartupDataError, "no supported detector catalog"):
            subject.read_detector(catalog_fixture((40,)), 98)

    def test_pe32_optional_header_is_required(self):
        with self.assertRaisesRegex(subject.StartupDataError, "PE32"):
            subject.read_detector(catalog_fixture(optional_magic=0x20B), 98)

    def test_relocated_pointer_before_name_array_is_not_a_name_record(self):
        relocations = relocation_block(0x10000, [0x3000 | 12])
        geometry = subject.read_detector(catalog_fixture(prefix=pointer_prefix(), relocations=relocations), 98)
        self.assertEqual(1340, geometry.columns)

    def test_unrelocated_pointer_before_name_array_shadows_its_start(self):
        with self.assertRaisesRegex(subject.StartupDataError, "no supported detector catalog"):
            subject.read_detector(catalog_fixture(prefix=pointer_prefix()), 98)

    def test_relocated_catalog_record_is_rejected(self):
        relocations = relocation_block(0x10000, [0x3000 | 48])
        with self.assertRaisesRegex(subject.StartupDataError, "no supported detector catalog"):
            subject.read_detector(catalog_fixture(prefix=pointer_prefix(), relocations=relocations), 98)

    def test_unknown_relocation_type_is_unsupported(self):
        relocations = relocation_block(0x10000, [0xA000 | 12])
        with self.assertRaisesRegex(subject.StartupDataError, "relocation type 10"):
            subject.read_detector(catalog_fixture(prefix=pointer_prefix(), relocations=relocations), 98)

    def test_malformed_relocation_block_is_rejected(self):
        relocations = struct.pack("<II", 0x10000, 4)
        with self.assertRaisesRegex(subject.StartupDataError, "relocation block"):
            subject.read_detector(catalog_fixture(relocations=relocations), 98)

    def test_non_pe_data_is_rejected_before_catalog_search(self):
        with self.assertRaisesRegex(subject.StartupDataError, "DOS|PE"):
            subject.read_detector(b"not a detector catalog", 98)


class PatternExpansionTests(unittest.TestCase):
    def test_signed_descriptors_expand_literal_and_grouped_components(self):
        expanded = subject.expand_pattern(bytes((1, 2, 3)), (2, -2, 2, 1, 2))
        self.assertEqual(bytes((1, 1, 2, 3, 3, 2, 3, 3)), expanded)

    def test_final_copies_use_512_byte_offsets_for_1024_byte_copies(self):
        generated = bytearray(65536)
        for block in range(128):
            generated[block * 512:(block + 1) * 512] = bytes((block,)) * 512
        result = subject.apply_final_copies(generated)
        for destination, source in ((26, 88), (38, 100), (46, 108), (62, 124)):
            with self.subTest(destination=destination):
                self.assertEqual(65536, len(result))
                self.assertEqual(
                    generated[source * 512:source * 512 + 1024],
                    result[destination * 512:destination * 512 + 1024],
                )


class ScanProgramTests(unittest.TestCase):
    def assert_program(self, expected_hash, expected_length, *region):
        program = subject.ordinary_program(detector_geometry(), *region)
        self.assertEqual(expected_length, len(program))
        self.assertEqual(expected_hash, hashlib.sha256(program).hexdigest())

    def test_full_frame(self):
        self.assert_program(EXPECTED_TABLE_HASHES[1], 1368, 0, 0, 1340, 100, 1, 1)

    def test_full_vertical_binning(self):
        self.assert_program(EXPECTED_TABLE_HASHES[4], 1359, 0, 0, 1340, 100, 1, 100)

    def test_rectangular_roi(self):
        self.assert_program(EXPECTED_TABLE_HASHES[5], 4136, 100, 20, 200, 40, 1, 1)

    def test_horizontal_binning(self):
        self.assert_program(EXPECTED_TABLE_HASHES[6], 1368, 0, 0, 1340, 100, 2, 1)


class ClearProgramTests(unittest.TestCase):
    def test_partitioned_clear(self):
        partitioned, _ = subject.clear_programs(1356)
        self.assertEqual(512, len(partitioned))
        self.assertEqual(EXPECTED_TABLE_HASHES[2], hashlib.sha256(partitioned).hexdigest())

    def test_repeated_pair_clear(self):
        _, repeated = subject.clear_programs(1356)
        self.assertEqual(512, len(repeated))
        self.assertEqual(EXPECTED_TABLE_HASHES[3], hashlib.sha256(repeated).hexdigest())


class InstalledSourceTests(unittest.TestCase):
    def test_explicit_files_match_folder_input(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary) / "WinSpec"
            folder.mkdir()
            self.write_synthetic_installation(folder)
            loose = Path(temporary) / "loose"
            loose.mkdir()
            (loose / "dat.bin").write_bytes((folder / "PI133B.DAT").read_bytes())
            (loose / "dll.bin").write_bytes((folder / "PICHIPDB.dll").read_bytes())
            from_folder, from_files = Path(temporary) / "a.bin", Path(temporary) / "b.bin"
            with contextlib.redirect_stdout(io.StringIO()):
                subject.main([str(folder), "--output", str(from_folder)])
                subject.main(["--dat", str(loose / "dat.bin"), "--dll", str(loose / "dll.bin"), "--output", str(from_files)])
            self.assertEqual(from_folder.read_bytes(), from_files.read_bytes())

    def test_missing_source_is_refused(self):
        with self.assertRaises(subject.StartupDataError):
            subject.main(["--dat", "only-one.dat"])

    def write_synthetic_installation(self, directory):
        (directory / "PI133B.DAT").write_bytes(synthetic_resource())
        (directory / "PICHIPDB.dll").write_bytes(catalog_fixture((34,)))

    def test_installed_folder_writes_expected_tables_and_sidecar(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary) / "WinSpec"
            directory.mkdir()
            self.write_synthetic_installation(directory)
            output = Path(temporary) / "st133-startup.bin"
            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                result = subject.main([str(directory), "--output", str(output)])
            self.assertEqual(0, result)
            tables = subject.build_tables(
                (directory / "PI133B.DAT").read_bytes(),
                (directory / "PICHIPDB.dll").read_bytes(),
            )
            packed = output.read_bytes()
            self.assertEqual(subject.pack_tables(tables), packed)
            self.assertEqual(8, len(tables))
            self.assertEqual(
                (32768, 1368, 512, 512, 1359, 4136, 1368, 32768),
                tuple(len(args) for _, args in tables),
            )
            self.assertIn("keep it private, do not redistribute", stdout.getvalue())
            sidecar = output.with_name(output.name + ".sha256").read_text(encoding="ascii")
            self.assertEqual("%s  %s\n" % (hashlib.sha256(packed).hexdigest(), output.name), sidecar)

    def test_pack_tables_has_stable_little_endian_format(self):
        tables = [(0x48, b"\x01\x02"), (0x20, b"\xaa")]
        expected = (
            b"OSTSTART" + struct.pack("<II", 1, 2)
            + struct.pack("<HHI", 0x48, 0, 2) + b"\x01\x02"
            + struct.pack("<HHI", 0x20, 0, 1) + b"\xaa"
        )
        self.assertEqual(expected, subject.pack_tables(tables))

    def test_help_describes_the_two_input_files(self):
        stdout = io.StringIO()
        with contextlib.redirect_stdout(stdout), self.assertRaises(SystemExit) as exit_status:
            subject.main(["--help"])
        self.assertEqual(0, exit_status.exception.code)
        help_text = stdout.getvalue()
        self.assertIn("--dat", help_text)
        self.assertIn("--dll", help_text)
        self.assertNotIn("installer", help_text)

    def test_checkout_guard_rejects_repository_output(self):
        output = ROOT / "startup-test-output.bin"
        self.assertTrue(subject.inside_checkout(str(output)))
        with self.assertRaisesRegex(subject.StartupDataError, "refusing to write"):
            subject.main([str(ROOT), "--output", str(output)])
        self.assertFalse(output.exists())
        with tempfile.TemporaryDirectory() as temporary:
            self.assertFalse(subject.inside_checkout(str(Path(temporary) / "startup.bin")))

    def test_data_root_controls_default_output(self):
        with mock.patch.dict(os.environ, {"OPENST133_DATA": "/tmp/openst133-data"}, clear=True):
            self.assertEqual(
                os.path.join("/tmp/openst133-data", "firmware", "st133-startup.bin"),
                subject.default_output(),
            )


if __name__ == "__main__":
    unittest.main()
