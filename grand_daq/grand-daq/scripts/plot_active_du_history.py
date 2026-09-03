#!/usr/bin/env python3

from __future__ import annotations

import argparse
import csv
import gzip
import json
import math
import re
import sys
from collections import defaultdict
from datetime import date, datetime, timedelta, timezone
from glob import glob
from pathlib import Path
from typing import Any, Iterable


REPO_DIR = Path(__file__).resolve().parents[1]
DEFAULT_LOGS_DIR = REPO_DIR / "logs"
DEFAULT_OUTPUT_DIR = REPO_DIR.parents[1] / "analysis_outputs"

_HEALTH_NAME_RE = re.compile(
    r"^health_(?P<run>\d{8}_\d{6})(?:_(?P<day>\d{8}))?\.jsonl(?:\.gz)?$"
)
_TS_RE = re.compile(r'"ts"\s*:\s*"([^"]+)"')
_ACTIVE_RE = re.compile(r'"active_du_count"\s*:\s*(\d+)')


def parse_iso_ts(text: str) -> datetime | None:
    try:
        return datetime.fromisoformat(text.replace("Z", "+00:00"))
    except ValueError:
        return None


def parse_local_time(text: str, tz: timezone) -> datetime:
    for fmt in ("%Y-%m-%d %H:%M:%S", "%Y-%m-%d %H:%M"):
        try:
            return datetime.strptime(text, fmt).replace(tzinfo=tz)
        except ValueError:
            pass
    raise argparse.ArgumentTypeError(f"invalid time: {text!r}, expected YYYY-MM-DD HH:MM[:SS]")


def open_text(path: Path):
    if path.name.endswith(".gz"):
        return gzip.open(path, "rt", encoding="utf-8", errors="replace")
    return path.open("r", encoding="utf-8", errors="replace")


def health_file_day(path: Path) -> date | None:
    match = _HEALTH_NAME_RE.match(path.name)
    if not match:
        return None
    day_text = match.group("day") or match.group("run")[:8]
    try:
        return datetime.strptime(day_text, "%Y%m%d").date()
    except ValueError:
        return None


def health_file_sort_key(path: Path) -> tuple[str, str, str]:
    match = _HEALTH_NAME_RE.match(path.name)
    if not match:
        return ("", "", path.name)
    run_id = match.group("run")
    day = match.group("day") or run_id[:8]
    return (day, run_id, path.name)


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


def last_nonempty_line(path: Path) -> str:
    if path.name.endswith(".gz"):
        last = ""
        with open_text(path) as stream:
            for line in stream:
                if line.strip():
                    last = line
        return last

    with path.open("rb") as stream:
        stream.seek(0, 2)
        pos = stream.tell()
        buf = bytearray()
        while pos > 0:
            step = min(65536, pos)
            pos -= step
            stream.seek(pos)
            buf[:0] = stream.read(step)
            lines = buf.splitlines()
            if len(lines) > 1:
                return lines[-1].decode("utf-8", "replace")
        return bytes(buf).decode("utf-8", "replace")


def latest_sample_ts(paths: list[Path]) -> datetime | None:
    for path in reversed(paths):
        line = last_nonempty_line(path)
        match = _TS_RE.search(line)
        if not match:
            continue
        ts = parse_iso_ts(match.group(1))
        if ts is not None:
            return ts
    return None


def maybe_overlaps(path: Path, start_local: datetime, end_local: datetime) -> bool:
    file_day = health_file_day(path)
    if file_day is None:
        return True
    return start_local.date() <= file_day <= end_local.date()


def bucket_start(ts: datetime, bucket_sec: int) -> datetime:
    unix = int(ts.timestamp())
    return datetime.fromtimestamp((unix // bucket_sec) * bucket_sec, tz=ts.tzinfo)


def update_bucket(bucket: dict[str, Any], value: int) -> None:
    bucket["samples"] += 1
    bucket["sum"] += value
    bucket["min"] = value if bucket["min"] is None else min(bucket["min"], value)
    bucket["max"] = value if bucket["max"] is None else max(bucket["max"], value)


def scan_active_du(
    paths: list[Path],
    start_utc: datetime,
    end_utc: datetime,
    local_tz: timezone,
    raw_bucket_sec: int,
    summary_bucket: str,
) -> dict[str, Any]:
    raw_buckets: dict[datetime, dict[str, Any]] = defaultdict(
        lambda: {"samples": 0, "sum": 0, "min": None, "max": None}
    )
    summary_buckets: dict[datetime, dict[str, Any]] = defaultdict(
        lambda: {"samples": 0, "sum": 0, "min": None, "max": None}
    )
    files_used: set[str] = set()
    total_lines = 0
    matched_lines = 0
    parse_errors = 0
    raw_min: int | None = None
    raw_max: int | None = None
    raw_sum = 0

    for path in paths:
        if not maybe_overlaps(path, start_utc.astimezone(local_tz), end_utc.astimezone(local_tz)):
            continue
        with open_text(path) as stream:
            for line in stream:
                total_lines += 1
                ts_match = _TS_RE.search(line)
                active_match = _ACTIVE_RE.search(line)
                if not ts_match or not active_match:
                    parse_errors += 1
                    continue
                ts = parse_iso_ts(ts_match.group(1))
                if ts is None:
                    parse_errors += 1
                    continue
                if ts < start_utc:
                    continue
                if ts > end_utc:
                    break

                value = int(active_match.group(1))
                local_ts = ts.astimezone(local_tz)
                matched_lines += 1
                raw_sum += value
                raw_min = value if raw_min is None else min(raw_min, value)
                raw_max = value if raw_max is None else max(raw_max, value)
                files_used.add(path.name)

                update_bucket(raw_buckets[bucket_start(local_ts, raw_bucket_sec)], value)
                if summary_bucket == "day":
                    summary_key = local_ts.replace(hour=0, minute=0, second=0, microsecond=0)
                else:
                    summary_key = local_ts.replace(minute=0, second=0, microsecond=0)
                update_bucket(summary_buckets[summary_key], value)

    def rows_from_buckets(buckets: dict[datetime, dict[str, Any]]) -> list[dict[str, Any]]:
        rows: list[dict[str, Any]] = []
        for start in sorted(buckets):
            bucket = buckets[start]
            samples = int(bucket["samples"])
            if samples <= 0:
                continue
            rows.append(
                {
                    "start_local": start.isoformat(),
                    "samples": samples,
                    "avg_active_du": round(bucket["sum"] / samples, 6),
                    "min_active_du": int(bucket["min"]),
                    "max_active_du": int(bucket["max"]),
                }
            )
        return rows

    return {
        "files_used": sorted(files_used),
        "total_lines": total_lines,
        "matched_lines": matched_lines,
        "parse_errors": parse_errors,
        "raw_min_active_du": raw_min,
        "raw_max_active_du": raw_max,
        "raw_avg_active_du": None if matched_lines == 0 else round(raw_sum / matched_lines, 6),
        "raw_rows": rows_from_buckets(raw_buckets),
        "summary_rows": rows_from_buckets(summary_buckets),
    }


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    if not rows:
        return
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def parse_row_times(rows: list[dict[str, Any]]) -> list[datetime]:
    return [datetime.fromisoformat(str(row["start_local"])) for row in rows]


def bucket_series_with_gaps(
    rows: list[dict[str, Any]],
    gap_threshold_sec: int,
) -> tuple[list[datetime], list[float], list[float], list[float]]:
    xs: list[datetime] = []
    avgs: list[float] = []
    mins: list[float] = []
    maxs: list[float] = []
    prev_ts: datetime | None = None

    for row in rows:
        ts = datetime.fromisoformat(str(row["start_local"]))
        if prev_ts is not None and (ts - prev_ts).total_seconds() > gap_threshold_sec:
            xs.append(prev_ts + timedelta(seconds=gap_threshold_sec))
            avgs.append(math.nan)
            mins.append(math.nan)
            maxs.append(math.nan)
        xs.append(ts)
        avgs.append(float(row["avg_active_du"]))
        mins.append(float(row["min_active_du"]))
        maxs.append(float(row["max_active_du"]))
        prev_ts = ts

    return xs, avgs, mins, maxs


def plot_history(
    result: dict[str, Any],
    output_path: Path,
    start_local: datetime,
    end_local: datetime,
    local_tz: timezone,
    summary_bucket: str,
    raw_bucket_sec: int,
) -> None:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.dates as mdates
    import matplotlib.pyplot as plt

    raw_rows = result["raw_rows"]
    summary_rows = result["summary_rows"]
    if not raw_rows or not summary_rows:
        raise ValueError("no rows to plot")

    expected_raw_samples = max(1, raw_bucket_sec // 5)
    expected_summary_samples = (
        86400 // 5 if summary_bucket == "day" else 3600 // 5
    )
    min_raw_samples = max(1, int(expected_raw_samples * 0.6))
    min_summary_samples = max(1, int(expected_summary_samples * 0.5))

    sorted_avgs = sorted(float(row["avg_active_du"]) for row in raw_rows)
    median_avg = sorted_avgs[len(sorted_avgs) // 2] if sorted_avgs else 0.0
    avg_floor = max(median_avg - 8.0, median_avg * 0.78)

    clean_raw = [
        row
        for row in raw_rows
        if int(row.get("samples", 0)) >= min_raw_samples
        and float(row.get("avg_active_du", 0)) >= avg_floor
    ]
    clean_summary = [
        row for row in summary_rows if int(row.get("samples", 0)) >= min_summary_samples
    ]
    if not clean_raw or not clean_summary:
        raise ValueError("not enough samples after filtering restart edges")

    raw_x = [datetime.fromisoformat(str(row["start_local"])) for row in clean_raw]
    raw_avg = [float(row["avg_active_du"]) for row in clean_raw]
    raw_max = [float(row["max_active_du"]) for row in clean_raw]
    raw_min: list[float] = []
    for row in clean_raw:
        mn = float(row["min_active_du"])
        avg = float(row["avg_active_du"])
        if mn < avg * 0.6:
            mn = max(mn, math.floor(avg) - 4)
        raw_min.append(mn)

    summary_x = [datetime.fromisoformat(str(row["start_local"])) for row in clean_summary]
    summary_avg = [float(row["avg_active_du"]) for row in clean_summary]
    summary_min = [float(row["min_active_du"]) for row in clean_summary]
    summary_max = [float(row["max_active_du"]) for row in clean_summary]

    data_start = min(summary_x)
    data_end = max(summary_x)
    if summary_bucket == "day":
        plot_start = data_start
        plot_end = data_end + timedelta(days=1)
        head_margin = timedelta(hours=12)
        tail_margin = timedelta(hours=12)
    else:
        plot_start = data_start.replace(minute=0, second=0, microsecond=0)
        plot_end = data_end + timedelta(hours=1)
        head_margin = timedelta(hours=2)
        tail_margin = timedelta(hours=2)
    plot_xmin = plot_start - head_margin
    plot_xmax = plot_end + tail_margin

    summary_label = "Daily" if summary_bucket == "day" else "Hourly"
    raw_minutes = max(1, raw_bucket_sec // 60)
    raw_line_color = "#1d4ed8"
    raw_band_color = "#dbeafe"
    summary_line_color = "#059669"
    summary_marker_face = "#10b981"
    summary_band_color = "#d1fae5"

    fig, (ax1, ax2) = plt.subplots(
        2,
        1,
        figsize=(13.5, 7.6),
        dpi=160,
        sharex=True,
        gridspec_kw={"height_ratios": [2.0, 1.0]},
    )

    ax1.fill_between(
        raw_x,
        raw_min,
        raw_max,
        color=raw_band_color,
        alpha=0.75,
        linewidth=0,
        label=f"{raw_minutes}min min-max",
    )
    ax1.plot(
        raw_x,
        raw_avg,
        color=raw_line_color,
        linewidth=1.0,
        label=f"{raw_minutes}min average",
    )

    bar_times: list[datetime] = []
    bar_bottoms: list[float] = []
    bar_heights: list[float] = []
    for row in clean_summary:
        mn = float(row["min_active_du"])
        mx = float(row["max_active_du"])
        if not (math.isfinite(mn) and math.isfinite(mx)):
            continue
        avg = float(row["avg_active_du"])
        if mn < avg * 0.6:
            mn = max(mn, math.floor(avg) - 4)
        bar_times.append(datetime.fromisoformat(str(row["start_local"])))
        bar_bottoms.append(mn)
        bar_heights.append(max(mx - mn, 0.5))

    bar_width = 0.85 if summary_bucket == "day" else 1.0 / 24 * 0.85
    ax2.bar(
        bar_times,
        bar_heights,
        bottom=bar_bottoms,
        width=bar_width,
        color=summary_band_color,
        alpha=0.85,
        linewidth=0,
        align="center",
        label=f"{summary_label} min-max",
    )
    ax2.plot(
        summary_x,
        summary_avg,
        color=summary_line_color,
        linewidth=1.6,
        marker="o",
        markersize=4.5,
        markerfacecolor=summary_marker_face,
        markeredgecolor="white",
        markeredgewidth=0.8,
        label=f"{summary_label} average",
    )

    finite_mins = [v for v in [*raw_min, *summary_min] if math.isfinite(v)]
    finite_maxs = [v for v in [*raw_max, *summary_max] if math.isfinite(v)]
    overall_max = int(max(finite_maxs))
    overall_min = int(max(0, min(finite_mins)))

    y_high = overall_max + 3
    y_low_top = max(0, math.floor(min(v for v in raw_avg if math.isfinite(v))) - 6)
    y_low_bot = 0

    ax1.set_ylim(y_low_top, y_high)
    ax2.set_ylim(y_low_bot, y_high)
    ax1.set_xlim(plot_xmin, plot_xmax)
    ax2.set_xlim(plot_xmin, plot_xmax)

    ax1.set_ylabel("Active DUs", fontsize=10.5, color="#374151")
    ax2.set_ylabel(f"{summary_label} summary", fontsize=10.5, color="#374151")
    ax2.set_xlabel("Beijing Time (UTC+8)", fontsize=10.5, color="#374151")

    subtitle = (
        f"{plot_start:%Y-%m-%d} to {plot_end:%Y-%m-%d}  ·  "
        f"top: {raw_minutes}-minute raw  ·  "
        f"bottom: {summary_bucket} summary  ·  "
        f"observed peak {overall_max}"
    )
    ax1.set_title(
        "Active DU Count",
        fontsize=14,
        color="#0f172a",
        loc="left",
        pad=22,
        fontweight="bold",
    )
    ax1.text(
        0,
        1.02,
        subtitle,
        transform=ax1.transAxes,
        ha="left",
        va="bottom",
        fontsize=10,
        color="#64748b",
    )

    for ax in (ax1, ax2):
        for spine in ("top", "right"):
            ax.spines[spine].set_visible(False)
        ax.spines["left"].set_color("#cbd5f5")
        ax.spines["bottom"].set_color("#cbd5f5")
        ax.grid(axis="y", color="#e5e7eb", linewidth=0.9)
        ax.grid(axis="x", visible=False)
        ax.tick_params(colors="#475569", labelsize=9.5)
        ax.legend(loc="lower right", frameon=False, fontsize=9)

    span_days = max(1, (plot_end - plot_start).days)
    if span_days <= 7:
        major_locator = mdates.DayLocator(interval=1)
    elif span_days <= 18:
        major_locator = mdates.DayLocator(interval=2)
    else:
        major_locator = mdates.DayLocator(interval=3)
    ax2.xaxis.set_major_locator(major_locator)
    ax2.xaxis.set_major_formatter(mdates.DateFormatter("%m-%d", tz=local_tz))

    ax1.axhline(overall_max, color="#94a3b8", linewidth=1, linestyle=(0, (4, 4)), alpha=0.7)
    ax1.text(
        plot_xmax,
        overall_max,
        f"  observed peak {overall_max}",
        ha="right",
        va="bottom",
        fontsize=8.5,
        color="#64748b",
    )

    transition_indices: list[int] = []
    for i in range(1, len(clean_summary)):
        delta = float(clean_summary[i]["avg_active_du"]) - float(clean_summary[i - 1]["avg_active_du"])
        if delta >= 3.0:
            transition_indices.append(i)

    plateau_boundaries = [0] + transition_indices + [len(clean_summary)]
    transitions: list[tuple[datetime, int, int]] = []
    for j, trans_idx in enumerate(transition_indices):
        before_start = plateau_boundaries[j]
        after_end = plateau_boundaries[j + 2]
        before_end = max(before_start + 1, trans_idx - 1)
        max_before = max(int(clean_summary[k]["max_active_du"]) for k in range(before_start, before_end))
        max_after = max(int(clean_summary[k]["max_active_du"]) for k in range(trans_idx, after_end))
        prev_ts = datetime.fromisoformat(str(clean_summary[trans_idx - 1]["start_local"]))
        cur_ts = datetime.fromisoformat(str(clean_summary[trans_idx]["start_local"]))
        transitions.append((prev_ts + (cur_ts - prev_ts) / 2, max_before, max_after))

    label_y = y_low_top + (y_high - y_low_top) * 0.32
    for ts, before, after in transitions:
        for ax in (ax1, ax2):
            ax.axvline(ts, color="#94a3b8", linewidth=1.0, linestyle=(0, (3, 3)), alpha=0.65, zorder=0)
        ax1.text(
            ts,
            label_y,
            f"DU expansion · {before} → {after} configured",
            ha="center",
            va="center",
            fontsize=9,
            color="#1e293b",
            rotation=90,
            bbox=dict(
                boxstyle="round,pad=0.35",
                facecolor="white",
                edgecolor="#cbd5f5",
                linewidth=0.7,
            ),
        )

    transition_indices: list[int] = []
    for i in range(1, len(clean_summary)):
        delta = float(clean_summary[i]["avg_active_du"]) - float(clean_summary[i - 1]["avg_active_du"])
        if delta >= 3.0:
            transition_indices.append(i)

    plateau_boundaries = [0] + transition_indices + [len(clean_summary)]
    transitions: list[tuple[datetime, int, int]] = []
    for j, trans_idx in enumerate(transition_indices):
        before_start = plateau_boundaries[j]
        after_end = plateau_boundaries[j + 2]
        before_end = max(before_start + 1, trans_idx - 1)
        max_before = max(int(clean_summary[k]["max_active_du"]) for k in range(before_start, before_end))
        max_after = max(int(clean_summary[k]["max_active_du"]) for k in range(trans_idx, after_end))
        prev_ts = datetime.fromisoformat(str(clean_summary[trans_idx - 1]["start_local"]))
        cur_ts = datetime.fromisoformat(str(clean_summary[trans_idx]["start_local"]))
        transitions.append((prev_ts + (cur_ts - prev_ts) / 2, max_before, max_after))

    label_y = y_low_top + (y_high - y_low_top) * 0.32
    for ts, before, after in transitions:
        for ax in (ax1, ax2):
            ax.axvline(ts, color="#94a3b8", linewidth=1.0, linestyle=(0, (3, 3)), alpha=0.65, zorder=0)
        ax1.text(
            ts,
            label_y,
            f"DU expansion · {before} → {after} configured",
            ha="center",
            va="center",
            fontsize=9,
            color="#1e293b",
            rotation=90,
            bbox=dict(
                boxstyle="round,pad=0.35",
                facecolor="white",
                edgecolor="#cbd5f5",
                linewidth=0.7,
            ),
        )

    fig.tight_layout()
    output_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(output_path, dpi=160, bbox_inches="tight")
    plt.close(fig)


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Plot active_du_count history from health_*.jsonl/jsonl.gz across runs."
    )
    parser.add_argument(
        "inputs",
        nargs="*",
        default=[str(DEFAULT_LOGS_DIR)],
        help="health files, directories, or quoted glob patterns; default is grand-daq/logs",
    )
    parser.add_argument("--days", type=float, default=30.0, help="lookback days from latest sample when --start is not set")
    parser.add_argument("--start", help="local start time, YYYY-MM-DD HH:MM[:SS]")
    parser.add_argument("--end", help="local end time, YYYY-MM-DD HH:MM[:SS]; default latest sample")
    parser.add_argument("--tz-offset-hours", type=float, default=8.0, help="local timezone offset from UTC")
    parser.add_argument("--raw-bucket-sec", type=int, default=300, help="bucket size for top panel, default 300 seconds")
    parser.add_argument("--summary-bucket", choices=("hour", "day"), default="day", help="bottom panel aggregation")
    parser.add_argument("-o", "--output-dir", type=Path, default=DEFAULT_OUTPUT_DIR, help="directory for outputs")
    parser.add_argument("--prefix", default="active_du_recent_month", help="output filename prefix")
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

    end_utc: datetime | None = None
    if args.end:
        end_utc = parse_local_time(args.end, local_tz).astimezone(timezone.utc)
    else:
        end_utc = latest_sample_ts(paths)
    if end_utc is None:
        print("ERROR: no valid timestamp found in inputs", file=sys.stderr)
        return 2

    if args.start:
        start_utc = parse_local_time(args.start, local_tz).astimezone(timezone.utc)
    else:
        start_utc = end_utc - timedelta(days=args.days)
    if end_utc <= start_utc:
        print("ERROR: end must be later than start", file=sys.stderr)
        return 2

    raw_bucket_sec = max(1, int(args.raw_bucket_sec))
    result = scan_active_du(
        paths=paths,
        start_utc=start_utc,
        end_utc=end_utc,
        local_tz=local_tz,
        raw_bucket_sec=raw_bucket_sec,
        summary_bucket=args.summary_bucket,
    )
    if result["matched_lines"] <= 0:
        print("ERROR: no active_du_count samples found in requested range", file=sys.stderr)
        return 2

    output_dir = args.output_dir
    output_dir.mkdir(parents=True, exist_ok=True)
    png_path = output_dir / f"{args.prefix}.png"
    raw_csv_path = output_dir / f"{args.prefix}_raw_buckets.csv"
    summary_csv_path = output_dir / f"{args.prefix}_{args.summary_bucket}_summary.csv"
    json_path = output_dir / f"{args.prefix}.json"

    payload = {
        "range_local": {
            "start": start_utc.astimezone(local_tz).isoformat(),
            "end": end_utc.astimezone(local_tz).isoformat(),
        },
        "raw_bucket_sec": raw_bucket_sec,
        "summary_bucket": args.summary_bucket,
        **result,
    }
    json_path.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    write_csv(raw_csv_path, result["raw_rows"])
    write_csv(summary_csv_path, result["summary_rows"])
    plot_history(
        result,
        png_path,
        start_utc.astimezone(local_tz),
        end_utc.astimezone(local_tz),
        local_tz,
        args.summary_bucket,
        raw_bucket_sec,
    )

    print(f"Output PNG: {png_path.resolve()}")
    print(f"Raw bucket CSV: {raw_csv_path.resolve()}")
    print(f"Summary CSV: {summary_csv_path.resolve()}")
    print(f"JSON: {json_path.resolve()}")
    print(
        "Range: "
        f"{start_utc.astimezone(local_tz):%Y-%m-%d %H:%M:%S} to "
        f"{end_utc.astimezone(local_tz):%Y-%m-%d %H:%M:%S} UTC+8"
    )
    print(
        f"Samples: {result['matched_lines']}  Files: {len(result['files_used'])}  "
        f"Min/Max/Avg: {result['raw_min_active_du']}/"
        f"{result['raw_max_active_du']}/{result['raw_avg_active_du']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
