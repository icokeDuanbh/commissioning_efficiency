#!/usr/bin/env python3

from __future__ import annotations

import argparse
import gzip
import json
import re
import sys
import urllib.error
import urllib.request
from collections import OrderedDict
from datetime import datetime
from glob import glob
from pathlib import Path
from typing import Any, Iterable


_STAMP_RE = re.compile(r"^health_(\d{8}_\d{6})(?:_(\d{8}))?\.jsonl(?:\.gz)?$")
_TIMEOUT_LOG_RE = re.compile(
    r"^\[(?P<ts>\d{4}/\d{2}/\d{2} \d{2}:\d{2}:\d{2})(?:\.(?P<ms>\d+))?\].*"
    r"Builder diag type=Timeout tag=(?P<tag>\d+).*?"
    r"event_trigger_timestamp_ns=(?P<event_ts_ns>\d+)"
)


def _to_int(value: Any, default: int = 0) -> int:
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def _to_float(value: Any, default: float = 0.0) -> float:
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def _parse_ts_ms(ts: Any) -> int:
    if not isinstance(ts, str) or not ts:
        return 0
    try:
        return int(datetime.fromisoformat(ts.replace("Z", "+00:00")).timestamp() * 1000)
    except ValueError:
        return 0


def _format_local_ms(ts_ms: int) -> str:
    if ts_ms <= 0:
        return "-"
    return datetime.fromtimestamp(ts_ms / 1000).strftime("%Y/%m/%d %H:%M:%S")


def _parse_log_ts_ms(ts: str, ms_text: str | None) -> int:
    try:
        base = datetime.strptime(ts, "%Y/%m/%d %H:%M:%S")
    except ValueError:
        return 0
    fraction = int((ms_text or "0")[:3].ljust(3, "0"))
    return int(base.timestamp() * 1000) + fraction


def _open_text(path: Path):
    if path.name.endswith(".gz"):
        return gzip.open(path, "rt", encoding="utf-8", errors="replace")
    return path.open("r", encoding="utf-8", errors="replace")


def _file_sort_key(path: Path) -> tuple[str, str, str]:
    match = _STAMP_RE.match(path.name)
    if not match:
        return ("", "", path.name)
    run_id = match.group(1)
    day = match.group(2) or run_id[:8]
    return (run_id, day, path.name)


def _run_id(path: Path) -> str | None:
    match = _STAMP_RE.match(path.name)
    return match.group(1) if match else None


def _builder_counter(health: dict[str, Any], name: str) -> int:
    builder = health.get("builder")
    if not isinstance(builder, dict):
        return 0
    return _to_int(builder.get(name), 0)


def _builder_per_du_missing(health: dict[str, Any]) -> dict[int, int]:
    builder = health.get("builder")
    raw = builder.get("missing_on_timeout_per_du") if isinstance(builder, dict) else None
    if not isinstance(raw, dict):
        return {}

    out: dict[int, int] = {}
    for du_id, count in raw.items():
        try:
            out[int(du_id)] = _to_int(count, 0)
        except (TypeError, ValueError):
            continue
    return out


def _normalize_top_missing_du(raw: Any) -> list[dict[str, Any]]:
    if not isinstance(raw, list):
        return []
    out: list[dict[str, Any]] = []
    for item in raw:
        if not isinstance(item, dict):
            continue
        out.append(
            {
                "du_id": _to_int(item.get("du_id")),
                "missing": _to_int(item.get("missing")),
                "expected": None if item.get("expected") is None else _to_int(item.get("expected")),
                "loss_pct": None if item.get("loss_pct") is None else _to_float(item.get("loss_pct")),
            }
        )
    return out


def _normalize_bucket(bucket: dict[str, Any]) -> dict[str, Any] | None:
    start_ms = _to_int(bucket.get("start_unix_ms"), 0)
    if start_ms <= 0:
        return None
    expected = _to_int(bucket.get("expected_fragments"), 0)
    missing = _to_int(bucket.get("missing_fragments"), 0)
    loss_pct = bucket.get("loss_pct")
    return {
        "start_unix_ms": start_ms,
        "expected_fragments": expected,
        "missing_fragments": missing,
        "timeout_events": _to_int(bucket.get("timeout_events"), 0),
        "loss_pct": missing / expected if loss_pct is None and expected > 0 else _to_float(loss_pct, 0.0),
        "top_missing_du": _normalize_top_missing_du(bucket.get("top_missing_du")),
    }


def _normalize_recent_event(event: dict[str, Any]) -> dict[str, Any] | None:
    missing_du_raw = event.get("missing_du")
    if not isinstance(missing_du_raw, list):
        missing_du_raw = []

    missing_du: list[int] = []
    for du_id in missing_du_raw:
        try:
            missing_du.append(int(du_id))
        except (TypeError, ValueError):
            continue

    observed_ms = _to_int(event.get("observed_unix_ms"), 0)
    if observed_ms <= 0:
        return None

    normalized = {
        "observed_unix_ms": observed_ms,
        "tag": _to_int(event.get("tag"), 0),
        "event_ts_ns": _to_int(event.get("event_ts_ns"), 0),
        "expected": _to_int(event.get("expected"), 0),
        "attached": _to_int(event.get("attached"), 0),
        "missing_count": _to_int(event.get("missing_count"), len(missing_du)),
        "missing_du": missing_du,
    }
    if event.get("source"):
        normalized["source"] = event.get("source")
    return normalized


def _recent_key(event: dict[str, Any]) -> tuple[Any, ...]:
    return (
        event.get("tag"),
        event.get("event_ts_ns"),
    )


def _recent_detail_score(event: dict[str, Any]) -> int:
    return (
        (10 if event.get("missing_du") else 0)
        + (5 if _to_int(event.get("expected")) > 0 else 0)
        + (5 if _to_int(event.get("attached")) > 0 else 0)
    )


def _normalize_loss_totals(raw: Any) -> dict[str, Any] | None:
    if not isinstance(raw, dict):
        return None
    expected = _to_int(raw.get("expected_fragments"), 0)
    missing = _to_int(raw.get("missing_fragments"), 0)
    loss_pct = raw.get("loss_pct")
    return {
        "expected_fragments": expected,
        "missing_fragments": missing,
        "timeout_events": _to_int(raw.get("timeout_events"), 0),
        "loss_pct": missing / expected if loss_pct is None and expected > 0 else _to_float(loss_pct, 0.0),
    }


def loss_focus_limits(buckets: list[dict[str, Any]], bucket_sec: int) -> tuple[int, int] | None:
    nonzero = [
        _to_int(bucket.get("start_unix_ms"))
        for bucket in buckets
        if _to_int(bucket.get("missing_fragments")) > 0
    ]
    if not nonzero:
        return None
    bucket_ms = max(1, bucket_sec) * 1000
    return max(0, min(nonzero) - bucket_ms), max(nonzero) + bucket_ms


class LossAnalyzer:
    def __init__(self, bucket_sec: int = 300, recent_limit: int = 200) -> None:
        self.bucket_sec = max(1, bucket_sec)
        self.bucket_ms = self.bucket_sec * 1000
        self.recent_limit = recent_limit
        self.total_bytes = 0
        self.total_lines = 0
        self.parse_errors = 0
        self.health_parse_errors = 0
        self.jsonl_files: list[str] = []
        self.log_files: list[str] = []
        self.log_lines = 0

        self._current_run_id: str | None = None
        self._prev_health: dict[str, Any] | None = None
        self._loss_buckets: dict[int, dict[str, Any]] = {}
        self._counter_buckets: dict[int, dict[str, Any]] = {}
        self._log_buckets: dict[int, dict[str, Any]] = {}
        self._live_buckets: dict[int, dict[str, Any]] = {}
        self._live_totals: dict[str, Any] | None = None
        self._live_recent_keys: set[tuple[Any, ...]] | None = None
        self._recent_events: OrderedDict[tuple[Any, ...], dict[str, Any]] = OrderedDict()

    def reset_counter_baseline(self) -> None:
        self._prev_health = None

    def process_file(self, path: Path) -> None:
        run_id = _run_id(path)
        if run_id and self._current_run_id and run_id != self._current_run_id:
            self.reset_counter_baseline()
        if run_id:
            self._current_run_id = run_id

        self.jsonl_files.append(str(path))
        try:
            self.total_bytes += path.stat().st_size
        except OSError:
            pass

        with _open_text(path) as stream:
            for line in stream:
                line = line.strip()
                if not line:
                    continue
                self.total_lines += 1
                try:
                    record = json.loads(line)
                except json.JSONDecodeError:
                    self.parse_errors += 1
                    continue
                if record.get("raw") == "health_parse_error":
                    self.health_parse_errors += 1
                    continue
                ts_ms = _parse_ts_ms(record.get("ts"))
                health = record.get("health")
                if isinstance(health, dict):
                    self.process_record(ts_ms, health)

    def process_log_file(self, path: Path) -> None:
        self.log_files.append(str(path))
        with path.open("r", encoding="utf-8", errors="replace") as stream:
            for line in stream:
                self.log_lines += 1
                match = _TIMEOUT_LOG_RE.search(line)
                if not match:
                    continue
                observed_ms = _parse_log_ts_ms(match.group("ts"), match.group("ms"))
                if observed_ms <= 0:
                    continue
                event = {
                    "observed_unix_ms": observed_ms,
                    "tag": _to_int(match.group("tag")),
                    "event_ts_ns": _to_int(match.group("event_ts_ns")),
                    "expected": 0,
                    "attached": 0,
                    "missing_count": 1,
                    "missing_du": [],
                    "source": "csdaq_log_timeout",
                }
                self._add_recent_event(event)

                start_ms = (observed_ms // self.bucket_ms) * self.bucket_ms
                bucket = self._log_buckets.setdefault(
                    start_ms,
                    {
                        "start_unix_ms": start_ms,
                        "expected_fragments": 0,
                        "missing_fragments": 0,
                        "timeout_events": 0,
                        "loss_pct": 0.0,
                        "top_missing_du": [],
                        "source": "csdaq_log_timeout",
                    },
                )
                bucket["missing_fragments"] += 1
                bucket["timeout_events"] += 1

    def process_record(self, ts_ms: int, health: dict[str, Any]) -> None:
        self._collect_loss_stats(health)
        self._collect_counter_delta(ts_ms, health)
        self._prev_health = health

    def _add_recent_event(self, event: dict[str, Any]) -> None:
        key = _recent_key(event)
        if key in self._recent_events:
            existing = self._recent_events[key]
            if _recent_detail_score(event) >= _recent_detail_score(existing):
                self._recent_events[key] = event
            self._recent_events.move_to_end(key)
        else:
            self._recent_events[key] = event
        if self.recent_limit > 0:
            while len(self._recent_events) > self.recent_limit:
                self._recent_events.popitem(last=False)

    def apply_live_health_snapshot(self, health: dict[str, Any]) -> None:
        loss = health.get("loss_stats")
        if not isinstance(loss, dict):
            return

        totals = _normalize_loss_totals(loss.get("totals"))
        if totals is not None:
            self._live_totals = totals

        buckets = loss.get("buckets")
        if isinstance(buckets, list):
            for raw_bucket in buckets:
                if not isinstance(raw_bucket, dict):
                    continue
                bucket = _normalize_bucket(raw_bucket)
                if bucket is not None:
                    bucket["source"] = "live_health"
                    self._live_buckets[bucket["start_unix_ms"]] = bucket

        recent = loss.get("recent_events")
        if isinstance(recent, list):
            self._live_recent_keys = set()
            for raw_event in recent:
                if not isinstance(raw_event, dict):
                    continue
                event = dict(raw_event)
                event["source"] = "live_health"
                normalized = _normalize_recent_event(event)
                if normalized is not None:
                    self._add_recent_event(normalized)
                    self._live_recent_keys.add(_recent_key(normalized))

    def _collect_loss_stats(self, health: dict[str, Any]) -> None:
        loss = health.get("loss_stats")
        if not isinstance(loss, dict):
            return

        buckets = loss.get("buckets")
        if isinstance(buckets, list):
            for raw_bucket in buckets:
                if not isinstance(raw_bucket, dict):
                    continue
                bucket = _normalize_bucket(raw_bucket)
                if bucket is not None:
                    self._loss_buckets[bucket["start_unix_ms"]] = bucket

        recent = loss.get("recent_events")
        if isinstance(recent, list):
            for raw_event in recent:
                if not isinstance(raw_event, dict):
                    continue
                event = _normalize_recent_event(raw_event)
                if event is None:
                    continue
                self._add_recent_event(event)

    def _collect_counter_delta(self, ts_ms: int, health: dict[str, Any]) -> None:
        if self._prev_health is None:
            return

        start_ms = (ts_ms // self.bucket_ms) * self.bucket_ms if ts_ms > 0 else 0
        bucket = self._counter_buckets.setdefault(
            start_ms,
            {
                "start_unix_ms": start_ms,
                "expected_fragments": 0,
                "missing_fragments": 0,
                "timeout_events": 0,
                "_per_du_missing": {},
            },
        )

        expected_delta = max(
            0,
            _builder_counter(health, "expected_fragments")
            - _builder_counter(self._prev_health, "expected_fragments"),
        )
        missing_delta = max(
            0,
            _builder_counter(health, "missing_on_timeout")
            - _builder_counter(self._prev_health, "missing_on_timeout"),
        )
        timeout_delta = max(
            0,
            _builder_counter(health, "timeout_events")
            - _builder_counter(self._prev_health, "timeout_events"),
        )

        bucket["expected_fragments"] += expected_delta
        bucket["missing_fragments"] += missing_delta
        bucket["timeout_events"] += timeout_delta

        prev_du = _builder_per_du_missing(self._prev_health)
        cur_du = _builder_per_du_missing(health)
        per_du_missing = bucket["_per_du_missing"]
        for du_id, count in cur_du.items():
            delta = max(0, count - prev_du.get(du_id, 0))
            if delta:
                per_du_missing[du_id] = per_du_missing.get(du_id, 0) + delta

    def result(self) -> dict[str, Any]:
        counter_buckets = self._counter_bucket_result()
        if self._loss_buckets:
            merged_by_start = dict(self._loss_buckets)
            added_counter_bucket = False
            for bucket in counter_buckets:
                start_ms = _to_int(bucket.get("start_unix_ms"), 0)
                if start_ms not in merged_by_start:
                    merged_by_start[start_ms] = bucket
                    added_counter_bucket = True
            buckets = [merged_by_start[start_ms] for start_ms in sorted(merged_by_start)]
            source = "mixed" if added_counter_bucket else "health_loss_stats"
        else:
            buckets = counter_buckets
            source = "counter_delta"

        log_buckets = [self._log_buckets[start_ms] for start_ms in sorted(self._log_buckets)]
        if log_buckets and self.health_parse_errors:
            buckets, added_log_bucket = self._merge_log_fallback_buckets(buckets, log_buckets)
            if added_log_bucket:
                source = "log_timeout" if source == "counter_delta" and not counter_buckets else "mixed_log_fallback"

        if self._live_buckets:
            buckets = [self._live_buckets[start_ms] for start_ms in sorted(self._live_buckets)]
            source = "live_health"

        if self._live_totals is not None:
            totals = self._live_totals
        else:
            totals = {
                "expected_fragments": sum(_to_int(bucket.get("expected_fragments")) for bucket in buckets),
                "missing_fragments": sum(_to_int(bucket.get("missing_fragments")) for bucket in buckets),
                "timeout_events": sum(_to_int(bucket.get("timeout_events")) for bucket in buckets),
                "loss_pct": 0.0,
            }
            if totals["expected_fragments"] > 0:
                totals["loss_pct"] = totals["missing_fragments"] / totals["expected_fragments"]

        return {
            "source": source,
            "bucket_sec": self.bucket_sec,
            "total_bytes": self.total_bytes,
            "total_lines": self.total_lines,
            "parse_errors": self.parse_errors,
            "health_parse_errors": self.health_parse_errors,
            "jsonl_files": self.jsonl_files,
            "log_files": self.log_files,
            "log_lines": self.log_lines,
            "totals": totals,
            "buckets": buckets,
            "recent_events": sorted(
                self._recent_events_for_output(),
                key=lambda e: (_to_int(e.get("observed_unix_ms")), _to_int(e.get("tag"))),
            ),
        }

    def _recent_events_for_output(self) -> list[dict[str, Any]]:
        if self._live_recent_keys is None:
            return list(self._recent_events.values())
        return [
            event
            for key, event in self._recent_events.items()
            if key in self._live_recent_keys
        ]

    def _merge_log_fallback_buckets(
        self,
        buckets: list[dict[str, Any]],
        log_buckets: list[dict[str, Any]],
    ) -> tuple[list[dict[str, Any]], bool]:
        by_start = {_to_int(bucket.get("start_unix_ms")): dict(bucket) for bucket in buckets}
        added = False
        for log_bucket in log_buckets:
            start_ms = _to_int(log_bucket.get("start_unix_ms"))
            existing = by_start.get(start_ms)
            if existing is None:
                by_start[start_ms] = dict(log_bucket)
                added = True
                continue
            if _to_int(existing.get("missing_fragments")) > 0:
                continue
            existing["missing_fragments"] = _to_int(log_bucket.get("missing_fragments"))
            existing["timeout_events"] = max(
                _to_int(existing.get("timeout_events")),
                _to_int(log_bucket.get("timeout_events")),
            )
            expected = _to_int(existing.get("expected_fragments"))
            existing["loss_pct"] = existing["missing_fragments"] / expected if expected > 0 else 0.0
            existing["source"] = "csdaq_log_timeout"
            added = True
        return [by_start[start_ms] for start_ms in sorted(by_start)], added

    def _counter_bucket_result(self) -> list[dict[str, Any]]:
        out: list[dict[str, Any]] = []
        for start_ms in sorted(self._counter_buckets):
            bucket = self._counter_buckets[start_ms]
            per_du_missing = bucket.get("_per_du_missing")
            if not isinstance(per_du_missing, dict):
                per_du_missing = {}
            expected = _to_int(bucket.get("expected_fragments"), 0)
            missing = _to_int(bucket.get("missing_fragments"), 0)
            ranked_du = sorted(per_du_missing.items(), key=lambda item: (-item[1], item[0]))[:10]
            out.append(
                {
                    "start_unix_ms": start_ms,
                    "expected_fragments": expected,
                    "missing_fragments": missing,
                    "timeout_events": _to_int(bucket.get("timeout_events"), 0),
                    "loss_pct": missing / expected if expected > 0 else 0.0,
                    "top_missing_du": [
                        {
                            "du_id": du_id,
                            "missing": count,
                            "expected": None,
                            "loss_pct": None,
                        }
                        for du_id, count in ranked_du
                    ],
                }
            )
        return out


def analyze_files(
    paths: Iterable[Path],
    bucket_sec: int = 300,
    recent_limit: int = 200,
    log_paths: Iterable[Path] = (),
    live_health: dict[str, Any] | None = None,
) -> dict[str, Any]:
    analyzer = LossAnalyzer(bucket_sec=bucket_sec, recent_limit=recent_limit)
    for path in sorted((Path(p) for p in paths), key=_file_sort_key):
        analyzer.process_file(path)
    for path in sorted((Path(p) for p in log_paths), key=lambda p: p.name):
        analyzer.process_log_file(path)
    if live_health is not None:
        analyzer.apply_live_health_snapshot(live_health)
    return analyzer.result()


def resolve_inputs(inputs: Iterable[str]) -> list[Path]:
    paths: list[Path] = []
    seen: set[Path] = set()
    for value in inputs:
        expanded = [Path(p) for p in glob(value)] or [Path(value)]
        for path in expanded:
            if path.is_dir():
                candidates = sorted(path.glob("health_*.jsonl*"))
            else:
                candidates = [path]
            for candidate in candidates:
                resolved = candidate.resolve()
                if resolved in seen:
                    continue
                if not resolved.is_file():
                    raise FileNotFoundError(str(candidate))
                seen.add(resolved)
                paths.append(resolved)
    return sorted(paths, key=_file_sort_key)


def resolve_log_inputs(inputs: Iterable[str]) -> list[Path]:
    paths: list[Path] = []
    seen: set[Path] = set()
    for value in inputs:
        expanded = [Path(p) for p in glob(value)] or [Path(value)]
        for path in expanded:
            candidates = sorted(path.glob("csdaq_*.log")) if path.is_dir() else [path]
            for candidate in candidates:
                resolved = candidate.resolve()
                if resolved in seen:
                    continue
                if not resolved.is_file():
                    raise FileNotFoundError(str(candidate))
                seen.add(resolved)
                paths.append(resolved)
    return sorted(paths, key=lambda p: p.name)


def find_sibling_logs(health_paths: Iterable[Path]) -> list[Path]:
    logs: list[Path] = []
    seen: set[Path] = set()
    for path in health_paths:
        run_id = _run_id(path)
        if not run_id:
            continue
        log_path = path.with_name(f"csdaq_{run_id}.log").resolve()
        if log_path in seen or not log_path.is_file():
            continue
        seen.add(log_path)
        logs.append(log_path)
    return sorted(logs, key=lambda p: p.name)


def write_summary(result: dict[str, Any], output_path: Path) -> None:
    output_path.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def fetch_live_health(url: str, timeout_sec: float = 5.0) -> dict[str, Any]:
    with urllib.request.urlopen(url, timeout=timeout_sec) as response:
        return json.load(response)


def plot_loss_distribution(result: dict[str, Any], output_path: Path, focus_loss_window: bool = True) -> None:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.dates as mdates
    import matplotlib.pyplot as plt

    buckets = result.get("buckets") or []
    fig, ax1 = plt.subplots(figsize=(14, 5))

    if not buckets:
        ax1.text(0.5, 0.5, "No loss buckets found", ha="center", va="center", transform=ax1.transAxes)
        ax1.set_axis_off()
    else:
        times = [
            datetime.fromtimestamp(_to_int(bucket.get("start_unix_ms")) / 1000)
            for bucket in buckets
        ]
        missing = [_to_int(bucket.get("missing_fragments")) for bucket in buckets]
        loss_pct = [_to_float(bucket.get("loss_pct")) * 100.0 for bucket in buckets]
        width_days = max(result.get("bucket_sec", 300), 1) / 86400.0 * 0.8

        ax1.bar(times, missing, width=width_days, color="#5470c6", alpha=0.75, label="Missing fragments")
        ax1.set_ylabel("fragments")
        ax1.set_ylim(0, 1 if max(missing, default=0) == 0 else max(missing) * 1.15)
        ax1.grid(axis="y", alpha=0.25)

        ax2 = ax1.twinx()
        ax2.plot(times, loss_pct, color="#91cc75", marker="o", linewidth=1.5, markersize=3, label="Loss pct")
        ax2.set_ylabel("%")
        ax2.set_ylim(0, 1 if max(loss_pct, default=0.0) == 0 else max(loss_pct) * 1.15)

        ax1.xaxis.set_major_locator(mdates.AutoDateLocator(minticks=4, maxticks=10))
        ax1.xaxis.set_major_formatter(mdates.DateFormatter("%m-%d %H:%M"))
        if focus_loss_window:
            limits = loss_focus_limits(buckets, _to_int(result.get("bucket_sec"), 300))
            if limits is not None:
                ax1.set_xlim(
                    datetime.fromtimestamp(limits[0] / 1000),
                    datetime.fromtimestamp(limits[1] / 1000),
                )
        fig.autofmt_xdate()

        handles1, labels1 = ax1.get_legend_handles_labels()
        handles2, labels2 = ax2.get_legend_handles_labels()
        ax1.legend(handles1 + handles2, labels1 + labels2, loc="upper right")

    totals = result.get("totals") or {}
    title = (
        "Loss time distribution "
        f"(bucket={result.get('bucket_sec')}s, source={result.get('source')}, "
        f"missing={_to_int(totals.get('missing_fragments'))}, "
        f"loss={_to_float(totals.get('loss_pct')) * 100:.4f}%)"
    )
    ax1.set_title(title)
    fig.tight_layout()
    fig.savefig(output_path, dpi=150)
    plt.close(fig)


def _event_rows(events: list[dict[str, Any]], max_rows: int) -> list[list[str]]:
    selected = sorted(events, key=lambda e: _to_int(e.get("observed_unix_ms")), reverse=True)
    if max_rows > 0:
        selected = selected[:max_rows]

    rows: list[list[str]] = []
    for event in selected:
        missing_du = ",".join(str(du) for du in event.get("missing_du") or [])
        if len(missing_du) > 48:
            missing_du = missing_du[:45] + "..."
        expected = _to_int(event.get("expected"))
        attached = _to_int(event.get("attached"))
        attached_expected = "-" if expected == 0 and attached == 0 else f"{attached}/{expected}"
        rows.append(
            [
                _format_local_ms(_to_int(event.get("observed_unix_ms"))),
                str(_to_int(event.get("tag"))),
                str(_to_int(event.get("event_ts_ns"))),
                missing_du,
                attached_expected,
            ]
        )
    return rows


def plot_recent_events(result: dict[str, Any], output_path: Path, max_rows: int = 50) -> None:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    events = result.get("recent_events") or []
    rows = _event_rows(events, max_rows)
    row_count = max(1, len(rows))
    fig_height = min(18, max(3.5, 1.1 + row_count * 0.38))
    fig, ax = plt.subplots(figsize=(14, fig_height))
    ax.axis("off")

    title = f"Recent loss events from offline JSONL/logs (showing {len(rows)} of {len(events)})"
    ax.set_title(title, loc="left", pad=12, fontsize=12, fontweight="bold")

    if rows:
        table = ax.table(
            cellText=rows,
            colLabels=["observed", "tag", "event_ts_ns", "missing DU", "attached/expected"],
            loc="center",
            cellLoc="left",
            colLoc="left",
            colWidths=[0.20, 0.10, 0.20, 0.34, 0.16],
        )
        table.auto_set_font_size(False)
        table.set_fontsize(8)
        table.scale(1, 1.25)
        for (row, _col), cell in table.get_celld().items():
            if row == 0:
                cell.set_text_props(weight="bold")
                cell.set_facecolor("#eef2f7")
            else:
                cell.set_facecolor("#ffffff" if row % 2 else "#f8fafc")
    else:
        ax.text(0.5, 0.5, "No recent loss events found", ha="center", va="center", transform=ax.transAxes)

    fig.tight_layout()
    fig.savefig(output_path, dpi=150)
    plt.close(fig)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Analyze CSDAQ health JSONL files and render offline loss plots."
    )
    parser.add_argument(
        "inputs",
        nargs="+",
        help="health_*.jsonl files, .jsonl.gz files, directories, or quoted glob patterns",
    )
    parser.add_argument("--bucket-sec", type=int, default=300, help="loss distribution bucket size")
    parser.add_argument("--recent-limit", type=int, default=500, help="keep last N deduplicated recent events; 0 keeps all")
    parser.add_argument("--table-rows", type=int, default=50, help="max rows rendered in recent event table image")
    parser.add_argument(
        "--log",
        action="append",
        default=[],
        help="extra csdaq_*.log file, directory, or glob for timeout fallback; can be repeated",
    )
    parser.add_argument(
        "--no-log-fallback",
        action="store_true",
        help="do not auto-read sibling csdaq_<run_id>.log files when health JSONL has parse gaps",
    )
    parser.add_argument(
        "--live-health-url",
        help="optional live /api/health URL used to fill current loss_stats recent_events and buckets",
    )
    parser.add_argument(
        "--full-time-range",
        action="store_true",
        help="plot the full input time range instead of zooming to buckets with loss",
    )
    parser.add_argument(
        "-o",
        "--output-dir",
        default="health_loss_report",
        help="directory for PNG and JSON outputs",
    )
    parser.add_argument("--no-plots", action="store_true", help="only write summary JSON")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv if argv is not None else sys.argv[1:])

    try:
        paths = resolve_inputs(args.inputs)
    except FileNotFoundError as exc:
        print(f"ERROR: input not found: {exc}", file=sys.stderr)
        return 2

    if not paths:
        print("ERROR: no input files", file=sys.stderr)
        return 2

    try:
        log_paths = resolve_log_inputs(args.log)
    except FileNotFoundError as exc:
        print(f"ERROR: log input not found: {exc}", file=sys.stderr)
        return 2
    if not args.no_log_fallback:
        known_logs = {path.resolve() for path in log_paths}
        for path in find_sibling_logs(paths):
            if path.resolve() not in known_logs:
                log_paths.append(path)
                known_logs.add(path.resolve())

    live_health = None
    if args.live_health_url:
        try:
            live_health = fetch_live_health(args.live_health_url)
        except (OSError, urllib.error.URLError, json.JSONDecodeError) as exc:
            print(f"ERROR: live health fetch failed: {exc}", file=sys.stderr)
            return 2

    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    result = analyze_files(
        paths,
        bucket_sec=args.bucket_sec,
        recent_limit=args.recent_limit,
        log_paths=log_paths,
        live_health=live_health,
    )
    summary_path = output_dir / "loss_summary.json"
    write_summary(result, summary_path)

    if not args.no_plots:
        plot_loss_distribution(
            result,
            output_dir / "loss_distribution.png",
            focus_loss_window=not args.full_time_range,
        )
        plot_recent_events(result, output_dir / "recent_loss_events.png", max_rows=args.table_rows)

    totals = result["totals"]
    print(f"Input files: {len(paths)}")
    print(
        f"Lines: {result['total_lines']}  Parse errors: {result['parse_errors']}  "
        f"Health parse errors: {result['health_parse_errors']}"
    )
    if log_paths:
        print(f"Log fallback files: {len(log_paths)}  Log lines: {result['log_lines']}")
    if args.live_health_url:
        print(f"Live health: {args.live_health_url}")
    print(
        "Totals: "
        f"expected={totals['expected_fragments']} "
        f"missing={totals['missing_fragments']} "
        f"timeout_events={totals['timeout_events']} "
        f"loss_pct={totals['loss_pct'] * 100:.6f}%"
    )
    print(f"Source: {result['source']}")
    print(f"Output: {output_dir.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
