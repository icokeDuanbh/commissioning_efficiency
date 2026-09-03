#!/usr/bin/env python3

import json
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))

import analyze_health_loss as ahl  # noqa: E402


class AnalyzeHealthLossTest(unittest.TestCase):
    def test_counter_delta_across_files_and_recent_event_dedup(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            first = Path(tmpdir) / "health_a.jsonl"
            second = Path(tmpdir) / "health_b.jsonl"

            recent_event = {
                "observed_unix_ms": 1_772_000_010_000,
                "tag": 42,
                "event_ts_ns": 123456789,
                "expected": 4,
                "attached": 2,
                "missing_count": 2,
                "missing_du": [101, 102],
            }

            first.write_text(
                "\n".join(
                    [
                        json.dumps(
                            {
                                "ts": "2026-04-28T00:00:00Z",
                                "health": {
                                    "builder": {
                                        "expected_fragments": 10,
                                        "missing_on_timeout": 1,
                                        "timeout_events": 1,
                                        "missing_on_timeout_per_du": {"101": 1},
                                    }
                                },
                            }
                        ),
                        json.dumps(
                            {
                                "ts": "2026-04-28T00:04:00Z",
                                "health": {
                                    "builder": {
                                        "expected_fragments": 20,
                                        "missing_on_timeout": 3,
                                        "timeout_events": 2,
                                        "missing_on_timeout_per_du": {"101": 2, "102": 1},
                                    },
                                    "loss_stats": {"recent_events": [recent_event]},
                                },
                            }
                        ),
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            second.write_text(
                json.dumps(
                    {
                        "ts": "2026-04-28T00:06:00Z",
                        "health": {
                            "builder": {
                                "expected_fragments": 28,
                                "missing_on_timeout": 4,
                                "timeout_events": 3,
                                "missing_on_timeout_per_du": {"101": 2, "102": 2},
                            },
                            "loss_stats": {"recent_events": [recent_event]},
                        },
                    }
                )
                + "\n",
                encoding="utf-8",
            )

            result = ahl.analyze_files([first, second], bucket_sec=300, recent_limit=20)

        self.assertEqual(result["source"], "counter_delta")
        self.assertEqual(result["totals"]["expected_fragments"], 18)
        self.assertEqual(result["totals"]["missing_fragments"], 3)
        self.assertEqual(result["totals"]["timeout_events"], 2)
        self.assertEqual(len(result["buckets"]), 2)
        self.assertEqual(result["buckets"][0]["missing_fragments"], 2)
        self.assertEqual(result["buckets"][1]["missing_fragments"], 1)
        self.assertEqual(result["recent_events"], [recent_event])

    def test_mixed_loss_stats_keeps_counter_only_buckets(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            first = Path(tmpdir) / "health_20260428_000000.jsonl"
            second = Path(tmpdir) / "health_20260428_000000_20260428.jsonl"

            first.write_text(
                "\n".join(
                    [
                        json.dumps(
                            {
                                "ts": "2026-04-28T00:00:00Z",
                                "health": {"builder": {"expected_fragments": 10, "missing_on_timeout": 0}},
                            }
                        ),
                        json.dumps(
                            {
                                "ts": "2026-04-28T00:04:00Z",
                                "health": {"builder": {"expected_fragments": 20, "missing_on_timeout": 2}},
                            }
                        ),
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            second.write_text(
                json.dumps(
                    {
                        "ts": "2026-04-28T00:06:00Z",
                        "health": {
                            "builder": {"expected_fragments": 30, "missing_on_timeout": 5},
                            "loss_stats": {
                                "bucket_sec": 300,
                                "buckets": [
                                    {
                                        "start_unix_ms": 1777334700000,
                                        "expected_fragments": 8,
                                        "missing_fragments": 3,
                                        "timeout_events": 1,
                                        "loss_pct": 0.375,
                                        "top_missing_du": [],
                                    }
                                ],
                            },
                        },
                    }
                )
                + "\n",
                encoding="utf-8",
            )

            result = ahl.analyze_files([first, second], bucket_sec=300, recent_limit=20)

        self.assertEqual(result["source"], "mixed")
        self.assertEqual([b["missing_fragments"] for b in result["buckets"]], [2, 3])
        self.assertEqual(result["totals"]["missing_fragments"], 5)

    def test_console_log_timeout_fallback_when_health_jsonl_has_parse_errors(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            health = Path(tmpdir) / "health_20260428_091336.jsonl"
            log = Path(tmpdir) / "csdaq_20260428_091336.log"

            health.write_text(
                "\n".join(
                    [
                        json.dumps(
                            {
                                "ts": "2026-04-29T08:06:53Z",
                                "health": {
                                    "builder": {
                                        "expected_fragments": 100,
                                        "missing_on_timeout": 0,
                                        "timeout_events": 0,
                                    },
                                    "loss_stats": {"buckets": []},
                                },
                            }
                        ),
                        json.dumps({"ts": "2026-04-29T08:06:58Z", "raw": "health_parse_error"}),
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            log.write_text(
                "\n".join(
                    [
                        "[2026/04/29 16:06:58.136] [WARN] Builder diag type=Timeout "
                        "tag=848728 du_id=0 trigger_number=0 fragment_trigger_time=0 "
                        "event_trigger_timestamp_ns=8390296971816438",
                        "[2026/04/29 16:16:47.912] [WARN] Builder diag type=Timeout "
                        "tag=961470 du_id=0 trigger_number=0 fragment_trigger_time=0 "
                        "event_trigger_timestamp_ns=8390886915034485",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )

            result = ahl.analyze_files([health], bucket_sec=300, recent_limit=20, log_paths=[log])

        self.assertEqual(result["health_parse_errors"], 1)
        self.assertEqual(result["source"], "log_timeout")
        self.assertEqual(result["totals"]["missing_fragments"], 2)
        self.assertEqual(result["totals"]["timeout_events"], 2)
        self.assertEqual([event["tag"] for event in result["recent_events"]], [848728, 961470])

    def test_live_health_snapshot_replaces_sparse_log_events(self) -> None:
        analyzer = ahl.LossAnalyzer(bucket_sec=300, recent_limit=20)
        sparse_event = {
            "observed_unix_ms": 1_777_460_658_995,
            "tag": 3607390,
            "event_ts_ns": 8400937923744989,
            "expected": 0,
            "attached": 0,
            "missing_count": 1,
            "missing_du": [],
            "source": "csdaq_log_timeout",
        }
        analyzer._add_recent_event(sparse_event)
        analyzer._add_recent_event(
            {
                "observed_unix_ms": 1_777_460_659_000,
                "tag": 3607391,
                "event_ts_ns": 8400937923744990,
                "expected": 0,
                "attached": 0,
                "missing_count": 1,
                "missing_du": [],
                "source": "csdaq_log_timeout",
            }
        )

        analyzer.apply_live_health_snapshot(
            {
                "loss_stats": {
                    "bucket_sec": 300,
                    "totals": {
                        "expected_fragments": 1000,
                        "missing_fragments": 8,
                        "timeout_events": 7,
                        "loss_pct": 0.008,
                    },
                    "buckets": [
                        {
                            "start_unix_ms": 1_777_460_400_000,
                            "expected_fragments": 1000,
                            "missing_fragments": 8,
                            "timeout_events": 7,
                            "loss_pct": 0.008,
                            "top_missing_du": [],
                        }
                    ],
                    "recent_events": [
                        {
                            "observed_unix_ms": 1_777_460_658_995,
                            "tag": 3607390,
                            "event_ts_ns": 8400937923744989,
                            "expected": 9,
                            "attached": 8,
                            "missing_count": 1,
                            "missing_du": [1059],
                        }
                    ],
                }
            }
        )

        result = analyzer.result()

        self.assertEqual(result["source"], "live_health")
        self.assertEqual(result["totals"]["missing_fragments"], 8)
        self.assertEqual(len(result["recent_events"]), 1)
        self.assertEqual(result["recent_events"][0]["tag"], 3607390)
        self.assertEqual(result["recent_events"][0]["missing_du"], [1059])
        self.assertEqual(result["recent_events"][0]["attached"], 8)
        self.assertEqual(result["recent_events"][0]["expected"], 9)

    def test_loss_focus_limits_zoom_to_nonzero_buckets(self) -> None:
        buckets = [
            {"start_unix_ms": 100_000, "missing_fragments": 0},
            {"start_unix_ms": 400_000, "missing_fragments": 3},
            {"start_unix_ms": 700_000, "missing_fragments": 2},
            {"start_unix_ms": 1_600_000, "missing_fragments": 0},
        ]

        self.assertEqual(ahl.loss_focus_limits(buckets, 300), (100_000, 1_000_000))


if __name__ == "__main__":
    unittest.main()
