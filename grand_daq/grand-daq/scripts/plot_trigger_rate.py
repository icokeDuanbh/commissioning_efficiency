#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any, Iterable


REPO_DIR = Path(__file__).resolve().parents[1]
DEFAULT_INPUT = REPO_DIR / "logs" / "health_20260430_083305_20260430.jsonl"
DEFAULT_OUTPUT = REPO_DIR / "logs" / "trigger_rate_20260430_1200_1400.png"


def parse_iso_ts(ts: Any) -> datetime | None:
    if not isinstance(ts, str) or not ts:
        return None
    try:
        return datetime.fromisoformat(ts.replace("Z", "+00:00"))
    except ValueError:
        return None


def parse_local_time(text: str, tz: timezone) -> datetime:
    for fmt in ("%Y-%m-%d %H:%M:%S", "%Y-%m-%d %H:%M"):
        try:
            return datetime.strptime(text, fmt).replace(tzinfo=tz)
        except ValueError:
            pass
    raise argparse.ArgumentTypeError(f"invalid time: {text!r}, expected YYYY-MM-DD HH:MM[:SS]")


def iter_trigger_counter_samples(
    path: Path,
    start_utc: datetime,
    end_utc: datetime,
) -> Iterable[tuple[datetime, int]]:
    with path.open("r", encoding="utf-8", errors="replace") as f:
        for line in f:
            try:
                entry = json.loads(line)
            except json.JSONDecodeError:
                continue

            ts = parse_iso_ts(entry.get("ts"))
            if ts is None or ts < start_utc or ts > end_utc:
                continue

            health = entry.get("health")
            dotrigger = health.get("dotrigger") if isinstance(health, dict) else None
            counter = dotrigger.get("dispatch_count_trigger_level") if isinstance(dotrigger, dict) else None
            if counter is None:
                continue

            try:
                yield ts, int(counter)
            except (TypeError, ValueError):
                continue


def build_trigger_rate_points(
    samples: Iterable[tuple[datetime, int]]
) -> list[tuple[datetime, float]]:
    points: list[tuple[datetime, float]] = []
    prev: tuple[datetime, int] | None = None
    for ts, counter in samples:
        if prev is not None:
            prev_ts, prev_counter = prev
            elapsed_sec = (ts - prev_ts).total_seconds()
            delta = max(0, counter - prev_counter)
            if elapsed_sec > 0:
                points.append((ts, delta / elapsed_sec))
        prev = (ts, counter)
    return points


def plot_trigger_rate(
    points: list[tuple[datetime, float]],
    output: Path,
    local_tz: timezone,
    start_local: datetime,
    end_local: datetime,
) -> None:
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.dates as mdates
    import matplotlib.pyplot as plt
    from matplotlib import font_manager

    output.parent.mkdir(parents=True, exist_ok=True)

    local_times = [ts.astimezone(local_tz) for ts, _ in points]
    rates = [rate for _, rate in points]

    cjk_fonts = [
        "Noto Sans CJK SC",
        "WenQuanYi Zen Hei",
        "SimHei",
    ]
    available_fonts = {font.name for font in font_manager.fontManager.ttflist}
    has_cjk_font = any(font in available_fonts for font in cjk_fonts)
    plt.rcParams["font.sans-serif"] = [*cjk_fonts, "DejaVu Sans"]
    plt.rcParams["axes.unicode_minus"] = False

    fig, ax = plt.subplots(figsize=(9, 4.8), dpi=140)
    ax.plot(local_times, rates, color="#5470c6", linewidth=1.4, label="trigger_rate (events/s)")
    ax.set_title("触发率" if has_cjk_font else "Trigger Rate", loc="left", fontsize=12)
    ax.set_ylabel("Hz", rotation=0, labelpad=16)
    ax.set_xlim(start_local, end_local)
    ax.grid(axis="y", color="#dfe6f3", linewidth=0.8)
    ax.grid(axis="x", visible=False)
    ax.legend(loc="upper right", frameon=False)

    ax.xaxis.set_major_locator(mdates.MinuteLocator(interval=20))
    ax.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M", tz=local_tz))
    fig.autofmt_xdate(rotation=0, ha="center")

    for spine in ("top", "right"):
        ax.spines[spine].set_visible(False)
    ax.spines["left"].set_visible(False)
    ax.spines["bottom"].set_color("#d0d7e2")
    ax.tick_params(colors="#6b7280", labelsize=9)

    fig.tight_layout()
    fig.savefig(output)
    plt.close(fig)


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Plot trigger_rate (events/s) from a health_*.jsonl file.",
    )
    parser.add_argument("--input", type=Path, default=DEFAULT_INPUT, help=f"health jsonl path (default: {DEFAULT_INPUT})")
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT, help=f"output png path (default: {DEFAULT_OUTPUT})")
    parser.add_argument("--start", default="2026-04-30 12:00:00", help="local start time, YYYY-MM-DD HH:MM[:SS]")
    parser.add_argument("--end", default="2026-04-30 14:00:00", help="local end time, YYYY-MM-DD HH:MM[:SS]")
    parser.add_argument("--tz-offset-hours", type=float, default=8.0, help="local timezone offset from UTC (default: 8)")
    return parser


def main() -> int:
    args = build_arg_parser().parse_args()
    local_tz = timezone(timedelta(hours=args.tz_offset_hours))
    start_local = parse_local_time(args.start, local_tz)
    end_local = parse_local_time(args.end, local_tz)
    if end_local <= start_local:
        raise SystemExit("--end must be later than --start")

    samples = iter_trigger_counter_samples(
        args.input,
        start_local.astimezone(timezone.utc),
        end_local.astimezone(timezone.utc),
    )
    points = build_trigger_rate_points(samples)
    if not points:
        raise SystemExit("no trigger rate points found in the requested range")

    plot_trigger_rate(points, args.output, local_tz, start_local, end_local)
    print(f"wrote {args.output} ({len(points)} points)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
