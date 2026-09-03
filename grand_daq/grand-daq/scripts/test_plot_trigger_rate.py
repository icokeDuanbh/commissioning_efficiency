#!/usr/bin/env python3

import sys
import unittest
from datetime import datetime, timezone
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))

import plot_trigger_rate as ptr  # noqa: E402


class PlotTriggerRateTest(unittest.TestCase):
    def test_build_trigger_rate_points_matches_dashboard_counter_delta(self) -> None:
        samples = [
            (datetime(2026, 4, 30, 4, 0, 0, tzinfo=timezone.utc), 100),
            (datetime(2026, 4, 30, 4, 0, 5, tzinfo=timezone.utc), 130),
            (datetime(2026, 4, 30, 4, 0, 15, tzinfo=timezone.utc), 190),
        ]

        points = ptr.build_trigger_rate_points(samples)

        self.assertEqual(
            points,
            [
                (datetime(2026, 4, 30, 4, 0, 5, tzinfo=timezone.utc), 6.0),
                (datetime(2026, 4, 30, 4, 0, 15, tzinfo=timezone.utc), 6.0),
            ],
        )

    def test_build_trigger_rate_points_clamps_counter_reset_to_zero(self) -> None:
        samples = [
            (datetime(2026, 4, 30, 4, 0, 0, tzinfo=timezone.utc), 100),
            (datetime(2026, 4, 30, 4, 0, 5, tzinfo=timezone.utc), 90),
        ]

        points = ptr.build_trigger_rate_points(samples)

        self.assertEqual(points, [(datetime(2026, 4, 30, 4, 0, 5, tzinfo=timezone.utc), 0.0)])


if __name__ == "__main__":
    unittest.main()
