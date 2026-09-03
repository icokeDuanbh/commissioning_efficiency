#!/usr/bin/env python3
"""
Compute and plot the FFT spectrum of the trigger efficiency vs. time series.

Usage:
    python plot_efficiency_fft.py [--results-dir /path/to/results]
"""

import argparse
import numpy as np
import matplotlib.pyplot as plt
from scipy.fft import rfft, rfftfreq
from pathlib import Path

plt.style.use("~/Dropbox/Config/presentation_dark.mplstyle")

ROOT = Path(__file__).resolve().parent


def main(results_dir: Path, slt_tag: str = "SLT"):
    npz_file = results_dir / f"efficiency_vs_time_SLT_{slt_tag}.npz"
    imgs_dir = results_dir / "imgs"

    if not npz_file.exists():
        print(f"Error: {npz_file} not found. Please run plot_efficiency_vs_time.py first.")
        return

    data = np.load(npz_file)
    rel_time = data["rel_time_sec"]

    # Sampling rate is exactly 20 Hz -> d = 0.05 s
    dt = 0.05
    freq = rfftfreq(len(rel_time), d=dt)

    fig, ax = plt.subplots(figsize=(11, 5))

    eff_keys = [k for k in data.keys() if k.startswith("eff_")]

    label_map = {
        "eff_gt100_PeV_(all)": ">100 PeV (all)",
        "eff_lt3_EeV": "<3 EeV",
        "eff_ge3_EeV": "≥3 EeV",
    }

    for key in eff_keys:
        eff = data[key]
        if len(eff) == 0:
            continue

        fft_val = np.abs(rfft(eff))
        label = label_map.get(key, key)
        ax.plot(freq, fft_val, marker=".", label=label)

    ax.set_xlabel("Frequency [Hz]")
    ax.set_ylabel("PSD [a. u.]")
    ax.set_title(f"FFT Spectrum of SLT Trigger Efficiency Time Series ({slt_tag})")
    ax.set_xlim(0, 1)
    ax.semilogy()
    ax.grid(True, which="both")
    ax.legend()
    fig.tight_layout()

    imgs_dir.mkdir(parents=True, exist_ok=True)
    out_img = imgs_dir / f"fft_efficiency_vs_time_SLT_{slt_tag}.png"
    fig.savefig(out_img, dpi=300)
    print(f"Saved FFT plot to: {out_img}")
    plt.show()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Plot FFT spectrum of efficiency time series.")
    parser.add_argument("--results-dir", type=Path, default=ROOT / "results",
                        help="path of the efficiency over time (default: ./results)")
    parser.add_argument("--slt-tag", type=str, default="SLT",
                        help="Suffix tag describing SLT condition (e.g. SLT, FLT0, etc.)")
    args = parser.parse_args()
    main(args.results_dir.resolve(), slt_tag=args.slt_tag)
