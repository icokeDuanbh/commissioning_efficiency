#!/usr/bin/env python3
"""
Evaluate exposure variation across all 200 possible phase shifts for 10s downsampling.

Usage:
    python evaluate_phase_exposure.py [--results-dir /path/to/results]
"""

import argparse
import numpy as np
import matplotlib.pyplot as plt
from scipy.interpolate import interp1d
from pathlib import Path

plt.style.use("~/Dropbox/Config/presentation_dark.mplstyle")

ROOT = Path(__file__).resolve().parent


def main(results_dir: Path, slt_tag: str = "SLT"):
    npz_file = results_dir / f"efficiency_vs_time_SLT_{slt_tag}.npz"
    imgs_dir = results_dir / "imgs"

    if not npz_file.exists():
        print(f"Error: {npz_file} not found. Run plot_efficiency_vs_time.py first.")
        return

    data = np.load(npz_file)
    rel_time = data["rel_time_sec"]
    dt_orig = 0.05
    step_10s = int(round(10.0 / dt_orig))

    eff_keys = [k for k in data.keys() if k.startswith("eff_")]
    label_map = {
        "eff_gt100_PeV_(all)": ">100 PeV (all)",
        "eff_lt3_EeV": "<3 EeV",
        "eff_ge3_EeV": "≥3 EeV",
    }

    fig, ax = plt.subplots(figsize=(10, 5))
    fig_hist, ax_hist = plt.subplots(figsize=(10, 5))

    print("=" * 105)
    print(f"{'Energy Band':<20} | {'E_20Hz [s]':<12} | {'E_10s Mean [s]':<15} | {'Std across 200 phases [s]':<25} | {'Rel Std (Uncertainty)':<22}")
    print("=" * 105)

    colors = ["#1f77b4", "#ff7f0e", "#2ca02c"]

    for idx, key in enumerate(eff_keys):
        eff_20hz = data[key]
        if len(eff_20hz) == 0:
            continue

        label = label_map.get(key, key)
        color = colors[idx % len(colors)]

        e_true = np.sum(eff_20hz) * dt_orig

        e_10s_phases = []
        phase_offsets_sec = np.arange(step_10s) * dt_orig

        for offset in range(step_10s):
            idx_sub = np.arange(offset, len(rel_time), step_10s)
            t_sub = rel_time[idx_sub]
            eff_sub = eff_20hz[idx_sub]

            interp_func = interp1d(t_sub, eff_sub, kind="linear", fill_value="extrapolate")
            eff_interp = interp_func(rel_time)

            e_10s = np.sum(eff_interp) * dt_orig
            e_10s_phases.append(e_10s)

        e_10s_phases = np.array(e_10s_phases)
        mean_10s = np.mean(e_10s_phases)
        std_10s = np.std(e_10s_phases)
        rel_std = (std_10s / e_true) * 100.0

        print(f"{label:<20} | {e_true:<12.3f} | {mean_10s:<15.3f} | {std_10s:<25.4f} | {rel_std:<22.4f}%")

        ax.plot(phase_offsets_sec, e_10s_phases, label=f"{label} (Std = {rel_std:.3f}%)", color=color)
        ax.axhline(e_true, color=color, linestyle=":", alpha=0.7)

        ax_hist.hist((e_10s_phases - e_true) / e_true * 100.0, bins=20, alpha=0.5, label=label, color=color)

    print("=" * 105)

    ax.set_xlabel("Phase Offset $t_0$ [s]")
    ax.set_ylabel("Total Integrated Exposure $E_{\\mathrm{total}}$ [s]")
    ax.set_title("Total 10s Integrated Exposure vs. Sampling Phase Offset $t_0$")
    ax.grid(True, which="both")
    ax.legend()
    fig.tight_layout()

    imgs_dir.mkdir(parents=True, exist_ok=True)
    out_phase_img = imgs_dir / "exposure_vs_phase_offset.png"
    fig.savefig(out_phase_img, dpi=300)
    print(f"\nSaved Phase Offset Plot: {out_phase_img}")

    ax_hist.set_xlabel("Relative Exposure Difference from True 20Hz [%]")
    ax_hist.set_ylabel("Count (out of 200 Phase Offsets)")
    ax_hist.set_title("Distribution of Relative Exposure Errors across 200 Phase Offsets")
    ax_hist.grid(True, which="both")
    ax_hist.legend()
    fig_hist.tight_layout()

    out_hist_img = imgs_dir / "exposure_phase_distribution.png"
    fig_hist.savefig(out_hist_img, dpi=300)
    print(f"Saved Phase Distribution Histogram: {out_hist_img}")

    plt.show()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Evaluate exposure variation across phase offsets.")
    parser.add_argument("--results-dir", type=Path, default=ROOT / "results",
                        help="path of the efficiency over time (default: ./results)")
    parser.add_argument("--slt-tag", type=str, default="SLT",
                        help="Suffix tag describing SLT condition (e.g. SLT, FLT0, etc.)")
    args = parser.parse_args()
    main(args.results_dir.resolve(), slt_tag=args.slt_tag)
