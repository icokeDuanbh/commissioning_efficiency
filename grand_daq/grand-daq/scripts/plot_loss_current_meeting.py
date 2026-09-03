#!/usr/bin/env python3
"""Render meeting-style loss chart from precomputed loss_current_run_* artifacts."""

from __future__ import annotations

import argparse
import json
import sys
from datetime import timedelta, timezone
from pathlib import Path

REPO_DIR = Path(__file__).resolve().parents[1]
if str(REPO_DIR / "scripts") not in sys.path:
    sys.path.insert(0, str(REPO_DIR / "scripts"))

from plot_loss_current_run import plot_loss_run_stability


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Plot meeting loss chart from summary JSON")
    parser.add_argument("--summary", required=True, type=Path)
    parser.add_argument("--buckets", required=True, type=Path)
    parser.add_argument("--by-du", required=True, type=Path)
    parser.add_argument("--png", required=True, type=Path)
    parser.add_argument("--svg", type=Path, default=None)
    args = parser.parse_args(argv)

    if not args.summary.is_file():
        raise SystemExit(f"summary not found: {args.summary}")
    if not args.buckets.is_file():
        raise SystemExit(f"buckets not found: {args.buckets}")
    if not args.by_du.is_file():
        raise SystemExit(f"by-du not found: {args.by_du}")

    result = json.loads(args.summary.read_text(encoding="utf-8"))
    local_tz = timezone(timedelta(hours=8))
    args.png.parent.mkdir(parents=True, exist_ok=True)
    plot_loss_run_stability(result, args.png, local_tz)

    if args.svg is not None:
        import matplotlib.image as mpimg
        import matplotlib.pyplot as plt

        img = mpimg.imread(args.png)
        fig, ax = plt.subplots(figsize=(img.shape[1] / 160, img.shape[0] / 160), dpi=160)
        ax.imshow(img)
        ax.axis("off")
        args.svg.parent.mkdir(parents=True, exist_ok=True)
        fig.savefig(args.svg, format="svg", bbox_inches="tight", pad_inches=0)
        plt.close(fig)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
