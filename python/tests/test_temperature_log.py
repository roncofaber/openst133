# Copyright (c) 2026 DATM group, The Molecular Foundry, Lawrence Berkeley National Laboratory
# SPDX-License-Identifier: MIT

from contextlib import redirect_stdout
import csv
import io
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from openst133.temperature_log import log_cooling, main


ROOT = Path(__file__).resolve().parents[2]
MODULE = "openst133.temperature_log"


class Clock:
    def __init__(self):
        self.now = 1000.0

    def monotonic(self):
        return self.now

    def sleep(self, duration):
        self.now += max(0.0, duration)


class Camera:
    def __init__(self, temperatures=(-20.0, -95.5, -95.0, -95.0)):
        self.temperatures = iter(temperatures)
        self.setpoint = None
        self.closed = False

    def open(self):
        pass

    def set_temperature(self, value):
        self.setpoint = value

    def temperature(self):
        return next(self.temperatures)

    def close(self):
        self.closed = True


class TimePlot:
    def __init__(self):
        self.updates = []
        self.closed = False

    def update(self, elapsed, temperature):
        self.updates.append((elapsed, temperature))

    def close(self):
        self.closed = True


class TemperatureLogTests(unittest.TestCase):
    def test_simulated_camera_cooling_curve_and_csv(self):
        from openst133.camera import SimulatedCamera

        clock = Clock()
        output = Path(self.temporary.name) / "cooling.csv"
        camera = SimulatedCamera(cooling_time_constant=600.0, clock=clock.monotonic)
        samples = log_cooling(
            camera, output, interval=60.0, setpoint=-120.0, count=4,
            clock=clock.monotonic, sleep=clock.sleep,
        )
        self.assertEqual(samples, 4)
        with output.open(newline="", encoding="utf-8") as stream:
            rows = list(csv.DictReader(stream))
        temperatures = [float(row["temperature_c"]) for row in rows]
        self.assertEqual(temperatures, sorted(temperatures, reverse=True))
        self.assertLess(temperatures[-1], temperatures[0])
        self.assertEqual([row["lock_state"] for row in rows], ["unlocked"] * 4)
        self.assertEqual([float(row["elapsed_s"]) for row in rows], [0.0, 60.0, 120.0, 180.0])
        self.assertEqual({row["setpoint_c"] for row in rows}, {"-120.000"})

    def test_lock_requires_continuous_tolerance_hold(self):
        camera = Camera()
        clock = Clock()
        output = Path(self.temporary.name) / "lock.csv"
        log_cooling(
            camera, output, interval=1.0, setpoint=-95.0, count=4,
            lock_tolerance=1.0, lock_hold=2.0, clock=clock.monotonic, sleep=clock.sleep,
        )
        with output.open(newline="", encoding="utf-8") as stream:
            rows = list(csv.DictReader(stream))
        self.assertEqual(camera.setpoint, -95.0)
        self.assertEqual([row["lock_state"] for row in rows], ["unlocked", "unlocked", "unlocked", "locked"])
        self.assertTrue(camera.closed)

    def test_readiness_waits_for_extra_settling_after_continuous_lock(self):
        camera = Camera((-95.5, -95.0, -95.0, -96.5, -95.0, -95.0, -95.0, -95.0, -95.0))
        clock = Clock()
        output = Path(self.temporary.name) / "ready.csv"
        stdout = io.StringIO()
        with redirect_stdout(stdout):
            samples = log_cooling(
                camera, output, interval=1.0, setpoint=-95.0,
                lock_tolerance=1.0, lock_hold=2.0,
                wait_ready=True, settle_time=2.0,
                clock=clock.monotonic, sleep=clock.sleep,
            )
        self.assertEqual(samples, 9)
        self.assertTrue(camera.closed)
        with output.open(newline="", encoding="utf-8") as stream:
            rows = list(csv.DictReader(stream))
        self.assertEqual(
            [row["lock_state"] for row in rows],
            ["unlocked", "unlocked", "locked", "unlocked", "unlocked", "unlocked", "locked", "locked", "locked"],
        )
        report = stdout.getvalue()
        self.assertIn("acquisition ready under software criteria", report)
        self.assertIn("target -95.000 C", report)
        self.assertIn("measured -95.000 C", report)
        self.assertIn("tolerance 1.000 C", report)
        self.assertIn("stable 4.000 s", report)
        self.assertIn("extra settling 2.000 s", report)

    def test_readiness_requires_setpoint_and_enough_samples(self):
        output = Path(self.temporary.name) / "missing-target.csv"
        with self.assertRaisesRegex(ValueError, "requires a setpoint"):
            log_cooling(Camera(), output, wait_ready=True)
        self.assertFalse(output.exists())

        camera = Camera((-95.5, -95.5))
        clock = Clock()
        output = Path(self.temporary.name) / "not-ready.csv"
        with redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(RuntimeError, "not reached after 2 samples"):
                log_cooling(
                    camera, output, interval=1.0, setpoint=-95.0, count=2,
                    lock_tolerance=1.0, lock_hold=2.0, wait_ready=True,
                    clock=clock.monotonic, sleep=clock.sleep,
                )
        self.assertTrue(camera.closed)

    def test_no_setpoint_has_unknown_lock_state(self):
        camera = Camera()
        output = Path(self.temporary.name) / "unknown.csv"
        log_cooling(camera, output, count=2)
        with output.open(newline="", encoding="utf-8") as stream:
            rows = list(csv.DictReader(stream))
        self.assertEqual([row["lock_state"] for row in rows], ["unknown", "unknown"])
        self.assertEqual([row["setpoint_c"] for row in rows], ["", ""])
        self.assertIsNone(camera.setpoint)

    def test_interrupt_and_read_failure_close_camera_and_plot(self):
        class Interrupt:
            def __call__(self, _duration):
                raise KeyboardInterrupt

        for camera, sleep, expected in (
            (Camera(), Interrupt(), KeyboardInterrupt),
            (Camera((25.0, float("nan"))), Clock().sleep, RuntimeError),
        ):
            plot = TimePlot()
            output = Path(self.temporary.name) / f"{expected.__name__}.csv"
            with mock.patch("openst133.temperature_log.LivePlot", return_value=plot):
                with self.assertRaises(expected):
                    log_cooling(camera, output, count=2, interval=1.0, plot=True, sleep=sleep)
            self.assertTrue(camera.closed)
            self.assertTrue(plot.closed)
            self.assertEqual(len(output.read_text(encoding="utf-8").splitlines()), 2)

    def test_real_camera_requires_acknowledgment(self):
        self.assertEqual(main(["--output", str(Path(self.temporary.name) / "no.csv"), "--count", "1"]), 2)

    def test_setpoint_range_and_exclusive_output_are_enforced(self):
        output = Path(self.temporary.name) / "existing.csv"
        output.write_text("keep\n", encoding="utf-8")
        camera = Camera()
        with self.assertRaises(FileExistsError):
            log_cooling(camera, output, count=1)
        self.assertEqual(output.read_text(encoding="utf-8"), "keep\n")
        self.assertFalse(camera.closed)
        with self.assertRaises(SystemExit):
            main(["--simulate", "--output", str(output), "--setpoint", "-153"])

    def test_live_plot_renders_measured_series(self):
        try:
            import matplotlib
        except ImportError:
            self.skipTest("matplotlib not installed")
        matplotlib.use("Agg", force=True)
        import matplotlib.pyplot as pyplot
        from openst133.temperature_log import LivePlot

        plot = LivePlot(-120.0, pyplot)
        try:
            for elapsed, temperature in ((0.0, 20.0), (60.0, 6.677), (120.0, -5.378)):
                plot.update(elapsed, temperature)
            self.assertEqual(list(plot.line.get_xdata()), [0.0, 60.0, 120.0])
            self.assertEqual(list(plot.endpoint.get_xdata()), [120.0])
            output = Path(self.temporary.name) / "cooling.png"
            plot.figure.savefig(output)
            self.assertGreater(output.stat().st_size, 1000)
        finally:
            plot.close()

    def setUp(self):        self.temporary = tempfile.TemporaryDirectory(prefix="ost-temperature-log-")

    def tearDown(self):
        self.temporary.cleanup()


class CommandTests(unittest.TestCase):
    def run_command(self, *arguments):
        environment = {
            **os.environ,
            "PYTHONPATH": os.pathsep.join(filter(None, [str(ROOT / "python" / "src"), os.environ.get("PYTHONPATH")])),
            "QT_QPA_PLATFORM": "offscreen",
            "MPLBACKEND": "Agg",
        }
        return subprocess.run(
            [sys.executable, "-m", MODULE, "--simulate", *arguments],
            cwd=ROOT, env=environment, text=True, capture_output=True, check=False, timeout=30,
        )

    def test_simulated_cli_stops_after_count(self):
        with tempfile.TemporaryDirectory(prefix="ost-temperature-cli-") as temporary:
            output = Path(temporary) / "curve.csv"
            result = self.run_command("--output", str(output), "--setpoint", "-100", "--interval", "0.01", "--count", "3")
            self.assertEqual(result.returncode, 0, result.stderr)
            with output.open(newline="", encoding="utf-8") as stream:
                rows = list(csv.DictReader(stream))
            self.assertEqual(len(rows), 3)
            self.assertTrue(all(row["timestamp_utc"].endswith("Z") for row in rows))
            self.assertTrue(all(row["lock_state"] == "unlocked" for row in rows))

    def test_simulated_cli_stops_at_readiness(self):
        with tempfile.TemporaryDirectory(prefix="ost-temperature-ready-") as temporary:
            output = Path(temporary) / "ready.csv"
            result = self.run_command(
                "--output", str(output), "--setpoint", "-100", "--interval", "0.01", "--count", "3",
                "--lock-tolerance", "200", "--lock-hold", "0", "--settle-time", "0", "--wait-ready",
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("acquisition ready under software criteria", result.stdout)
            self.assertIn("target -100.000 C", result.stdout)
            self.assertIn("extra settling 0.000 s", result.stdout)
            with output.open(newline="", encoding="utf-8") as stream:
                rows = list(csv.DictReader(stream))
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0]["lock_state"], "locked")

    def test_sigint_stops_cleanly_and_keeps_csv(self):
        with tempfile.TemporaryDirectory(prefix="ost-temperature-int-") as temporary:
            output = Path(temporary) / "interrupted.csv"
            environment = {
                **os.environ,
                "PYTHONPATH": str(ROOT / "python" / "src"),
                "OPENST133_LOG_LEVEL": "WARNING",
            }
            process = subprocess.Popen(
                [sys.executable, "-m", MODULE, "--simulate", "--output", str(output), "--setpoint", "-100", "--interval", "10"],
                cwd=ROOT, env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
            )
            try:
                first_line = process.stdout.readline()
                self.assertIn(" C  ", first_line)
                process.send_signal(signal.SIGINT)
                stdout, stderr = process.communicate(timeout=10)
            except BaseException:
                process.kill()
                process.wait()
                raise
            self.assertEqual(process.returncode, 130, stderr)
            self.assertIn("stopped by user", stderr)
            with output.open(newline="", encoding="utf-8") as stream:
                rows = list(csv.DictReader(stream))
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0]["lock_state"], "unlocked")


if __name__ == "__main__":
    unittest.main()
