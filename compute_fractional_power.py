#!/usr/bin/env python3
"""
Compute the Fractional Spectral Power of efficiency fluctuations in the high-frequency band [0.05, 10] Hz
relative to the full AC spectrum (f > 0 Hz).

Usage:
    python compute_fractional_power.py [--results-dir /path/to/results]
"""

import argparse
import numpy as np
from scipy.fft import rfft, rfftfreq
from pathlib import Path

ROOT = Path(__file__).resolve().parent


def main(results_dir: Path, slt_tag: str = "SLT"):
    npz_file = results_dir / f"efficiency_vs_time_SLT_{slt_tag}.npz"

    if not npz_file.exists():
        print(f"Error: {npz_file} not found. Run plot_efficiency_vs_time.py first.")
        return

    data = np.load(npz_file)
    rel_time = data["rel_time_sec"]

    dt = 0.05
    freq = rfftfreq(len(rel_time), d=dt)

    eff_keys = [k for k in data.keys() if k.startswith("eff_")]
    label_map = {
        "eff_gt100_PeV_(all)": ">100 PeV (all)",
        "eff_lt3_EeV": "<3 EeV",
        "eff_ge3_EeV": "≥3 EeV",
    }

    print("=" * 95)
    print(f"{'Energy Band':<20} | {'P_AC (f > 0)':<12} | {'P [0.05-10Hz] / P_AC':<22} | {'P (f < 0.05Hz) / P_AC':<22}")
    print("=" * 95)

    for key in eff_keys:
        eff = data[key]
        if len(eff) == 0:
            continue

        label = label_map.get(key, key)
        fft_vals = rfft(eff)
        power = np.abs(fft_vals) ** 2

        mask_ac   = (freq > 0.0)   & (freq <= 10.0)
        mask_low  = (freq > 0.0)   & (freq < 0.05)
        mask_high = (freq >= 0.05) & (freq <= 10.0)

        p_ac   = np.sum(power[mask_ac])
        p_low  = np.sum(power[mask_low])
        p_high = np.sum(power[mask_high])

        frac_low  = p_low / p_ac  if p_ac > 0 else 0.0
        frac_high = p_high / p_ac if p_ac > 0 else 0.0

        print(f"{label:<20} | {p_ac:<12.4e} | {frac_high:<22.4%} | {frac_low:<22.4%}")

    print("=" * 95)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Compute fractional power above/below 0.05 Hz.")
    parser.add_argument("--results-dir", type=Path, default=ROOT / "results",
                        help="path of the efficiency over time (default: ./results)")
    parser.add_argument("--slt-tag", type=str, default="SLT",
                        help="Suffix tag describing SLT condition (e.g. SLT, FLT0, etc.)")
    args = parser.parse_args()
    main(args.results_dir.resolve(), slt_tag=args.slt_tag)
