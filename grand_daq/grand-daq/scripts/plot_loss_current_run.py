#!/usr/bin/env python3

from __future__ import annotations

import argparse
import csv
import gzip
import json
import re
import sys
from collections import Counter, defaultdict
from datetime import datetime, timedelta, timezone
from glob import glob
from pathlib import Path
from typing import Any, Iterable


REPO_DIR = Path(__file__).resolve().parents[1]
DEFAULT_LOGS_DIR = REPO_DIR / "logs"
DEFAULT_OUTPUT_DIR = REPO_DIR.parents[1] / "analysis_outputs"

_HEALTH_NAME_RE = re.compile(
    r"^health_(?P<run>\d{8}_\d{6})(?:_(?P<day>\d{8}))?\.jsonl(?:\.gz)?$"
)


def open_text(path: Path):
    if path.name.endswith(".gz"):
        return gzip.open(path, "rt", encoding="utf-8", errors="replace")
    return path.open("r", encoding="utf-8", errors="replace")


def health_file_sort_key(path: Path) -> tuple[str, str, str]:
    match = _HEALTH_NAME_RE.match(path.name)
    if not match:
        return ("", "", path.name)
    run_id = match.group("run")
    day = match.group("day") or run_id[:8]
    return (run_id, day, path.name)


def resolve_inputs(inputs: Iterable[str]) -> list[Path]:
    paths: list[Path] = []
    seen: set[Path] = set()
    for value in inputs:
        expanded = [Path(p) for p in glob(value)] or [Path(value)]
        for path in expanded:
            candidates = sorted(path.glob("health_*.jsonl*")) if path.is_dir() else [path]
            for candidate in candidates:
                resolved = candidate.resolve()
                if resolved in seen:
                    continue
                if not resolved.is_file():
                    raise FileNotFoundError(str(candidate))
                if not _HEALTH_NAME_RE.match(resolved.name):
                    continue
                seen.add(resolved)
                paths.append(resolved)
    return sorted(paths, key=health_file_sort_key)


def run_id_from_path(path: Path) -> str | None:
    match = _HEALTH_NAME_RE.match(path.name)
    return match.group("run") if match else None


def latest_run_id(paths: list[Path]) -> str | None:
    run_ids = [run_id for path in paths if (run_id := run_id_from_path(path))]
    return max(run_ids) if run_ids else None


def run_files(paths: list[Path], run_id: str) -> list[Path]:
    return [path for path in paths if run_id_from_path(path) == run_id]


def parse_iso_ts(text: str) -> datetime | None:
    try:
        return datetime.fromisoformat(text.replace("Z", "+00:00"))
    except ValueError:
        return None


def parse_local_time(text: str, tz: timezone) -> datetime:
    for fmt in ("%Y-%m-%d %H:%M:%S", "%Y-%m-%d %H:%M", "%Y-%m-%d"):
        try:
            return datetime.strptime(text, fmt).replace(tzinfo=tz)
        except ValueError:
            pass
    raise argparse.ArgumentTypeError(f"invalid time: {text!r}")


def first_builder_ts(path: Path) -> datetime | None:
    with open_text(path) as stream:
        for line in stream:
            line = line.strip()
            if not line:
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError:
                continue
            ts = parse_iso_ts(str(record.get("ts", "")))
            health = record.get("health")
            if ts is None or not isinstance(health, dict):
                continue
            if isinstance(health.get("builder"), dict):
                return ts
    return None


def last_builder_ts(path: Path) -> datetime | None:
    last: datetime | None = None
    with open_text(path) as stream:
        for line in stream:
            line = line.strip()
            if not line:
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError:
                continue
            ts = parse_iso_ts(str(record.get("ts", "")))
            health = record.get("health")
            if ts is None or not isinstance(health, dict):
                continue
            if isinstance(health.get("builder"), dict):
                last = ts
    return last


def run_time_bounds(paths: list[Path]) -> tuple[datetime | None, datetime | None]:
    if not paths:
        return None, None
    start = first_builder_ts(paths[0])
    end = last_builder_ts(paths[-1])
    return start, end


def find_best_run_for_window(
    paths: list[Path],
    start_utc: datetime,
    end_utc: datetime,
) -> tuple[str | None, list[Path]]:
    by_run: dict[str, list[Path]] = defaultdict(list)
    for path in paths:
        run_id = run_id_from_path(path)
        if run_id:
            by_run[run_id].append(path)

    best_run: str | None = None
    best_files: list[Path] = []
    best_overlap = -1.0
    for run_id, files in by_run.items():
        sorted_files = sorted(files, key=health_file_sort_key)
        run_start, run_end = run_time_bounds(sorted_files)
        if run_start is None or run_end is None:
            continue
        overlap_start = max(run_start, start_utc)
        overlap_end = min(run_end, end_utc)
        overlap_sec = (overlap_end - overlap_start).total_seconds()
        if overlap_sec > best_overlap:
            best_overlap = overlap_sec
            best_run = run_id
            best_files = sorted_files
    return best_run, best_files


def builder_field(builder: dict[str, Any], name: str) -> int:
    try:
        return int(builder.get(name, 0))
    except (TypeError, ValueError):
        return 0


def builder_counter(health: dict[str, Any], name: str) -> int:
    builder = health.get("builder")
    if not isinstance(builder, dict):
        return 0
    return builder_field(builder, name)


def builder_per_du_missing(health: dict[str, Any]) -> dict[int, int]:
    builder = health.get("builder")
    raw = builder.get("missing_on_timeout_per_du") if isinstance(builder, dict) else None
    if not isinstance(raw, dict):
        return {}
    out: dict[int, int] = {}
    for du_id, count in raw.items():
        try:
            out[int(du_id)] = int(count)
        except (TypeError, ValueError):
            continue
    return out


def scan_run_loss(
    paths: list[Path],
    local_tz: timezone,
    bucket_sec: int,
    start_utc: datetime | None = None,
    end_utc: datetime | None = None,
) -> dict[str, Any]:
    if not paths:
        raise ValueError("no health files for run")

    bucket_ms = max(1, bucket_sec) * 1000
    buckets: dict[int, dict[str, Any]] = defaultdict(
        lambda: {
            "timeout_events": 0,
            "missing_fragments": 0,
            "expected_fragments": 0,
        }
    )
    per_du_missing: Counter[int] = Counter()

    first_builder: dict[str, Any] | None = None
    last_builder: dict[str, Any] | None = None
    first_ts: datetime | None = None
    last_ts: datetime | None = None
    prev_health: dict[str, Any] | None = None
    seen_ts: set[datetime] = set()
    files_used: list[str] = []

    for path in paths:
        files_used.append(path.name)
        with open_text(path) as stream:
            for line in stream:
                line = line.strip()
                if not line:
                    continue
                try:
                    record = json.loads(line)
                except json.JSONDecodeError:
                    continue
                ts = parse_iso_ts(str(record.get("ts", "")))
                health = record.get("health")
                if ts is None or not isinstance(health, dict):
                    continue
                if end_utc is not None and ts > end_utc:
                    break
                if ts in seen_ts:
                    continue

                builder = health.get("builder")
                if not isinstance(builder, dict):
                    continue

                if start_utc is not None and ts < start_utc:
                    prev_health = health
                    continue

                seen_ts.add(ts)
                if first_builder is None:
                    first_builder = builder
                    first_ts = ts
                last_builder = builder
                last_ts = ts

                if prev_health is not None:
                    t_ms = int(ts.timestamp() * 1000)
                    start_ms = (t_ms // bucket_ms) * bucket_ms
                    bucket = buckets[start_ms]

                    expected_delta = max(
                        0,
                        builder_counter(health, "expected_fragments")
                        - builder_counter(prev_health, "expected_fragments"),
                    )
                    missing_delta = max(
                        0,
                        builder_counter(health, "missing_on_timeout")
                        - builder_counter(prev_health, "missing_on_timeout"),
                    )
                    timeout_delta = max(
                        0,
                        builder_counter(health, "timeout_events")
                        - builder_counter(prev_health, "timeout_events"),
                    )
                    bucket["expected_fragments"] += expected_delta
                    bucket["missing_fragments"] += missing_delta
                    bucket["timeout_events"] += timeout_delta

                    prev_du = builder_per_du_missing(prev_health)
                    cur_du = builder_per_du_missing(health)
                    for du_id, count in cur_du.items():
                        delta = max(0, count - prev_du.get(du_id, 0))
                        if delta:
                            per_du_missing[du_id] += delta

                prev_health = health

    if first_builder is None or last_builder is None or first_ts is None or last_ts is None:
        raise ValueError("no builder counters found in run files")

    complete_events = builder_field(last_builder, "complete_events") - builder_field(
        first_builder, "complete_events"
    )
    timeout_events = builder_field(last_builder, "timeout_events") - builder_field(
        first_builder, "timeout_events"
    )
    missing_fragments = builder_field(last_builder, "missing_on_timeout") - builder_field(
        first_builder, "missing_on_timeout"
    )
    expected_fragments = builder_field(last_builder, "expected_fragments") - builder_field(
        first_builder, "expected_fragments"
    )
    loss_pct = missing_fragments / expected_fragments if expected_fragments > 0 else 0.0
    timeout_pct = timeout_events / complete_events if complete_events > 0 else 0.0

    bucket_rows: list[dict[str, Any]] = []
    for start_ms in sorted(buckets):
        bucket = buckets[start_ms]
        start_local = datetime.fromtimestamp(start_ms / 1000, tz=local_tz)
        bucket_rows.append(
            {
                "start_local": start_local.isoformat(),
                "timeout_events": int(bucket["timeout_events"]),
                "missing_fragments": int(bucket["missing_fragments"]),
                "expected_fragments": int(bucket["expected_fragments"]),
            }
        )

    du_rows = [
        {
            "du_id": du_id,
            "missing_fragments": count,
            "share_pct": round(count / missing_fragments * 100, 2) if missing_fragments else 0.0,
        }
        for du_id, count in per_du_missing.most_common()
    ]

    nonzero_buckets = [row for row in bucket_rows if row["timeout_events"] > 0]
    affected_du_count = len(du_rows)
    top_du_share = du_rows[0]["share_pct"] if du_rows else 0.0
    top3_share = (
        round(sum(row["share_pct"] for row in du_rows[:3]), 2) if len(du_rows) >= 3 else top_du_share
    )
    concentrated_timeout = sum(row["timeout_events"] for row in nonzero_buckets)
    timeout_in_spike_windows_pct = (
        round(concentrated_timeout / timeout_events * 100, 1) if timeout_events else 0.0
    )

    payload: dict[str, Any] = {
        "run_id": run_id_from_path(paths[0]),
        "files_used": files_used,
        "range_local": {
            "start": first_ts.astimezone(local_tz).isoformat(),
            "end": last_ts.astimezone(local_tz).isoformat(),
        },
        "bucket_sec": bucket_sec,
        "totals": {
            "complete_events": complete_events,
            "timeout_events": timeout_events,
            "missing_fragments": missing_fragments,
            "expected_fragments": expected_fragments,
            "loss_pct": round(loss_pct, 6),
            "timeout_pct": round(timeout_pct, 6),
        },
        "summary": {
            "affected_du_count": affected_du_count,
            "nonzero_timeout_buckets": len(nonzero_buckets),
            "top_du_share_pct": top_du_share,
            "top3_du_share_pct": top3_share,
            "timeout_in_spike_windows_pct": timeout_in_spike_windows_pct,
        },
        "buckets": bucket_rows,
        "by_du": du_rows,
    }
    if start_utc is not None or end_utc is not None:
        payload["requested_range_local"] = {
            "start": None if start_utc is None else start_utc.astimezone(local_tz).isoformat(),
            "end": None if end_utc is None else end_utc.astimezone(local_tz).isoformat(),
        }
    return payload


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    if not rows:
        return
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def _style_axes(ax) -> None:
    for spine in ax.spines.values():
        spine.set_visible(True)
        spine.set_color("#9ca3af")
        spine.set_linewidth(0.9)
    ax.grid(which="major", axis="both", color="#d1d5db", linewidth=0.8, alpha=0.58)
    ax.tick_params(colors="#111827", labelsize=11.5)
    ax.set_axisbelow(True)


def _time_axis(ax, plot_start: datetime, plot_end: datetime, local_tz: timezone) -> None:
    import matplotlib.dates as mdates

    span_days = max(1, (plot_end - plot_start).days)
    if span_days <= 2:
        locator = mdates.HourLocator(interval=6)
        fmt = "%m-%d %H:%M"
    elif span_days <= 7:
        locator = mdates.DayLocator(interval=1)
        fmt = "%m-%d"
    else:
        locator = mdates.DayLocator(interval=2)
        fmt = "%m-%d"
    ax.xaxis.set_major_locator(locator)
    ax.xaxis.set_major_formatter(mdates.DateFormatter(fmt, tz=local_tz))
    ax.set_xlabel("Beijing Time (UTC+8)", fontsize=12.5, color="#374151", labelpad=8)


def _draw_header(
    fig,
    *,
    title: str,
    subtitle: str,
    plot_start: datetime,
    plot_end: datetime,
) -> None:
    fig.text(
        0.055,
        0.965,
        title,
        ha="left",
        va="top",
        fontsize=18,
        color="#111827",
        fontweight="bold",
    )
    fig.text(
        0.055,
        0.935,
        subtitle,
        ha="left",
        va="top",
        fontsize=12,
        color="#111827",
    )
    fig.text(
        0.055,
        0.905,
        f"Data Period: {plot_start:%B %d, %Y} – {plot_end:%B %d, %Y} (UTC+8)",
        ha="left",
        va="top",
        fontsize=12,
        color="#111827",
    )


def plot_loss_run_stability(result: dict[str, Any], output_path: Path, local_tz: timezone) -> None:
    """Minimal PPT layout: completion-first, no spike charts or DU bars."""
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.patches import FancyBboxPatch

    totals = result["totals"]
    complete_events = int(totals["complete_events"])
    timeout_events = int(totals["timeout_events"])
    timeout_pct = float(totals["timeout_pct"]) * 100.0
    loss_pct = float(totals["loss_pct"]) * 100.0
    complete_pct = max(0.0, 100.0 - timeout_pct)

    plot_start = datetime.fromisoformat(result["range_local"]["start"])
    plot_end = datetime.fromisoformat(result["range_local"]["end"])
    run_id = result.get("run_id") or "current"

    line_color = "#1d4ed8"
    muted = "#64748b"
    light = "#94a3b8"

    fig = plt.figure(figsize=(12.8, 5.6), dpi=160)
    fig.patch.set_facecolor("white")
    ax = fig.add_axes([0.07, 0.12, 0.91, 0.62])
    ax_metrics = fig.add_axes([0.07, 0.02, 0.91, 0.08])
    ax.axis("off")
    ax_metrics.axis("off")

    fig.text(
        0.055,
        0.96,
        "Packet Loss Monitoring (Current Run)",
        ha="left",
        va="top",
        fontsize=18,
        color="#111827",
        fontweight="bold",
    )
    fig.text(
        0.055,
        0.905,
        f"run {run_id}",
        ha="left",
        va="top",
        fontsize=12,
        color="#111827",
    )
    fig.text(
        0.055,
        0.875,
        f"Data Period: {plot_start:%B %d, %Y} – {plot_end:%B %d, %Y} (UTC+8)",
        ha="left",
        va="top",
        fontsize=12,
        color="#111827",
    )

    ax.text(
        0.5,
        0.78,
        f"{complete_pct:.3f}%",
        ha="center",
        va="center",
        fontsize=42,
        color=line_color,
        fontweight="bold",
        transform=ax.transAxes,
    )
    ax.text(
        0.5,
        0.62,
        "Event Completion Rate",
        ha="center",
        va="center",
        fontsize=14,
        color="#374151",
        transform=ax.transAxes,
    )

    bar_y = 0.38
    bar_h = 0.12
    ax.barh(
        bar_y,
        complete_pct,
        height=bar_h,
        left=0,
        color=line_color,
        transform=ax.transAxes,
    )
    if timeout_pct > 0:
        ax.barh(
            bar_y,
            timeout_pct,
            height=bar_h,
            left=complete_pct,
            color=light,
            transform=ax.transAxes,
        )
    ax.plot([0, 1], [bar_y - bar_h * 0.9, bar_y - bar_h * 0.9], color="#cbd5e1", lw=0.8, transform=ax.transAxes)
    ax.plot([0, 0], [bar_y - bar_h * 0.9, bar_y + bar_h], color="#cbd5e1", lw=0.8, transform=ax.transAxes)
    ax.plot([1, 1], [bar_y - bar_h * 0.9, bar_y + bar_h], color="#cbd5e1", lw=0.8, transform=ax.transAxes)
    ax.text(0, bar_y - bar_h * 1.8, "0%", ha="left", va="top", fontsize=11, color=muted, transform=ax.transAxes)
    ax.text(1, bar_y - bar_h * 1.8, "100%", ha="right", va="top", fontsize=11, color=muted, transform=ax.transAxes)
    ax.text(
        complete_pct / 200,
        bar_y,
        f"Complete {complete_events:,}",
        ha="center",
        va="center",
        fontsize=11.5,
        color="white",
        fontweight="bold",
        transform=ax.transAxes,
    )
    ax.text(
        0.992,
        bar_y,
        f"Timeout {timeout_events:,}",
        ha="right",
        va="center",
        fontsize=11,
        color=muted,
        transform=ax.transAxes,
    )

    metrics = [
        ("Complete Events", f"{complete_events:,}"),
        ("Timeout Events", f"{timeout_events:,}"),
        ("Timeout Rate", f"{timeout_pct:.3f}%"),
        ("Fragment Loss Rate", f"{loss_pct:.4f}%"),
    ]
    n = len(metrics)
    for idx, (label, value) in enumerate(metrics):
        x = (idx + 0.5) / n
        patch = FancyBboxPatch(
            (x - 0.44 / n, 0.05),
            0.88 / n,
            0.9,
            boxstyle="round,pad=0.02,rounding_size=0.02",
            linewidth=0.9,
            edgecolor="#cbd5e1",
            facecolor="#f8fafc",
            transform=ax_metrics.transAxes,
        )
        ax_metrics.add_patch(patch)
        ax_metrics.text(
            x,
            0.68,
            label,
            ha="center",
            va="center",
            fontsize=10.5,
            color="#374151",
            transform=ax_metrics.transAxes,
        )
        ax_metrics.text(
            x,
            0.30,
            value,
            ha="center",
            va="center",
            fontsize=13,
            color="#111827" if idx == 0 else muted,
            fontweight="bold" if idx == 0 else "normal",
            transform=ax_metrics.transAxes,
        )

    output_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(output_path, dpi=160, bbox_inches="tight")
    plt.close(fig)


def plot_loss_run_absolute(result: dict[str, Any], output_path: Path, local_tz: timezone) -> None:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.dates as mdates
    import matplotlib.pyplot as plt
    from matplotlib.patches import FancyBboxPatch

    totals = result["totals"]
    summary = result["summary"]
    bucket_rows = result["buckets"]
    du_rows = result["by_du"][:10]
    if not bucket_rows:
        raise ValueError("no bucket rows to plot")

    complete_events = int(totals["complete_events"])
    timeout_events = int(totals["timeout_events"])
    missing_fragments = int(totals["missing_fragments"])
    timeout_pct = float(totals["timeout_pct"]) * 100.0
    loss_pct = float(totals["loss_pct"]) * 100.0

    times = [datetime.fromisoformat(row["start_local"]) for row in bucket_rows]
    timeout_counts = [int(row["timeout_events"]) for row in bucket_rows]
    plot_start = datetime.fromisoformat(result["range_local"]["start"])
    plot_end = datetime.fromisoformat(result["range_local"]["end"])

    line_color = "#1d4ed8"
    accent_orange = "#ea580c"
    band_color = "#dbeafe"
    muted = "#6b7280"

    fig = plt.figure(figsize=(12.8, 7.8), dpi=160)
    fig.patch.set_facecolor("white")
    gs = fig.add_gridspec(
        3,
        1,
        height_ratios=[0.72, 2.0, 1.15],
        hspace=0.28,
        left=0.07,
        right=0.985,
        top=0.90,
        bottom=0.10,
    )
    ax_kpi = fig.add_subplot(gs[0, 0])
    ax_time = fig.add_subplot(gs[1, 0])
    ax_du = fig.add_subplot(gs[2, 0])

    run_id = result.get("run_id") or "current"
    subtitle = (
        f"run {run_id} • timeout {timeout_events:,} / complete {complete_events:,} "
        f"({timeout_pct:.3f}%) • {summary['affected_du_count']} DUs affected"
    )
    fig.text(
        0.055,
        0.965,
        "Packet Loss Monitoring (Current Run)",
        ha="left",
        va="top",
        fontsize=18,
        color="#111827",
        fontweight="bold",
    )
    fig.text(
        0.055,
        0.935,
        subtitle,
        ha="left",
        va="top",
        fontsize=12,
        color="#111827",
    )
    fig.text(
        0.055,
        0.905,
        f"Data Period: {plot_start:%B %d, %Y} – {plot_end:%B %d, %Y} (UTC+8)",
        ha="left",
        va="top",
        fontsize=12,
        color="#111827",
    )

    ax_kpi.set_xlim(0, 1)
    ax_kpi.set_ylim(0, 1)
    ax_kpi.axis("off")

    cards = [
        ("Complete Events", f"{complete_events:,}", line_color),
        ("Timeout Events", f"{timeout_events:,}", accent_orange),
        ("Timeout Rate", f"{timeout_pct:.3f}%", accent_orange),
        ("Missing Fragments", f"{missing_fragments:,}", accent_orange),
        ("Fragment Loss Rate", f"{loss_pct:.4f}%", accent_orange),
    ]
    card_width = 0.18
    gap = 0.015
    start_x = 0.01
    for idx, (label, value, color) in enumerate(cards):
        x = start_x + idx * (card_width + gap)
        patch = FancyBboxPatch(
            (x, 0.08),
            card_width,
            0.84,
            boxstyle="round,pad=0.02,rounding_size=0.02",
            linewidth=0.9,
            edgecolor="#cbd5e1",
            facecolor="#f8fafc",
            transform=ax_kpi.transAxes,
        )
        ax_kpi.add_patch(patch)
        ax_kpi.text(
            x + card_width / 2,
            0.72,
            label,
            ha="center",
            va="center",
            fontsize=11,
            color="#374151",
            transform=ax_kpi.transAxes,
        )
        ax_kpi.text(
            x + card_width / 2,
            0.34,
            value,
            ha="center",
            va="center",
            fontsize=16,
            color=color,
            fontweight="bold",
            transform=ax_kpi.transAxes,
        )

    bar_width = max(timedelta(minutes=max(1, int(result["bucket_sec"]) // 60)), timedelta(minutes=1))
    bucket_minutes = max(1, int(result["bucket_sec"]) // 60)
    ax_time.bar(
        times,
        timeout_counts,
        width=bar_width.total_seconds() / 86400,
        color=accent_orange,
        alpha=0.88,
        linewidth=0,
        label=f"{bucket_minutes}min timeout events",
    )
    ax_time.set_ylabel(f"Timeout Events / {bucket_minutes} min", fontsize=12.5, color="#374151")
    ax_time.set_xlim(plot_start, plot_end)
    ymax = max(timeout_counts) if timeout_counts else 1
    ax_time.set_ylim(0, max(5, ymax * 1.15))
    ax_time.text(
        0.01,
        0.97,
        f"Loss concentrated in {summary['nonzero_timeout_buckets']} short windows "
        f"({summary['timeout_in_spike_windows_pct']:.0f}% of timeout events)",
        transform=ax_time.transAxes,
        ha="left",
        va="top",
        fontsize=11.5,
        color="#111827",
        bbox=dict(
            boxstyle="round,pad=0.35",
            facecolor="#e5e7eb",
            edgecolor="#6b7280",
            linewidth=0.9,
            alpha=0.96,
        ),
    )

    if du_rows:
        du_labels = [f"DU {row['du_id']}" for row in du_rows]
        du_values = [int(row["missing_fragments"]) for row in du_rows]
        y_pos = list(range(len(du_rows)))
        ax_du.barh(
            y_pos,
            du_values,
            color=line_color,
            alpha=0.92,
            height=0.68,
            label="Missing fragments by DU",
        )
        ax_du.set_yticks(y_pos)
        ax_du.set_yticklabels(du_labels)
        ax_du.invert_yaxis()
        ax_du.set_xlabel("Missing Fragments (this run)", fontsize=12.5, color="#374151")
        for idx, row in enumerate(du_rows):
            ax_du.text(
                du_values[idx] + max(du_values) * 0.02,
                idx,
                f"{row['missing_fragments']} ({row['share_pct']:.1f}%)",
                va="center",
                ha="left",
                fontsize=10.5,
                color="#111827",
            )
        ax_du.text(
            0.99,
            0.03,
            f"Top 3 DUs account for {summary['top3_du_share_pct']:.1f}% of missing fragments",
            transform=ax_du.transAxes,
            ha="right",
            va="bottom",
            fontsize=11.5,
            color=muted,
        )
    else:
        ax_du.text(0.5, 0.5, "No per-DU loss recorded", ha="center", va="center", fontsize=12)

    for ax in (ax_time, ax_du):
        _style_axes(ax)
    _time_axis(ax_time, plot_start, plot_end, local_tz)
    ax_time.legend(loc="upper right", frameon=False, fontsize=10.5)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(output_path, dpi=160, bbox_inches="tight")
    plt.close(fig)


def plot_loss_run_distribution(result: dict[str, Any], output_path: Path, local_tz: timezone) -> None:
    """Two-panel layout: 5-min run-loss contribution timeline + ranked DU summary."""
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.dates as mdates
    import matplotlib.pyplot as plt

    totals = result["totals"]
    summary = result["summary"]
    bucket_rows = result["buckets"]
    du_rows = result["by_du"]
    if not bucket_rows:
        raise ValueError("no bucket rows to plot")

    missing_fragments = int(totals["missing_fragments"])
    expected_fragments = int(totals["expected_fragments"])
    loss_pct = float(totals["loss_pct"]) * 100.0
    plot_start = datetime.fromisoformat(result["range_local"]["start"])
    plot_end = datetime.fromisoformat(result["range_local"]["end"])
    bucket_minutes = max(1, int(result["bucket_sec"]) // 60)

    line_color = "#1d4ed8"
    band_color = "#dbeafe"
    muted = "#64748b"
    ref_color = "#94a3b8"

    times = [datetime.fromisoformat(row["start_local"]) for row in bucket_rows]
    contributions = [
        (int(row["missing_fragments"]) / expected_fragments * 100.0) if expected_fragments > 0 else 0.0
        for row in bucket_rows
    ]
    clean_windows = sum(1 for row in bucket_rows if int(row["missing_fragments"]) == 0)
    main_idx = max(range(len(bucket_rows)), key=lambda i: int(bucket_rows[i]["missing_fragments"]))
    main_row = bucket_rows[main_idx]
    main_missing = int(main_row["missing_fragments"])
    main_contrib = contributions[main_idx]

    top_n = 10
    top_rows = du_rows[:top_n]
    other_rows = du_rows[top_n:]
    du_labels = [f"DU {row['du_id']}" for row in top_rows]
    du_values = [int(row["missing_fragments"]) for row in top_rows]
    du_shares = [float(row["share_pct"]) for row in top_rows]
    if other_rows:
        other_missing = sum(int(row["missing_fragments"]) for row in other_rows)
        other_share = sum(float(row["share_pct"]) for row in other_rows)
        du_labels.append(f"Other {len(other_rows)} DUs")
        du_values.append(other_missing)
        du_shares.append(round(other_share, 2))

    fig = plt.figure(figsize=(12.8, 8.0), dpi=160)
    fig.patch.set_facecolor("white")
    gs = fig.add_gridspec(
        2,
        1,
        height_ratios=[1.35, 1.0],
        hspace=0.34,
        left=0.07,
        right=0.985,
        top=0.88,
        bottom=0.10,
    )
    ax_time = fig.add_subplot(gs[0, 0])
    ax_du = fig.add_subplot(gs[1, 0])

    fig.text(
        0.055,
        0.965,
        "Packet Loss Monitoring",
        ha="left",
        va="top",
        fontsize=18,
        color="#111827",
        fontweight="bold",
    )
    fig.text(
        0.055,
        0.935,
        f"top: {bucket_minutes}-minute contribution to run loss • bottom: DU summary • "
        f"run loss rate {loss_pct:.4f}%",
        ha="left",
        va="top",
        fontsize=12,
        color="#111827",
    )
    fig.text(
        0.055,
        0.905,
        f"Data Period: {plot_start:%B %d, %Y} – {plot_end:%B %d, %Y} (UTC+8)",
        ha="left",
        va="top",
        fontsize=12,
        color="#111827",
    )

    ax_time.fill_between(times, contributions, color=band_color, alpha=0.55, linewidth=0, label="5min filled area")
    ax_time.plot(
        times,
        contributions,
        color=line_color,
        linewidth=1.2,
        marker="o",
        markersize=3.2,
        markerfacecolor=line_color,
        markeredgecolor="white",
        markeredgewidth=0.4,
        label="5min run-loss contribution",
    )
    ax_time.axhline(
        loss_pct,
        color=ref_color,
        linestyle="--",
        linewidth=1.2,
        label="Run total loss rate",
    )
    ax_time.set_ylabel(f"Run loss contribution / {bucket_minutes} min (%)", fontsize=12.5, color="#374151")
    ax_time.set_xlim(plot_start, plot_end)
    ymax = max([loss_pct, *contributions], default=loss_pct)
    ax_time.set_ylim(0, max(ymax * 1.25, loss_pct * 1.5, 0.0001))
    ax_time.text(
        0.99,
        0.97,
        f"Run loss rate: {loss_pct:.4f}% / {clean_windows}/{len(bucket_rows)} clean 5-min windows",
        transform=ax_time.transAxes,
        ha="right",
        va="top",
        fontsize=11,
        color="#111827",
        bbox=dict(boxstyle="round,pad=0.35", facecolor="white", edgecolor="#cbd5e1", linewidth=0.9),
    )
    if main_missing > 0:
        ax_time.annotate(
            f"Main window: {main_missing} fragments / Run contribution {main_contrib:.4f}%",
            xy=(times[main_idx], contributions[main_idx]),
            xytext=(0.62, 0.78),
            textcoords="axes fraction",
            arrowprops=dict(arrowstyle="->", color=line_color, lw=1.0),
            fontsize=10.5,
            color="#111827",
            bbox=dict(boxstyle="round,pad=0.3", facecolor="white", edgecolor="#cbd5e1", linewidth=0.8),
        )

    x_pos = list(range(len(du_labels)))
    ax_du.bar(x_pos, du_values, color=band_color, alpha=0.92, linewidth=0, label="Missing fragments")
    ax_du.plot(
        x_pos,
        du_values,
        color=line_color,
        linewidth=1.4,
        marker="o",
        markersize=4.5,
        markerfacecolor=line_color,
        markeredgecolor="white",
        markeredgewidth=0.6,
        label="DU loss count",
    )
    ax_du.set_xticks(x_pos)
    ax_du.set_xticklabels(du_labels, rotation=35, ha="right")
    ax_du.set_ylabel("DU summary", fontsize=12.5, color="#374151")
    ax_du.set_xlabel("DUs sorted by missing fragments", fontsize=12.5, color="#374151")
    ymax_du = max(du_values) if du_values else 1
    ax_du.set_ylim(0, max(5, ymax_du * 1.18))
    for idx, (value, share) in enumerate(zip(du_values, du_shares)):
        ax_du.text(
            idx,
            value + ymax_du * 0.03,
            f"{value}\n{share:.1f}%",
            ha="center",
            va="bottom",
            fontsize=9.5,
            color="#111827",
        )
    if len(top_rows) >= 3:
        ax_du.axvline(2.5, color=ref_color, linestyle="--", linewidth=1.0, alpha=0.9)
        ax_du.text(
            2.5,
            ymax_du * 1.08,
            f"Top 3 DUs: {summary['top3_du_share_pct']:.1f}%",
            ha="center",
            va="bottom",
            fontsize=11,
            color=muted,
        )
    ax_du.text(
        0.99,
        0.97,
        f"{summary['affected_du_count']} DUs / {missing_fragments:,} missing / "
        f"{expected_fragments:,} expected",
        transform=ax_du.transAxes,
        ha="right",
        va="top",
        fontsize=11,
        color="#111827",
        bbox=dict(boxstyle="round,pad=0.35", facecolor="white", edgecolor="#cbd5e1", linewidth=0.9),
    )

    for ax in (ax_time, ax_du):
        _style_axes(ax)
    _time_axis(ax_time, plot_start, plot_end, local_tz)
    ax_time.legend(loc="lower left", frameon=False, fontsize=10.5)
    ax_du.legend(loc="lower right", frameon=False, fontsize=10.5)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(output_path, dpi=160, bbox_inches="tight")
    plt.close(fig)


def plot_loss_run(
    result: dict[str, Any],
    output_path: Path,
    local_tz: timezone,
    style: str = "stability",
) -> None:
    if style == "absolute":
        plot_loss_run_absolute(result, output_path, local_tz)
    elif style == "distribution":
        plot_loss_run_distribution(result, output_path, local_tz)
    elif style == "stability":
        plot_loss_run_stability(result, output_path, local_tz)
    else:
        raise ValueError(f"unknown plot style: {style}")


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Plot packet-loss summary for the latest or selected RUN from health jsonl files."
    )
    parser.add_argument(
        "inputs",
        nargs="*",
        default=[str(DEFAULT_LOGS_DIR)],
        help="health files, directories, or quoted glob patterns",
    )
    parser.add_argument("--run", help="RUN id like 20260604_095814; default latest in inputs")
    parser.add_argument("--start", help="local start time filter, YYYY-MM-DD[ HH:MM[:SS]]")
    parser.add_argument("--end", help="local end time filter, YYYY-MM-DD[ HH:MM[:SS]]")
    parser.add_argument(
        "--pick-run-for-range",
        action="store_true",
        help="with --start/--end, choose the single RUN with the longest overlap",
    )
    parser.add_argument("--bucket-sec", type=int, default=300, help="time bucket size, default 300s")
    parser.add_argument("--tz-offset-hours", type=float, default=8.0, help="local timezone offset from UTC")
    parser.add_argument("-o", "--output-dir", type=Path, default=DEFAULT_OUTPUT_DIR, help="output directory")
    parser.add_argument("--prefix", default="loss_current_run", help="output filename prefix")
    parser.add_argument(
        "--style",
        choices=("stability", "absolute", "distribution"),
        default="stability",
        help="stability=ratio-first (default); absolute=raw counts; distribution=timeline+DU summary",
    )
    parser.add_argument("--data-only", action="store_true", help="write JSON/CSV only, skip PNG")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_arg_parser().parse_args(argv)
    local_tz = timezone(timedelta(hours=args.tz_offset_hours))

    try:
        paths = resolve_inputs(args.inputs)
    except FileNotFoundError as exc:
        print(f"ERROR: input not found: {exc}", file=sys.stderr)
        return 2
    if not paths:
        print("ERROR: no health jsonl files found", file=sys.stderr)
        return 2

    start_utc: datetime | None = None
    end_utc: datetime | None = None
    if args.start:
        start_utc = parse_local_time(args.start, local_tz).astimezone(timezone.utc)
    if args.end:
        end_local = parse_local_time(args.end, local_tz)
        if args.end.count(":") == 0:
            end_local = end_local + timedelta(days=1) - timedelta(microseconds=1)
        end_utc = end_local.astimezone(timezone.utc)
    if start_utc and end_utc and end_utc <= start_utc:
        print("ERROR: --end must be later than --start", file=sys.stderr)
        return 2

    run_id = args.run
    selected: list[Path]
    if run_id:
        selected = run_files(paths, run_id)
    elif start_utc and end_utc and args.pick_run_for_range:
        run_id, selected = find_best_run_for_window(paths, start_utc, end_utc)
    else:
        run_id = latest_run_id(paths)
        selected = run_files(paths, run_id) if run_id else []

    if not run_id or not selected:
        print("ERROR: could not determine run id", file=sys.stderr)
        return 2

    try:
        result = scan_run_loss(
            selected,
            local_tz=local_tz,
            bucket_sec=int(args.bucket_sec),
            start_utc=start_utc,
            end_utc=end_utc,
        )
    except ValueError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2

    output_dir = args.output_dir
    output_dir.mkdir(parents=True, exist_ok=True)
    prefix = args.prefix
    png_path = output_dir / f"{prefix}.png"
    json_path = output_dir / f"{prefix}.json"
    bucket_csv_path = output_dir / f"{prefix}_buckets.csv"
    du_csv_path = output_dir / f"{prefix}_by_du.csv"

    json_path.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    write_csv(bucket_csv_path, result["buckets"])
    write_csv(du_csv_path, result["by_du"])
    if not args.data_only:
        plot_loss_run(result, png_path, local_tz, style=args.style)

    totals = result["totals"]
    summary = result["summary"]
    if not args.data_only:
        print(f"Output PNG: {png_path.resolve()}")
    print(f"Bucket CSV: {bucket_csv_path.resolve()}")
    print(f"By-DU CSV: {du_csv_path.resolve()}")
    print(f"JSON: {json_path.resolve()}")
    print(f"Run: {run_id}")
    print(
        "Range: "
        f"{result['range_local']['start']} to {result['range_local']['end']}"
    )
    print(
        f"Complete: {totals['complete_events']:,}  Timeout: {totals['timeout_events']:,}  "
        f"Timeout rate: {totals['timeout_pct'] * 100:.4f}%  "
        f"Missing fragments: {totals['missing_fragments']:,}  "
        f"Loss rate: {totals['loss_pct'] * 100:.4f}%"
    )
    print(
        f"Affected DUs: {summary['affected_du_count']}  "
        f"Nonzero timeout windows: {summary['nonzero_timeout_buckets']}  "
        f"Top-3 DU share: {summary['top3_du_share_pct']:.1f}%"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
