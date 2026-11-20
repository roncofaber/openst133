# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT

import ctypes
import dataclasses
import pathlib
import re
import types
import unittest
from unittest import mock


from openst133 import device
from openst133.camera import RealCamera
from openst133.device import CameraOpenOptions, Device, NativeCamera, StreamStats

HEADER = pathlib.Path(__file__).resolve().parents[2] / "include" / "openst133.h"

CAMERA_COUNTERPARTS = {
    "ost_camera_open_options_default": "CameraOpenOptions",
    "ost_camera_open": "NativeCamera.__init__",
    "ost_camera_open_with_options": "NativeCamera.__init__",
    "ost_camera_open_error": "NativeCamera.__init__",
    "ost_camera_close": "NativeCamera.close",
    "ost_camera_query_support": "NativeCamera.supports",
    "ost_camera_get_target_source": "NativeCamera.target_source",
    "ost_camera_last_error": "NativeCamera.last_error",
    "ost_camera_get_geometry": "NativeCamera.geometry",
    "ost_camera_get_identity": "NativeCamera.identity",
    "ost_camera_get_temperature": "NativeCamera.get_temperature",
    "ost_camera_set_temperature": "NativeCamera.set_temperature",
    "ost_camera_set_exposure": "NativeCamera.set_exposure_ms",
    "ost_camera_get_exposure": "NativeCamera.get_exposure_ms",
    "ost_camera_get_effective_exposure": "NativeCamera.get_effective_exposure_ms",
    "ost_camera_set_shutter": "NativeCamera.set_shutter",
    "ost_camera_get_shutter": "NativeCamera.get_shutter",
    "ost_camera_set_gain": "NativeCamera.set_gain",
    "ost_camera_get_gain": "NativeCamera.get_gain",
    "ost_camera_set_accumulations": "NativeCamera.set_accumulations",
    "ost_camera_get_accumulations": "NativeCamera.get_accumulations",
    "ost_camera_set_readout": "NativeCamera.set_readout",
    "ost_camera_set_readout_rate": "NativeCamera.set_readout_rate",
    "ost_camera_get_readout": "NativeCamera.get_readout",
    "ost_camera_set_full_vertical_binning": "NativeCamera.set_full_vertical_binning",
    "ost_camera_acquire": "NativeCamera.acquire_into",
    "ost_camera_stream": "NativeCamera.stream",
    "ost_camera_abort": "NativeCamera.abort",
}

CTYPES_EXCLUSIONS = {
    "ost_camera_open_options_default": "CameraOpenOptions() supplies target_set=0 and target_celsius=0 locally",
}


def _enum(name):
    match = re.search(rf"typedef enum {name}\s*\{{(.*?)\}}", HEADER.read_text(), re.DOTALL)
    if match is None:
        raise AssertionError(f"C enum {name} was not found")
    values = {
        name: int(value)
        for name, value in re.findall(r"^\s*([A-Z][A-Z0-9_]+)\s*=\s*(-?\d+)\s*,?\s*$", match.group(1), re.MULTILINE)
    }
    if not values:
        raise AssertionError(f"no values parsed from C enum {name}")
    return values


def _camera_functions():
    text = HEADER.read_text()
    return set(
        re.findall(r"^(?:int|void|const char\s*\*)\s*(ost_camera_[a-z0-9_]+)\s*\(", text, re.MULTILINE)
    )


def _resolve(path):
    value = device
    for component in path.split("."):
        value = getattr(value, component)
    return value


def _assert_status_surface(namespace):
    errors = []
    exported = set(namespace.__all__)
    for name, expected in _enum("ost_status").items():
        if not hasattr(namespace, name):
            errors.append(f"missing Python status {name}")
        elif getattr(namespace, name) != expected:
            errors.append(f"{name} is {getattr(namespace, name)}, expected {expected}")
        if name not in exported:
            errors.append(f"{name} is not exported")
    if errors:
        raise AssertionError("\n".join(errors))


def _assert_function_surface(counterparts):
    functions = _camera_functions()
    errors = [f"no Python mapping for {name}" for name in sorted(functions - set(counterparts))]
    errors.extend(f"mapping for undeclared C function {name}" for name in sorted(set(counterparts) - functions))
    for name in sorted(functions & set(counterparts)):
        try:
            _resolve(counterparts[name])
        except AttributeError:
            errors.append(f"missing Python counterpart {counterparts[name]} for {name}")
    if errors:
        raise AssertionError("\n".join(errors))


class PublicSurfaceTest(unittest.TestCase):
    def test_every_c_status_has_an_exported_equal_python_constant(self):
        _assert_status_surface(device)

    def test_every_c_camera_function_has_a_python_counterpart(self):
        _assert_function_surface(CAMERA_COUNTERPARTS)

    def test_every_nonexcluded_c_camera_function_has_ctypes_declarations(self):
        library = types.SimpleNamespace(ost_status_text=mock.Mock())
        for name in _camera_functions() - set(CTYPES_EXCLUSIONS):
            setattr(library, name, mock.Mock())
        device._bind_camera(library)
        for name in sorted(_camera_functions()):
            if name in CTYPES_EXCLUSIONS:
                self.assertTrue(CTYPES_EXCLUSIONS[name])
                continue
            self.assertIsInstance(getattr(library, name).argtypes, list, name)

    def test_operation_and_target_source_values_match_c(self):
        for c_name, python_name in (
            ("ost_camera_operation", "OPERATION_"),
            ("ost_camera_target_source", "TARGET_SOURCE_"),
        ):
            prefix = "OST_CAMERA_OP_" if c_name == "ost_camera_operation" else "OST_CAMERA_TARGET_SOURCE_"
            for name, expected in _enum(c_name).items():
                self.assertEqual(getattr(device, python_name + name.removeprefix(prefix)), expected)

    def test_stream_stats_exposes_every_c_counter_without_changing_its_native_layout(self):
        fields = re.search(r"typedef struct ost_stream_stats\s*\{(.*?)\}", HEADER.read_text(), re.DOTALL)
        self.assertIsNotNone(fields)
        c_names = re.findall(r"^\s*(?:uint32_t|uint64_t|int)\s+([a-z0-9_]+)\s*;", fields.group(1), re.MULTILINE)
        self.assertEqual([name for name, _kind in device._CStreamStats._fields_], c_names)
        self.assertTrue(set(c_names) <= {field.name for field in dataclasses.fields(StreamStats)})
        compatible = StreamStats(1, 2, 3, 4, 5, 6, 7, True)
        self.assertEqual((compatible.max_ready, compatible.max_callback_us), (0, 0))

    def test_drift_check_fails_when_a_function_mapping_is_removed(self):
        broken = dict(CAMERA_COUNTERPARTS)
        del broken["ost_camera_get_target_source"]
        with self.assertRaisesRegex(AssertionError, "no Python mapping for ost_camera_get_target_source"):
            _assert_function_surface(broken)

    def test_drift_check_fails_when_a_status_is_missing(self):
        broken = types.SimpleNamespace(**vars(device))
        del broken.OST_ERR_CLOSED
        with self.assertRaisesRegex(AssertionError, "missing Python status OST_ERR_CLOSED"):
            _assert_status_surface(broken)


class NativeAdaptationTest(unittest.TestCase):
    def setUp(self):
        self.native = NativeCamera.__new__(NativeCamera)
        self.native._handle = ctypes.c_void_p(1)
        self.native._library = mock.Mock()

    def test_target_source_and_last_error_have_native_accessors(self):
        def target_source(_handle, output):
            output._obj.value = device.TARGET_SOURCE_CAMERA_DEFAULT
            return device.OST_SUCCESS

        self.native._library.ost_camera_get_target_source.side_effect = target_source
        self.native._library.ost_camera_last_error.return_value = b"detail"
        self.assertEqual(self.native.target_source, device.TARGET_SOURCE_CAMERA_DEFAULT)
        self.assertEqual(self.native.last_error(), "detail")

    def test_stream_result_preserves_queue_and_callback_counters(self):
        def stream(_handle, _count, _callback, _userdata, output):
            stats = output._obj
            stats.frames_delivered = 4
            stats.max_ready = 3
            stats.short_transfers = 2
            stats.max_callback_us = 17
            stats.stopped = 1
            return device.OST_SUCCESS

        self.native._library.ost_camera_stream.side_effect = stream
        stats = self.native.stream(4, lambda _index, _data: False)
        self.assertEqual(
            (stats.frames_delivered, stats.max_ready, stats.short_transfers, stats.max_callback_us, stats.stopped),
            (4, 3, 2, 17, True),
        )


class OpenOptionsTest(unittest.TestCase):
    def test_options_use_the_explicit_c_open_variant(self):
        for target, expected_set, expected_target in ((-100.0, 1, -100.0), (None, 0, 0.0)):
            with self.subTest(target=target):
                library = mock.Mock()
                library.ost_camera_open_with_options.return_value = device.OST_SUCCESS
                with mock.patch("openst133.device._load_library", return_value=library), mock.patch(
                    "openst133.device._bind_camera", side_effect=lambda value: value
                ):
                    NativeCamera(
                        Device(0x0BD7, 0xA010, 1, 2),
                        transport=object(),
                        options=CameraOpenOptions(target),
                    )
                options = library.ost_camera_open_with_options.call_args.args[2]._obj
                self.assertEqual((options.target_set, options.target_celsius), (expected_set, expected_target))

    def test_real_camera_forwards_options_and_exposes_target_source(self):
        options = CameraOpenOptions(-100.0)
        with mock.patch("openst133.camera.NativeCamera") as native_type:
            native = native_type.return_value
            native.geometry = (1340, 100, True)
            native.supports.return_value = True
            native.target_source = device.TARGET_SOURCE_OPTION
            camera = RealCamera(
                device=Device(0x0BD7, 0xA010, 1, 2),
                transport=object(),
                options=options,
            )
            camera.open()
            self.assertIs(native_type.call_args.kwargs["options"], options)
            self.assertEqual(camera.target_source, device.TARGET_SOURCE_OPTION)
            camera.close()


if __name__ == "__main__":
    unittest.main()
