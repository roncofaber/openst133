# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT
"""Build all eight ST-133 startup tables from the WinSpec files PI133B.DAT and PICHIPDB.dll.

Usage: openst133 startup-data (DIR | --dat PI133B.DAT --dll PICHIPDB.dll) [--detector 98] [--output FILE]
DIR is a folder containing both files; --dat and --dll name them individually. Obtaining them from your licensed
WinSpec is up to you. The tool reads the camera setup tables and writes a SHA-256 sidecar beside the startup data. The generated file contains vendor-derived
data: keep it private and never commit or redistribute it.
"""

import argparse
from dataclasses import dataclass
import hashlib
import os
from pathlib import Path
import struct
import sys

MAGIC = b"OSTSTART"
FORMAT_VERSION = 1
DETECTOR_ID = 98


def pack_tables(tables):
    out = bytearray(MAGIC + struct.pack("<II", FORMAT_VERSION, len(tables)))
    for command, args in tables:
        out += struct.pack("<HHI", command, 0, len(args)) + args
    return bytes(out)


def default_output():
    override = os.environ.get("OPENST133_STARTUP_DATA")
    if override:
        return override
    root = os.environ.get("OPENST133_DATA")
    if root:
        return root + "/firmware/st133-startup.bin"
    if os.name == "nt":
        base = os.environ.get("APPDATA")
        if base:
            return base + "/openst133/firmware/st133-startup.bin"
        return "./openst133/firmware/st133-startup.bin"
    home = os.environ.get("HOME")
    if home is None:
        home = "."
    return home + "/.local/share/openst133/firmware/st133-startup.bin"


def inside_checkout(path):
    for parent in Path(__file__).resolve().parents:
        if (parent / "pyproject.toml").is_file() and (parent / "CMakeLists.txt").is_file():
            try:
                Path(path).resolve().relative_to(parent)
            except ValueError:
                return False
            return True
    return False


PATTERN_ORDER = (
    0, 1, 2, 3, 4, 6, 10, 11,
    7, 12, 13, 14, 5, 8, 9, 15,
    4, 4, 4, 4, 4, 4, 16, 4,
    4, 18, 4, 17, 4, 4, 4, 19,
)
FINAL_COPIES = ((26, 88), (38, 100), (46, 108), (62, 124))


class StartupDataError(ValueError):
    pass


@dataclass(frozen=True)
class DetectorGeometry:
    detector_id: int
    name: str
    rows: int
    columns: int
    serial_positions: int
    leading_serial: int = 8
    trailing_serial: int = 8


@dataclass(frozen=True)
class DetectorCatalog:
    name_stride: int
    information_count: int
    names: dict
    information: dict
    timing_ids: frozenset


def word(data, offset, signed=False):
    if offset < 0 or offset + 2 > len(data):
        raise StartupDataError("unexpected end of data at byte offset %d" % offset)
    return struct.unpack_from("<h" if signed else "<H", data, offset)[0]


def take_words(data, offset, count, signed=False):
    size = 2 * count
    if offset < 0 or count < 0 or offset + size > len(data):
        raise StartupDataError("unexpected end of data at byte offset %d" % offset)
    return list(struct.unpack_from("<%d%s" % (count, "h" if signed else "H"), data, offset)), offset + size


def dword(data, offset):
    if offset < 0 or offset + 4 > len(data):
        raise StartupDataError("unexpected end of data at byte offset %d" % offset)
    return struct.unpack_from("<I", data, offset)[0]


def parse_resource(data):
    if word(data, 0, signed=True) != -3 or word(data, 4, signed=True) != 1:
        raise StartupDataError("PI133B.DAT is not version-3 kind-1")
    directory_offset = word(data, 6) + 8
    group_count = word(data, 8)
    offset = 10
    groups = []
    for _ in range(group_count):
        detector_count = word(data, offset)
        offset += 2
        detector_ids, offset = take_words(data, offset, detector_count)
        speed_count = word(data, offset)
        offset += 2
        speeds = []
        for _ in range(speed_count):
            values, offset = take_words(data, offset, 8)
            speeds.append((values[0], values[1:]))
        common, offset = take_words(data, offset, 8)
        flags, offset = take_words(data, offset, 11)
        groups.append({"detectors": detector_ids, "speeds": speeds, "common": common, "flags": flags})
    if offset != directory_offset:
        raise StartupDataError(
            "PI133B.DAT detector groups end at byte %d, directory offset is %d" % (offset, directory_offset)
        )
    pattern_count = word(data, offset)
    offset += 2
    patterns = {}
    for _ in range(pattern_count):
        pattern_id = word(data, offset)
        source_count = word(data, offset + 2, signed=True)
        offset += 4
        if source_count < 0:
            offset += 2
            source_count = word(data, offset)
            offset += 2
        descriptor_count = word(data, offset)
        offset += 2
        if offset + source_count > len(data):
            raise StartupDataError("PI133B.DAT pattern %d has truncated source data" % pattern_id)
        source = data[offset:offset + source_count]
        offset += source_count
        descriptors, offset = take_words(data, offset, descriptor_count, signed=True)
        if pattern_id in patterns:
            raise StartupDataError("PI133B.DAT contains duplicate pattern ID %d" % pattern_id)
        patterns[pattern_id] = (source, descriptors)
    if offset != len(data):
        raise StartupDataError("PI133B.DAT patterns end at byte %d, file size is %d" % (offset, len(data)))
    return groups, patterns


def expand_pattern(source, descriptors):
    output = bytearray()
    source_index = 0
    descriptor_index = 0
    while source_index < len(source):
        if descriptor_index >= len(descriptors):
            raise StartupDataError("pattern source has too few repetition descriptors")
        repeat = descriptors[descriptor_index]
        descriptor_index += 1
        if repeat >= 0:
            output.extend(bytes((source[source_index],)) * repeat)
            source_index += 1
            continue
        if descriptor_index >= len(descriptors):
            raise StartupDataError("pattern component group has no width")
        width = descriptors[descriptor_index]
        descriptor_index += 1
        if width <= 0 or source_index + width > len(source) or descriptor_index + width > len(descriptors):
            raise StartupDataError("pattern component group has an invalid width")
        counts = descriptors[descriptor_index:descriptor_index + width]
        if any(count < 0 for count in counts):
            raise StartupDataError("pattern component has a negative repeat count")
        component = source[source_index:source_index + width]
        for _ in range(-repeat):
            for value, count in zip(component, counts):
                output.extend(bytes((value,)) * count)
        descriptor_index += width
        source_index += width
        if len(output) > 2048:
            raise StartupDataError("expanded pattern exceeds its 2048-byte segment")
    if descriptor_index != len(descriptors):
        raise StartupDataError("pattern has unused repetition descriptors")
    if len(output) > 2048:
        raise StartupDataError("expanded pattern exceeds its 2048-byte segment")
    return bytes(output)


def apply_final_copies(generated):
    result = bytearray(generated)
    if len(result) != 65536:
        raise StartupDataError("setup generation requires 65536 bytes before final copies")
    for destination, source in FINAL_COPIES:
        destination *= 512
        source *= 512
        result[destination:destination + 1024] = generated[source:source + 1024]
    return result


def setup_table(groups, patterns, detector_id, selector, mode=0):
    if any(group["detectors"].count(detector_id) > 1 for group in groups):
        raise StartupDataError("detector ID %d is repeated in a PI133B.DAT group" % detector_id)
    matches = [group for group in groups if detector_id in group["detectors"]]
    if len(matches) != 1:
        raise StartupDataError("detector ID %d occurs in %d PI133B.DAT groups" % (detector_id, len(matches)))
    group = matches[0]
    if not group["speeds"]:
        raise StartupDataError("detector ID %d has no speed entries" % detector_id)
    selected_speeds = [entry for entry in group["speeds"] if entry[0] == selector]
    if len(selected_speeds) > 1:
        raise StartupDataError("speed selector %d is ambiguous for detector ID %d" % (selector, detector_id))
    selected = selected_speeds[0] if selected_speeds else group["speeds"][-1]
    if mode not in (0, 1):
        raise StartupDataError("unsupported setup mode %d; supported modes are 0 and 1" % mode)
    common = group["common"][:4] if mode == 1 else group["common"][4:]
    identifiers = selected[1][:6] + common + group["flags"][:10]
    generated = bytearray()
    for index in PATTERN_ORDER:
        pattern_id = identifiers[index]
        if pattern_id not in patterns:
            raise StartupDataError("pattern ID %d selected for detector %d is absent" % (pattern_id, detector_id))
        source, descriptors = patterns[pattern_id]
        expanded = expand_pattern(source, descriptors)
        generated.extend(expanded)
        generated.extend(b"\xff" * (2048 - len(expanded)))
    return bytes(apply_final_copies(generated)[:32768])


def pe_sections(data):
    if len(data) < 64 or data[:2] != b"MZ":
        raise StartupDataError("PICHIPDB.dll has no DOS signature")
    pe_offset = dword(data, 0x3C)
    if pe_offset + 24 > len(data) or data[pe_offset:pe_offset + 4] != b"PE\0\0":
        raise StartupDataError("PICHIPDB.dll has no complete PE signature and COFF header")
    section_count = word(data, pe_offset + 6)
    optional_size = word(data, pe_offset + 20)
    optional_offset = pe_offset + 24
    if optional_size < 2 or optional_offset + optional_size > len(data):
        raise StartupDataError("PICHIPDB.dll has a truncated optional header")
    if word(data, optional_offset) != 0x10B:
        raise StartupDataError("PICHIPDB.dll is not a PE32 file")
    section_table = optional_offset + optional_size
    if section_table + section_count * 40 > len(data):
        raise StartupDataError("PICHIPDB.dll has a truncated section table")
    sections = []
    for index in range(section_count):
        entry = section_table + index * 40
        sections.append((
            data[entry:entry + 8],
            dword(data, entry + 12),
            dword(data, entry + 16),
            dword(data, entry + 20),
        ))
    return optional_offset, optional_size, sections


def rva_file_offset(sections, rva, length):
    for _, virtual_address, raw_size, raw_offset in sections:
        if virtual_address <= rva and rva + length <= virtual_address + raw_size:
            return raw_offset + rva - virtual_address
    return None


def relocated_file_offsets(data, optional_offset, optional_size, sections):
    if optional_size < 96 + 6 * 8 or dword(data, optional_offset + 92) <= 5:
        return set()
    directory_rva = dword(data, optional_offset + 96 + 5 * 8)
    directory_size = dword(data, optional_offset + 96 + 5 * 8 + 4)
    if directory_size == 0:
        return set()
    position = rva_file_offset(sections, directory_rva, directory_size)
    if position is None or position + directory_size > len(data):
        raise StartupDataError("PICHIPDB.dll base relocation directory is outside the file sections")
    end = position + directory_size
    covered = set()
    while position < end:
        if position + 8 > end:
            raise StartupDataError("PICHIPDB.dll has a truncated base relocation block")
        page = dword(data, position)
        block_size = dword(data, position + 4)
        if block_size < 8 or block_size % 2 or position + block_size > end:
            raise StartupDataError("PICHIPDB.dll has an invalid base relocation block size")
        for entry_offset in range(position + 8, position + block_size, 2):
            entry = word(data, entry_offset)
            kind = entry >> 12
            if kind == 0:
                continue
            if kind != 3:
                raise StartupDataError("PICHIPDB.dll has unsupported base relocation type %d" % kind)
            target = rva_file_offset(sections, page + (entry & 0xFFF), 4)
            if target is not None:
                covered.update(range(target, target + 4))
        position += block_size
    return covered


def pe_data_sections(data):
    optional_offset, optional_size, sections = pe_sections(data)
    for name, _, raw_size, raw_offset in sections:
        if name == b".data\0\0\0" and raw_offset + raw_size > len(data):
            raise StartupDataError("PICHIPDB.dll .data section is truncated")
    relocated = relocated_file_offsets(data, optional_offset, optional_size, sections)
    result = []
    for name, _, raw_size, raw_offset in sections:
        if name != b".data\0\0\0":
            continue
        mask = bytearray(raw_size)
        for offset in relocated:
            if raw_offset <= offset < raw_offset + raw_size:
                mask[offset - raw_offset] = 1
        result.append((data[raw_offset:raw_offset + raw_size], bytes(mask)))
    return result


def holds_load_address(relocated, offset, size):
    return relocated.find(1, offset, offset + size) >= 0


def catalog_name_record(section, relocated, offset, stride):
    if offset < 0 or offset + stride > len(section) or holds_load_address(relocated, offset, stride):
        return None
    detector_id = word(section, offset)
    if detector_id == 0:
        return None
    record = section[offset + 2:offset + stride]
    terminator = record.find(b"\0")
    if terminator < 0 or any(value < 0x20 or value > 0x7E for value in record[:terminator]):
        return None
    return detector_id, record[:terminator].decode("ascii")


def catalog_identifiers(section, relocated, offset, stride, count):
    identifiers = []
    for index in range(count):
        record = offset + index * stride
        if holds_load_address(relocated, record, stride):
            return None
        detector_id = word(section, record)
        if detector_id == 0 or detector_id in identifiers:
            return None
        identifiers.append(detector_id)
    return identifiers


def recognize_catalog(data):
    candidates = []
    for section, relocated in pe_data_sections(data):
        for start in range(0, len(section), 16):
            for stride in (34, 98):
                if catalog_name_record(section, relocated, start - stride, stride) is not None:
                    continue
                names = {}
                offset = start
                valid = True
                while True:
                    record = catalog_name_record(section, relocated, offset, stride)
                    if record is None:
                        break
                    detector_id, name = record
                    if detector_id in names:
                        valid = False
                        break
                    names[detector_id] = name
                    offset += stride
                if not valid or not names:
                    continue
                information_offset = (offset + 7) & ~7
                maximum_count = (len(section) - information_offset) // 64
                for count in range(1, maximum_count + 1):
                    information_end = information_offset + count * 64
                    timing_offset = (information_end + 7) & ~7
                    if timing_offset + count * 80 > len(section):
                        break
                    information_ids = catalog_identifiers(section, relocated, information_offset, 64, count)
                    timing_ids = catalog_identifiers(section, relocated, timing_offset, 80, count)
                    if information_ids is None or timing_ids is None:
                        continue
                    if set(information_ids) != set(timing_ids) or not set(names).issubset(information_ids):
                        continue
                    information = {}
                    for index in range(count):
                        slots, _ = take_words(section, information_offset + index * 64, 32)
                        information[slots[0]] = tuple(slots)
                    candidates.append(DetectorCatalog(
                        stride, count, dict(names), information, frozenset(timing_ids)
                    ))
    if not candidates:
        raise StartupDataError("PICHIPDB.dll has no supported detector catalog structure")
    if len(candidates) != 1:
        raise StartupDataError(
            "PICHIPDB.dll detector catalog layout is ambiguous (%d complete candidates)" % len(candidates)
        )
    return candidates[0]


def read_detector(data, detector_id):
    catalog = recognize_catalog(data)
    slots = catalog.information.get(detector_id)
    if slots is None or detector_id not in catalog.timing_ids:
        raise StartupDataError("detector ID %d was not found in PICHIPDB.dll" % detector_id)
    rows = slots[1] * slots[9]
    columns = slots[2] * slots[8]
    serial_positions = slots[5] + columns + slots[6]
    if min(rows, columns, serial_positions, slots[8], slots[9]) <= 0 or max(
        rows, columns, serial_positions
    ) > 0xFFFF:
        raise StartupDataError("detector ID %d has invalid or unsupported geometry" % detector_id)
    return DetectorGeometry(
        detector_id,
        catalog.names.get(detector_id, ""),
        rows,
        columns,
        serial_positions,
        leading_serial=slots[5],
        trailing_serial=slots[6],
    )


def u16(value):
    if not 0 <= value <= 0xFFFF:
        raise StartupDataError("16-bit operation count is out of range: %d" % value)
    return struct.pack("<H", value)


def parallel(count):
    if count <= 0:
        return b""
    if count <= 4:
        return b"\x50" * count
    return b"\x54\x56" + u16(count - 2)


def serial(count):
    if count <= 0:
        return b""
    if count <= 4:
        return b"\x10" * count
    return b"\x14\x16" + u16(count - 2)


def repeat(count, body):
    if count <= 1:
        return body
    return b"\x44" + body + b"\x46" + u16(count - 1)


def active_samples(count, binning):
    if binning == 1:
        return b"\x00" * count
    return (b"\x20" * (binning - 1) + b"\x30") * count


def discarded_rows(count, serial_positions):
    output = bytearray()

    def residual(rows):
        output.extend(parallel(rows))
        output.extend(serial(serial_positions))

    def pairs(count):
        if count == 1:
            output.extend(b"\x50\x50" + serial(serial_positions))
        elif count > 1:
            output.extend(repeat(count, b"\x50\x50" + b"\x10" * serial_positions))

    if count <= 0:
        return b""
    if count <= 12:
        pair_count, remainder = divmod(count, 2)
        if remainder == 0:
            pair_count -= 1
            remainder = 2
        pairs(pair_count)
        residual(remainder)
        return bytes(output)
    remainder = count - 10
    group = 4
    while 2 * group < remainder:
        remainder -= group
        group *= 2
    if remainder > group:
        residual((remainder + 1) // 2)
        residual(remainder // 2)
    else:
        residual(remainder)
    while group // 2 > 2:
        group //= 2
        residual(group)
    pairs(5)
    return bytes(output)


def ordinary_program(geometry, x, y, width, height, horizontal_binning, vertical_binning):
    if min(x, y) < 0 or min(width, height, horizontal_binning, vertical_binning) <= 0:
        raise StartupDataError("region coordinates and binning are invalid")
    if x + width > geometry.columns or y + height > geometry.rows:
        raise StartupDataError("requested region exceeds detector %d geometry" % geometry.detector_id)
    s1, s2, p1, p2 = x + 1, x + width, y + 1, y + height
    s2 = s1 - 1 + horizontal_binning * max(1, (s2 - s1 + 1) // horizontal_binning)
    p2 = p1 - 1 + vertical_binning * max(1, (p2 - p1 + 1) // vertical_binning)
    columns = (s2 - s1 + 1) // horizontal_binning
    rows = (p2 - p1 + 1) // vertical_binning
    leading = geometry.leading_serial + s1 - 1
    trailing = geometry.trailing_serial + geometry.columns - s2
    if leading + columns * horizontal_binning + trailing != geometry.serial_positions:
        raise StartupDataError("region does not fill the complete serial line")
    body = (
        parallel(vertical_binning) + serial(leading)
        + active_samples(columns, horizontal_binning) + serial(trailing)
    )
    literal_body = (
        b"\x50" * vertical_binning + b"\x10" * leading
        + active_samples(columns, horizontal_binning) + b"\x10" * trailing
    )
    output = bytearray(b"\x40")
    leading_rows = p1 - 1
    if leading_rows == 0:
        output.extend(serial(geometry.serial_positions))
    else:
        output.extend(discarded_rows(leading_rows, geometry.serial_positions))
    output.extend(repeat(rows, literal_body) if rows > 1 else body)
    output.extend(discarded_rows(geometry.rows - p2, geometry.serial_positions))
    output.extend(b"\x70\x70")
    return bytes(output)


def clear_programs(serial_positions):
    complete_skip = serial(serial_positions)

    def group(count):
        return parallel(count) + complete_skip

    def finish(body):
        if len(body) + 3 > 512:
            raise StartupDataError("clear program exceeds the 512-argument setup limit")
        result = b"\x40" + body + b"\x70\x70"
        return result + b"\x70" * (512 - len(result))

    pair_body = b"\x50\x50" + complete_skip
    partitioned = finish(group(31) + group(31) + group(16) + group(8) + group(4) + repeat(5, pair_body))
    repeated = finish(repeat(50, pair_body))
    return partitioned, repeated


def build_tables(resource_data, database_data, detector_id=DETECTOR_ID):
    if detector_id != DETECTOR_ID:
        raise StartupDataError(
            "unsupported detector ID %d; installation-derived startup is validated only for detector 98" % detector_id
        )
    groups, patterns = parse_resource(resource_data)
    geometry = read_detector(database_data, detector_id)
    if (geometry.rows, geometry.columns, geometry.serial_positions) != (100, 1340, 1356):
        raise StartupDataError(
            "detector 98 geometry %dx%d with %d serial positions is unsupported"
            % (geometry.rows, geometry.columns, geometry.serial_positions)
        )
    partitioned, repeated = clear_programs(geometry.serial_positions)
    tables = [
        (0x48, setup_table(groups, patterns, detector_id, 11)),
        (0x20, ordinary_program(geometry, 0, 0, 1340, 100, 1, 1)),
        (0x20, partitioned),
        (0x20, repeated),
        (0x20, ordinary_program(geometry, 0, 0, 1340, 100, 1, 100)),
        (0x20, ordinary_program(geometry, 100, 20, 200, 40, 1, 1)),
        (0x20, ordinary_program(geometry, 0, 0, 1340, 100, 2, 1)),
        (0x48, setup_table(groups, patterns, detector_id, 6)),
    ]
    return tables


def read_source(path):
    try:
        return path.read_bytes()
    except OSError as error:
        raise StartupDataError("cannot read %s: %s" % (path, error.strerror or error)) from error


def add_arguments(parser):
    parser.add_argument("directory", type=Path, nargs="?", help="folder containing PI133B.DAT and PICHIPDB.dll")
    parser.add_argument("--dat", type=Path, help="path to PI133B.DAT (instead of a folder)")
    parser.add_argument("--dll", type=Path, help="path to PICHIPDB.dll (instead of a folder)")
    parser.add_argument("--detector", type=int, default=DETECTOR_ID, help="detector ID (validated: 98)")
    parser.add_argument("--output", default=None, help="output path (default: %s)" % default_output())


def source_files(directory=None, dat=None, dll=None):
    """Resolve the two WinSpec files from a folder, explicit paths, or a folder with one file overridden."""
    if directory is None and (dat is None or dll is None):
        raise StartupDataError("give a folder containing PI133B.DAT and PICHIPDB.dll, or both --dat and --dll")
    dat = Path(dat) if dat is not None else Path(directory) / "PI133B.DAT"
    dll = Path(dll) if dll is not None else Path(directory) / "PICHIPDB.dll"
    return dat, dll


def run(arguments):
    dat, dll = source_files(arguments.directory, getattr(arguments, "dat", None), getattr(arguments, "dll", None))
    output = Path(arguments.output or default_output())
    if inside_checkout(str(output)):
        raise StartupDataError("refusing to write vendor-derived data inside the repository: %s" % output)
    tables = build_tables(
        read_source(dat),
        read_source(dll),
        detector_id=arguments.detector,
    )
    data = pack_tables(tables)
    try:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(data)
        output.with_name(output.name + ".sha256").write_text(
            "%s  %s\n" % (hashlib.sha256(data).hexdigest(), output.name), encoding="ascii"
        )
    except OSError as error:
        raise StartupDataError("cannot write %s: %s" % (output, error.strerror or error)) from error
    print("wrote %s (%d bytes, %d tables); keep it private, do not redistribute" % (output, len(data), len(tables)))
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    add_arguments(parser)
    return run(parser.parse_args(argv))


if __name__ == "__main__":
    try:
        sys.exit(main())
    except StartupDataError as error:
        sys.exit("openst133 startup-data: error: %s" % error)
