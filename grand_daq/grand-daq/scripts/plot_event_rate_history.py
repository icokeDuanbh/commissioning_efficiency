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
_COUNTER_RE = re.compile(r'"dispatch_count_trigger_level"\s*:\s*(\d+)')


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


def scan_event_rate(
    paths: list[Path],
    start_utc: datetime,
    end_utc: datetime,
    local_tz: timezone,
    bucket_sec: int,
    max_gap_sec: int,
    max_rate_per_sec: float,
) -> dict[str, Any]:
    """Stream-scan health files, derive event rate per bucket from counter deltas."""
    buckets: dict[datetime, dict[str, float]] = defaultdict(
        lambda: {"sum_count": 0.0, "sum_time": 0.0, "pairs": 0}
    )
    files_used: set[str] = set()
    total_lines = 0
    matched_lines = 0
    skipped_pairs = 0
    first_local: datetime | None = None
    last_local: datetime | None = None

    prev_ts: datetime | None = None
    prev_counter: int | None = None

    start_local = start_utc.astimezone(local_tz)
    end_local = end_utc.astimezone(local_tz)

    for path in paths:
        day = health_file_day(path)
        if day is not None:
            if day < (start_local.date() - timedelta(days=1)):
                continue
            if day > (end_local.date() + timedelta(days=1)):
                continue

        with open_text(path) as stream:
            for line in stream:
                total_lines += 1
                m_ts = _TS_RE.search(line)
                m_counter = _COUNTER_RE.search(line)
                if not m_ts or not m_counter:
                    continue
                ts = parse_iso_ts(m_ts.group(1))
                if ts is None:
                    continue
                if ts < start_utc:
                    continue
                if ts > end_utc:
                    break

                counter = int(m_counter.group(1))
                local_ts = ts.astimezone(local_tz)
                matched_lines += 1
                files_used.add(path.name)
                if first_local is None or local_ts < first_local:
                    first_local = local_ts
                if last_local is None or local_ts > last_local:
                    last_local = local_ts

                if prev_ts is not None and prev_counter is not None:
                    delta_t = (ts - prev_ts).total_seconds()
                    delta_c = counter - prev_counter
                    if 0 <= delta_c and 0 < delta_t <= max_gap_sec:
                        if delta_c / delta_t > max_rate_per_sec:
                            # Glitch during DAQ restart/shutdown causes the counter
                            # to oscillate between its real value and 0; the rebound
                            # pair would otherwise inject a fake burst into a bucket.
                            skipped_pairs += 1
                        else:
                            midpoint = prev_ts + (ts - prev_ts) / 2
                            mid_local = midpoint.astimezone(local_tz)
                            unix = int(mid_local.timestamp())
                            bucket_start = datetime.fromtimestamp(
                                (unix // bucket_sec) * bucket_sec, tz=local_tz
                            )
                            bucket = buckets[bucket_start]
                            bucket["sum_count"] += delta_c
                            bucket["sum_time"] += delta_t
                            bucket["pairs"] += 1

                prev_ts = ts
                prev_counter = counter

    rows: list[dict[str, Any]] = []
    for bucket_start in sorted(buckets):
        bucket = buckets[bucket_start]
        if bucket["sum_time"] <= 0:
            continue
        rate = bucket["sum_count"] / bucket["sum_time"]
        rows.append(
            {
                "start_local": bucket_start.isoformat(),
                "pairs": int(bucket["pairs"]),
                "delta_events": int(bucket["sum_count"]),
                "delta_time_sec": round(bucket["sum_time"], 3),
                "avg_rate": round(rate, 4),
            }
        )

    total_events = sum(row["delta_events"] for row in rows)
    total_time = sum(row["delta_time_sec"] for row in rows)
    overall_rate = total_events / total_time if total_time > 0 else 0.0

    return {
        "bucket_sec": bucket_sec,
        "max_rate_per_sec": max_rate_per_sec,
        "files_used": sorted(files_used),
        "total_lines": total_lines,
        "matched_lines": matched_lines,
        "skipped_pairs": skipped_pairs,
        "first_sample_local": first_local.isoformat() if first_local else None,
        "last_sample_local": last_local.isoformat() if last_local else None,
        "total_events": int(total_events),
        "total_time_sec": round(total_time, 3),
        "overall_rate": round(overall_rate, 4),
        "rows": rows,
    }


def write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    if not rows:
        return
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def time_window_geometric_mean(
    times: list[datetime],
    values: list[float],
    window: timedelta,
) -> list[float | None]:
    """Centered rolling geometric mean (log-space average) of positive `values`.

    Geometric mean is appropriate for rates spanning several orders of magnitude:
    on a log Y axis it produces a visually smooth trend that is not dominated by
    occasional bursts.
    """
    half = window / 2
    results: list[float | None] = []
    for i, ts in enumerate(times):
        low = ts - half
        high = ts + half
        logs: list[float] = []
        for j in range(i, -1, -1):
            if times[j] < low:
                break
            if values[j] > 0:
                logs.append(math.log10(values[j]))
        for j in range(i + 1, len(times)):
            if times[j] > high:
                break
            if values[j] > 0:
                logs.append(math.log10(values[j]))
        if logs:
            results.append(10 ** (sum(logs) / len(logs)))
        else:
            results.append(None)
    return results


def plot_event_rate(
    result: dict[str, Any],
    output_path: Path,
    local_tz: timezone,
    requested_start_local: datetime,
    requested_end_local: datetime,
    smooth_hours: int = 12,
) -> None:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.dates as mdates
    import matplotlib.pyplot as plt

    rows = result["rows"]
    if not rows:
        raise ValueError("no event rate rows to plot")

    bucket_sec = int(result.get("bucket_sec") or 3600)
    times = [datetime.fromisoformat(str(row["start_local"])) for row in rows]
    rates = [float(row["avg_rate"]) for row in rows]
    positive_rates = [value for value in rates if value > 0]
    if not positive_rates:
        raise ValueError("no positive event rate samples to plot")

    bucket_hours = max(1, bucket_sec // 3600)
    bucket_label = f"{bucket_hours}h" if bucket_sec >= 3600 else f"{bucket_sec // 60}m"

    smooth_window = timedelta(hours=max(1, smooth_hours))
    smoothed = time_window_geometric_mean(times, rates, smooth_window)
    smoothed_times = [ts for ts, value in zip(times, smoothed) if value is not None]
    smoothed_values = [value for value in smoothed if value is not None]

    plot_start = requested_start_local
    plot_end = requested_end_local
    rate_max = max(positive_rates)
    rate_floor = min(positive_rates)
    y_low = max(0.01, 10 ** math.floor(math.log10(max(rate_floor, 1e-3))))
    y_high = 10 ** math.ceil(math.log10(rate_max * 1.4))

    plot_rates = [max(value, y_low) for value in rates]

    fig, ax = plt.subplots(figsize=(13.5, 6.0), dpi=160)
    ax.vlines(
        times,
        ymin=y_low,
        ymax=plot_rates,
        colors="#bfdbfe",
        linewidth=0.5,
        alpha=0.45,
    )
    if smoothed_values:
        ax.plot(
            smoothed_times,
            [max(value, y_low) for value in smoothed_values],
            color="#1d4ed8",
            linewidth=2.6,
            solid_capstyle="round",
            label=f"{smooth_hours}-hour moving average",
        )

    ax.set_yscale("log")
    ax.set_ylim(y_low, y_high)
    ax.set_xlim(plot_start, plot_end)

    ax.set_xlabel("Beijing Time", fontsize=12)
    ax.set_ylabel("Average Event Rate (events/1s)", fontsize=12)
    ax.set_title(
        f"Average Event Rate in {bucket_label} Windows\n"
        f"{plot_start:%Y-%m-%d-%H:%M:%S} to {plot_end:%Y-%m-%d-%H:%M:%S}",
        fontsize=12,
    )

    ax.grid(which="major", color="lightgray", linewidth=0.6)
    ax.grid(which="minor", axis="y", color="lightgray", linewidth=0.4, linestyle=":")
    if smoothed_values:
        ax.legend(loc="lower left", frameon=False, fontsize=10)

    span_days = max(1, (plot_end - plot_start).days)
    if span_days <= 6:
        ax.xaxis.set_major_locator(mdates.DayLocator(interval=1))
        fmt = "%m-%d %H"
    elif span_days <= 14:
        ax.xaxis.set_major_locator(mdates.DayLocator(interval=2))
        fmt = "%m-%d"
    elif span_days <= 25:
        ax.xaxis.set_major_locator(mdates.DayLocator(interval=2))
        fmt = "%m-%d"
    else:
        ax.xaxis.set_major_locator(mdates.DayLocator(interval=3))
        fmt = "%m-%d"
    ax.xaxis.set_major_formatter(mdates.DateFormatter(fmt, tz=local_tz))
    fig.autofmt_xdate(rotation=30, ha="right")

    fig.tight_layout()
    output_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(output_path, dpi=160, bbox_inches="tight")
    plt.close(fig)


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Plot event rate (hourly average) from health_*.jsonl/jsonl.gz across runs."
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
    parser.add_argument("--bucket-sec", type=int, default=3600, help="aggregation bucket size in seconds, default 3600 (hourly)")
    parser.add_argument("--max-gap-sec", type=int, default=60, help="drop sample pairs separated by more than this many seconds (default 60)")
    parser.add_argument("--max-rate-per-sec", type=float, default=2000.0, help="cap sane per-pair rate (events/s); higher values are treated as counter glitches (default 2000)")
    parser.add_argument("--smooth-hours", type=int, default=24, help="window in hours for the rolling-average trend line (default 24)")
    parser.add_argument("-o", "--output-dir", type=Path, default=DEFAULT_OUTPUT_DIR, help="directory for outputs")
    parser.add_argument("--prefix", default="event_rate_recent_30d", help="output filename prefix")
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

    bucket_sec = max(60, int(args.bucket_sec))
    max_gap_sec = max(1, int(args.max_gap_sec))

    result = scan_event_rate(
        paths=paths,
        start_utc=start_utc,
        end_utc=end_utc,
        local_tz=local_tz,
        bucket_sec=bucket_sec,
        max_gap_sec=max_gap_sec,
        max_rate_per_sec=float(args.max_rate_per_sec),
    )
    if not result["rows"]:
        print("ERROR: no event rate samples found in requested range", file=sys.stderr)
        return 2

    output_dir = args.output_dir
    output_dir.mkdir(parents=True, exist_ok=True)
    png_path = output_dir / f"{args.prefix}.png"
    csv_path = output_dir / f"{args.prefix}_buckets.csv"
    json_path = output_dir / f"{args.prefix}.json"

    payload = {
        "range_local": {
            "start": start_utc.astimezone(local_tz).isoformat(),
            "end": end_utc.astimezone(local_tz).isoformat(),
        },
        **result,
    }
    json_path.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    write_csv(csv_path, result["rows"])
    plot_event_rate(
        result,
        png_path,
        local_tz,
        start_utc.astimezone(local_tz),
        end_utc.astimezone(local_tz),
        smooth_hours=int(args.smooth_hours),
    )

    print(f"Output PNG: {png_path.resolve()}")
    print(f"Bucket CSV: {csv_path.resolve()}")
    print(f"JSON: {json_path.resolve()}")
    print(
        "Range: "
        f"{start_utc.astimezone(local_tz):%Y-%m-%d %H:%M:%S} to "
        f"{end_utc.astimezone(local_tz):%Y-%m-%d %H:%M:%S} UTC+8"
    )
    print(
        f"Buckets: {len(result['rows'])}  Total events: {result['total_events']:,}  "
        f"Overall rate: {result['overall_rate']:.3f} events/s  "
        f"Skipped glitch pairs: {result['skipped_pairs']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
