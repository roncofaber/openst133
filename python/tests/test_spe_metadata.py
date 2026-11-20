# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT

import os
import tempfile
import time
import unittest
from unittest import mock
import xml.etree.ElementTree as ElementTree

from openst133.spe import SpeFile, SpeMetadata, create
from openst133.camera import RealCamera
from openst133.device import NativeCamera, TARGET_SOURCE_OPTION, Readout, StreamStats

SPE_NS = "{http://www.princetoninstruments.com/spe/2009}"
ACQ_NS = "{urn:openst133:acquisition:1}"
PROVENANCE_NS = "{urn:openst133:provenance:1}"
FULL = {
    "accumulations": 3,
    "roi": (100, 20, 200, 40),
    "binning": (2, 4),
    "readout_rate_hz": 100000,
    "controller_gain": 3,
    "detector_id": 98,
    "detector_name": "EEV  100x1340B",
    "chip_width": 1340,
    "chip_height": 100,
    "temperature_celsius": -25.5,
    "setpoint_celsius": -120.0,
    "shutter": "closed",
    "acquired_utc": 1794953518,
}
PROVENANCE = {
    "requested_exposure_seconds": 60.0,
    "requested_roi": (100, 20, 201, 41),
    "requested_binning": (2, 4),
    "target_source": "option",
    "lock_tolerance_celsius": 0.5,
    "lock_hold_seconds": 0.0,
    "lock_settle_seconds": 12.5,
    "stream_frames_delivered": 2,
    "stream_frames_lagged": 0,
    "stream_host_stalls": 0,
    "stream_short_transfers": 1,
    "stream_bytes_received": 2**32 + 1,
    "stream_transfers_completed": 17,
    "stream_partial_bytes_discarded": 512,
    "stream_stopped": False,
    "stream_max_ready": 3,
    "stream_max_callback_us": 1200,
    "calibration_status": "calibrated",
}
def frames(width, height, count=1):
    return [[[0] * width for _ in range(height)] for _ in range(count)]


class TimeZone:
    def __init__(self, zone):
        self.zone = zone

    def __enter__(self):
        self.previous = os.environ.get("TZ")
        os.environ["TZ"] = self.zone
        time.tzset()

    def __exit__(self, *exception):
        if self.previous is None:
            os.environ.pop("TZ", None)
        else:
            os.environ["TZ"] = self.previous
        time.tzset()


class SpeMetadataTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)

    def path(self, name):
        return os.path.join(self.directory.name, name)

    def test_every_field_round_trips(self):
        extended = {**PROVENANCE, "requested_exposure_seconds": 0.375}
        with TimeZone("UTC"):
            create(self.path("full.spe"), frames(100, 10, 2), 0.25, metadata={**FULL, **extended})
        with SpeFile(self.path("full.spe")) as spe:
            metadata = spe.metadata
            footer = spe.xml_footer
        for key, value in {**FULL, **extended}.items():
            self.assertEqual(getattr(metadata, key), value, key)
        self.assertEqual(metadata.exposure_seconds, 0.25)
        self.assertEqual((metadata.date, metadata.time_local, metadata.time_utc), ("17Nov2026", "221158", "221158"))
        self.assertTrue(metadata.software_version.startswith("openst133 "))
        root = ElementTree.fromstring(footer)
        origin = root.find(f"{SPE_NS}DataHistories/{SPE_NS}DataHistory/{SPE_NS}Origin")
        self.assertEqual(origin.get("created"), "2026-11-17T22:11:58Z")
        self.assertEqual(origin.get("software"), "openst133")
        acquisition = origin.find(f"{ACQ_NS}Acquisition")
        self.assertEqual(
            {key: acquisition.get(key) for key in ("exposureSeconds", "frames", "accumulations", "readoutRateHz",
                                                   "controllerGain", "shutter")},
            {"exposureSeconds": "0.25", "frames": "2", "accumulations": "3", "readoutRateHz": "100000",
             "controllerGain": "3", "shutter": "closed"},
        )
        self.assertEqual(acquisition.find(f"{ACQ_NS}Detector").attrib,
                         {"id": "98", "name": "EEV  100x1340B", "width": "1340", "height": "100"})
        self.assertEqual(acquisition.find(f"{ACQ_NS}Region").attrib,
                         {"x": "100", "y": "20", "width": "200", "height": "40", "xBinning": "2", "yBinning": "4"})
        self.assertEqual(acquisition.find(f"{ACQ_NS}Temperature").attrib,
                         {"measuredCelsius": "-25.50", "setpointCelsius": "-120"})
        provenance = origin.find(f"{PROVENANCE_NS}Provenance")
        self.assertEqual(provenance.find(f"{PROVENANCE_NS}Exposure").attrib,
                         {"requestedSeconds": "0.375", "effectiveSeconds": "0.25", "generatedValueRule": "floor"})
        self.assertEqual(provenance.find(f"{PROVENANCE_NS}RequestedRegion").attrib,
                         {"x": "100", "y": "20", "width": "201", "height": "41", "xBinning": "2", "yBinning": "4"})
        self.assertEqual(provenance.find(f"{PROVENANCE_NS}EffectiveRegion").attrib,
                         {"x": "100", "y": "20", "width": "200", "height": "40", "xBinning": "2", "yBinning": "4"})
        self.assertEqual(provenance.find(f"{PROVENANCE_NS}Selection").attrib,
                         {"targetSource": "option"})
        self.assertEqual(provenance.find(f"{PROVENANCE_NS}SoftwareLock").attrib,
                         {"toleranceCelsius": "0.5", "holdSeconds": "0", "settleSeconds": "12.5"})
        self.assertEqual(provenance.find(f"{PROVENANCE_NS}Stream").get("bytesReceived"), str(2**32 + 1))
        self.assertEqual(provenance.find(f"{PROVENANCE_NS}Stream").get("stopped"), "false")
        self.assertEqual(provenance.find(f"{PROVENANCE_NS}Calibration").get("status"), "calibrated")
        self.assertEqual(root.find(f"{SPE_NS}GeneralInformation/{SPE_NS}FileInformation").get("created"),
                         "2026-11-17T22:11:58Z")

    def test_metadata_object_round_trips_and_unknown_fields_stay_none(self):
        create(self.path("minimal.spe"), frames(3, 2), 1.0)
        with SpeFile(self.path("minimal.spe")) as spe:
            minimal = spe.metadata
        self.assertIsNone(minimal.roi)
        self.assertIsNone(minimal.readout_rate_hz)
        self.assertIsNone(minimal.temperature_celsius)
        self.assertIsNone(minimal.setpoint_celsius)
        self.assertIsNone(minimal.shutter)
        self.assertAlmostEqual(minimal.acquired_utc, time.time(), delta=60)
        source = SpeMetadata(exposure_seconds=0.5, **FULL, **PROVENANCE)
        create(self.path("copy.spe"), frames(100, 10), source.exposure_seconds, metadata=source)
        with SpeFile(self.path("copy.spe")) as spe:
            copied = spe.metadata
        for key in {**FULL, **PROVENANCE}:
            self.assertEqual(getattr(copied, key), getattr(source, key), key)
        self.assertNotIn("date", SpeMetadata(exposure_seconds=1.0).as_dict())
        self.assertEqual(copied.as_dict()["roi"], [100, 20, 200, 40])

    def test_unknown_provenance_stays_unknown_and_calibration_defaults(self):
        create(self.path("unknown.spe"), frames(3, 2), 1.0)
        with SpeFile(self.path("unknown.spe")) as spe:
            metadata = spe.metadata
            root = ElementTree.fromstring(spe.xml_footer)
        for name in PROVENANCE:
            if name != "calibration_status":
                self.assertIsNone(getattr(metadata, name), name)
        self.assertEqual(metadata.calibration_status, "uncalibrated")
        provenance = root.find(f".//{PROVENANCE_NS}Provenance")
        self.assertIsNotNone(provenance)
        unknown = {name for name in PROVENANCE if name.startswith("stream_") and name != "stream_stopped"}
        self.assertEqual(provenance.find(f"{PROVENANCE_NS}Exposure").get("requestedSeconds"), "unknown")
        self.assertEqual(provenance.find(f"{PROVENANCE_NS}Exposure").get("generatedValueRule"), "unknown")
        for name in unknown:
            camel_name = name.removeprefix("stream_").split("_")
            attribute = camel_name[0] + "".join(part.capitalize() for part in camel_name[1:])
            self.assertEqual(provenance.find(f"{PROVENANCE_NS}Stream").get(attribute), "unknown", name)
        for tag in ("RequestedRegion", "EffectiveRegion"):
            self.assertEqual(set(provenance.find(f"{PROVENANCE_NS}{tag}").attrib.values()), {"unknown"})
        self.assertEqual(provenance.find(f"{PROVENANCE_NS}Selection").attrib,
                         {"targetSource": "unknown"})
        self.assertEqual(set(provenance.find(f"{PROVENANCE_NS}SoftwareLock").attrib.values()), {"unknown"})
        self.assertEqual(provenance.find(f"{PROVENANCE_NS}Stream").get("stopped"), "unknown")
        self.assertEqual(provenance.find(f"{PROVENANCE_NS}Calibration").get("status"), "uncalibrated")

    def test_long_generated_exposure_accumulations_and_100khz(self):
        metadata = {**FULL, **PROVENANCE}
        create(self.path("long.spe"), frames(100, 10), 59.904, metadata=metadata)
        with SpeFile(self.path("long.spe")) as spe:
            saved = spe.metadata
            footer = spe.xml_footer
        self.assertEqual(saved.requested_exposure_seconds, 60.0)
        self.assertEqual(saved.exposure_seconds, 59.904)
        self.assertEqual(saved.accumulations, 3)
        self.assertEqual(saved.readout_rate_hz, 100000)
        self.assertEqual(saved.roi, (100, 20, 200, 40))
        self.assertEqual(saved.requested_roi, (100, 20, 201, 41))
        root = ElementTree.fromstring(footer)
        exposure = root.find(f".//{PROVENANCE_NS}Provenance/{PROVENANCE_NS}Exposure")
        self.assertEqual(exposure.get("requestedSeconds"), "60")
        self.assertEqual(float(exposure.get("effectiveSeconds")), 59.904)
        self.assertEqual(exposure.get("generatedValueRule"), "floor")

    def test_real_camera_reports_requested_effective_and_stream_fields_without_hardware(self):
        class Native:
            target_source = TARGET_SOURCE_OPTION

            def __init__(self):
                self.readout = Readout(0, 0, 1340, 100, 1, 1)

            def get_readout(self):
                return self.readout

            def set_readout(self, readout):
                self.readout = readout

            def set_exposure_ms(self, milliseconds):
                self.exposure_ms = milliseconds

            def get_effective_exposure_ms(self):
                return 59904

            def set_readout_rate(self, rate_hz):
                self.rate_hz = rate_hz

        camera = RealCamera()
        camera._native = Native()
        camera._exposure_ms = 60000
        camera._requested_exposure = 60.0
        camera._requested_readout = Readout(10, 20, 40, 40, 2, 4)
        camera._readout_rate = 100000
        camera.stream_stats = StreamStats(
            frames_delivered=1, frames_lagged=0, host_stalls=0, short_transfers=0, bytes_received=268000,
            transfers_completed=17, partial_bytes_discarded=0, stopped=True, max_ready=1, max_callback_us=10,
        )
        with mock.patch.object(NativeCamera, "get_readout", lambda native: native.get_readout()):
            metadata = camera.spe_metadata()
        self.assertEqual(metadata["requested_exposure_seconds"], 60.0)
        self.assertEqual(metadata["requested_roi"], (10, 20, 40, 40))
        self.assertEqual(metadata["requested_binning"], (2, 4))
        self.assertEqual(metadata["roi"], (0, 0, 1340, 100))
        self.assertNotIn("policy", metadata)
        self.assertEqual(metadata["target_source"], "option")
        for name, value in camera.stream_stats.__dict__.items():
            self.assertEqual(metadata[f"stream_{name}"], value)

    def test_requested_exposure_is_recorded_before_rounding(self):
        camera = RealCamera()
        camera._native = object()
        with mock.patch.object(NativeCamera, "set_exposure_ms") as programmed:
            camera.set_exposure(0.3333)
        programmed.assert_called_once_with(camera._native, 334)
        self.assertEqual(camera._requested_exposure, 0.3333)

    def test_accumulated_int32_spe22_retains_legacy_metadata_only(self):
        create(self.path("accumulated.spe"), frames(100, 10), 59.904, data_type="int32",
               metadata={**FULL, **PROVENANCE})
        with SpeFile(self.path("accumulated.spe")) as spe:
            saved = spe.metadata
            self.assertIsNone(spe.xml_footer)
        self.assertEqual(saved.accumulations, 3)
        self.assertEqual(saved.readout_rate_hz, 100000)
        self.assertAlmostEqual(saved.exposure_seconds, 59.904, places=3)
        self.assertEqual(saved.roi, FULL["roi"])
        self.assertIsNone(saved.requested_exposure_seconds)
        self.assertIsNone(saved.stream_frames_delivered)

    def test_invalid_metadata_is_refused(self):
        cases = [
            ({"colour": "red"}, ValueError),
            ({"binning": (1, 1)}, ValueError),
            ({"shutter": "open"}, ValueError),
            ({"roi": (0, 0, 50, 10)}, OSError),
            ({"readout_rate_hz": -1}, ValueError),
            ({"detector_name": "x" * 32}, ValueError),
            ({"temperature_celsius": float("nan")}, OSError),
            ({"requested_roi": FULL["roi"]}, ValueError),
            ({"stream_frames_delivered": 0}, ValueError),
        ]
        for index, (metadata, error) in enumerate(cases):
            with self.subTest(metadata=metadata):
                with self.assertRaises(error):
                    create(self.path(f"bad-{index}.spe"), frames(100, 10), 0.1, metadata=metadata)
                self.assertFalse(os.path.exists(self.path(f"bad-{index}.spe")))


if __name__ == "__main__":
    unittest.main()
