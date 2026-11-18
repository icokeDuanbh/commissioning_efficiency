#!/usr/bin/env python3
"""
Generate an example FLT0 threshold file for tests and documentation.

FEB IDs come from the DU ID -> FEB ID mapping table (so they are consistent with the
sim-MD addition). Thresholds are close to the current global values with random
fluctuations of about +-10 % per channel (fixed seed, th1 >= th2 enforced).
"""

import argparse
import random
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_MAPPING = ROOT / "add_sim_MD" / "GP65_DUID_FEBID_from_json.txt"
DEFAULT_OUT = Path(__file__).resolve().parent / "threshold_FEBs_example.csv"


def read_feb_ids(mapping_file: Path):
    febs = set()
    with open(mapping_file, "r") as f:
        for line in f:
            parts = line.split()
            if len(parts) >= 2:
                febs.add(int(parts[1]))
    return sorted(febs)


def make_example(mapping_file: Path, out_file: Path, th1=60, th2=45, spread=0.10, seed=12345):
    rng = random.Random(seed)
    febs = read_feb_ids(mapping_file)
    lines = ["feb_id th1_x th2_x th1_y th2_y"]
    for feb in febs:
        vals = []
        for _ in range(2):  # chX, chY
            t1 = int(round(th1 * (1 + rng.uniform(-spread, spread))))
            t2 = int(round(th2 * (1 + rng.uniform(-spread, spread))))
            t2 = min(t2, t1)  # enforce th1 >= th2
            vals += [t1, t2]
        lines.append(f"{feb} " + " ".join(str(v) for v in vals))
    out_file.write_text("\n".join(lines) + "\n")
    return len(febs)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Generate an example FLT0 threshold file.")
    parser.add_argument("--mapping", type=Path, default=DEFAULT_MAPPING)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    args = parser.parse_args()
    n = make_example(args.mapping, args.out)
    print(f"Wrote {n} FEBs to {args.out}")
