#!/usr/bin/env python3
"""
Compare the Integrated Efficiency (Exposure) between full 20 Hz resolution and 10 s downsampling.

Usage:
    python compare_exposure.py [--results-dir /path/to/results]
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

    eff_keys = [k for k in data.keys() if k.startswith("eff_")]
    label_map = {
        "eff_gt100_PeV_(all)": ">100 PeV (all)",
        "eff_lt3_EeV": "<3 EeV",
        "eff_ge3_EeV": "≥3 EeV",
    }

    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(11, 8), sharex=True)

    print("=" * 85)
    print(f"{'Energy Band':<20} | {'E_total (20Hz) [s]':<18} | {'E_total (10s) [s]':<18} | {'Rel Diff [%]':<12}")
    print("=" * 85)

    colors = ["#1f77b4", "#ff7f0e", "#2ca02c"]

    for idx, key in enumerate(eff_keys):
        eff_20hz = data[key]
        if len(eff_20hz) == 0:
            continue

        label = label_map.get(key, key)
        color = colors[idx % len(colors)]

        exp_20hz = np.cumsum(eff_20hz) * dt_orig

        step_10s = int(round(10.0 / dt_orig))
        idx_10s = np.arange(0, len(rel_time), step_10s)
        t_10s = rel_time[idx_10s]
        eff_10s_samples = eff_20hz[idx_10s]

        interp_func = interp1d(t_10s, eff_10s_samples, kind="linear", fill_value="extrapolate")
        eff_10s_interp = interp_func(rel_time)

        exp_10s = np.cumsum(eff_10s_interp) * dt_orig

        total_exp_20hz = exp_20hz[-1]
        total_exp_10s  = exp_10s[-1]
        rel_diff_total = abs(total_exp_10s - total_exp_20hz) / total_exp_20hz * 100.0

        print(f"{label:<20} | {total_exp_20hz:<18.3f} | {total_exp_10s:<18.3f} | {rel_diff_total:<12.4f}%")

        rel_err_t = np.zeros_like(exp_20hz)
        valid_mask = exp_20hz > 0
        rel_err_t[valid_mask] = np.abs(exp_10s[valid_mask] - exp_20hz[valid_mask]) / exp_20hz[valid_mask] * 100.0

        ax1.plot(rel_time, exp_20hz, label=f"{label} (20 Hz)", color=color, linewidth=2)
        ax1.plot(rel_time, exp_10s, "--", label=f"{label} (10s sampling)", color=color, alpha=0.8)

        # Plot Relative Error over time [%]
        ax2.plot(rel_time, rel_err_t, label=f"{label} (Final = {rel_err_t[-1]:.3f}%, Min = {np.min(rel_err_t[valid_mask]):.4f}%)", color=color)

        # Find minimum error point (after initial t > 5s transient to avoid t=0 boundary artifacts)
        mask_after_transient = (rel_time > 5.0) & valid_mask
        if np.any(mask_after_transient):
            min_idx = np.argmin(rel_err_t[mask_after_transient])
            t_min = rel_time[mask_after_transient][min_idx]
            err_min = rel_err_t[mask_after_transient][min_idx]

            ax2.plot(t_min, err_min, "v", color=color, markersize=8)
            ax2.annotate(
                f"Min: {err_min:.4f}%\n@ t={t_min:.1f}s",
                xy=(t_min, err_min),
                xytext=(0, -25),
                textcoords="offset points",
                ha="center",
                fontsize=8,
                color=color,
                arrowprops=dict(arrowstyle="->", color=color, lw=0.8),
            )

    print("=" * 85)

    ax1.set_ylabel("Integrated Exposure $\\int \\epsilon(t) dt$ [s]")
    ax1.set_title("Cumulative Exposure Comparison: 20 Hz vs. 10 s Sampling")
    ax1.grid(True, which="both")
    ax1.legend()

    ax2.set_xlabel("Relative Background Time [s]")
    ax2.set_ylabel("Relative Error [%]")
    ax2.set_title("Relative Error on Exposure $|E_{10s}(t) - E_{20Hz}(t)| / E_{20Hz}(t)$")
    ax2.grid(True, which="both")
    ax2.legend()

    fig.tight_layout()
    imgs_dir.mkdir(parents=True, exist_ok=True)
    out_img = imgs_dir / "exposure_comparison_10s.png"
    fig.savefig(out_img, dpi=300)
    print(f"\nSaved Exposure plot to: {out_img}")
    plt.show()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Compare exposure between 20 Hz and 10 s sampling.")
    parser.add_argument("--results-dir", type=Path, default=ROOT / "results",
                        help="path of the efficiency over time (default: ./results)")
    parser.add_argument("--slt-tag", type=str, default="SLT",
                        help="Suffix tag describing SLT condition (e.g. SLT, FLT0, etc.)")
    args = parser.parse_args()
    main(args.results_dir.resolve(), slt_tag=args.slt_tag)
