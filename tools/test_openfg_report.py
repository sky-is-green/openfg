#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tests for tools/openfg-report.py."""

import importlib.util
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
spec = importlib.util.spec_from_file_location(
    "openfg_report", Path(__file__).resolve().parent / "openfg-report.py")
report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(report)

LOG = """\
[10:00:00.000] [openfg] display: DP-1 2560x1440 @ 165.08 Hz
[10:00:00.000] [openfg] session: pid 1, 2x ffx, max presented 60 fps, display unknown @ 0.00 Hz
[10:00:00.100] [openfg] backend init: 100.0 ms
[10:00:00.110] [openfg] adaptive: first generated frame (10.0 ms after start, 1 frame)
[10:00:01.000] [openfg] adaptive: start generating (capped mode, 60.0 fps, target 30.0)
[10:00:02.000] [openfg] perf: app 30.0 fps, presented 60.0 fps, generated 50%, gen time 100%, overhead avg 0.80 ms, frame p50 33.33 ms, p95 34.00 ms, generating yes
[10:00:02.000] [openfg] path: source 0.05 ms, generate 0.43 ms, copies 0.00 ms, present 0.30 ms (150 frames)
[10:00:03.000] [openfg] adaptive: stop generating (63.0 fps, target 30.0, dwell 1000 ms)
[10:00:03.000] [openfg] session: 30.0 s, app 30.0 fps, presented 60.0 fps, generated 50%, gen time 100%, overhead avg 0.80 ms, present p50 33.00 / p95 34.00 / p99 35.00 ms, frame p50 33.30 / p95 34.00 ms
"""


class ReportTest(unittest.TestCase):
    def test_parse_and_summary(self):
        parsed = report.parse_text(LOG, "test.log")
        self.assertEqual(len(parsed.sessions), 1)
        self.assertEqual(len(parsed.perf), 1)
        self.assertEqual(len(parsed.paths), 1)
        self.assertEqual(parsed.starts, 1)
        self.assertEqual(parsed.stops, 1)
        self.assertEqual(parsed.display, "DP-1 2560x1440 @ 165.08 Hz")
        row = report.summary(parsed)
        self.assertEqual(row["log"], "test.log")
        self.assertAlmostEqual(row["app_fps"], 30.0)
        self.assertAlmostEqual(row["presented_fps"], 60.0)
        self.assertAlmostEqual(row["generated_pct"], 50.0)
        self.assertAlmostEqual(row["overhead_mean"], 0.80)
        self.assertAlmostEqual(row["frame_p50"], 33.33)
        self.assertTrue(row["present_available"])
        self.assertAlmostEqual(row["present_p50"], 33.0)
        self.assertAlmostEqual(row["source_ms"], 0.05)
        self.assertAlmostEqual(row["generate_ms"], 0.43)

    def test_present_timing_off(self):
        parsed = report.parse_text(
            LOG.replace("present p50 33.00 / p95 34.00 / p99 35.00 ms",
                        "present p50 0.00 / p95 0.00 / p99 0.00 ms"),
            "off.log")
        row = report.summary(parsed)
        self.assertFalse(row["present_available"])

    def test_table_has_full_rows(self):
        parsed = report.parse_text(LOG, "test.log")
        table = report.format_table([report.summary(parsed)])
        self.assertIn("test.log", table)
        self.assertIn("33.0 ms", table)


if __name__ == "__main__":
    unittest.main()
