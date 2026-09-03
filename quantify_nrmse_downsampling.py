#!/usr/bin/env python3
"""
Quantify efficiency time-variation using Normalized Root-Mean-Square Error (NRMSE)
from downsampling.

Usage:
    python quantify_nrmse_downsampling.py [--results-dir /path/to/results]
"""

import argparse
import numpy as np
import pandas as pd
import matplotlib.pyplot as plt
from scipy.interpolate import interp1d
from pathlib import Path

plt.style.use("~/Dropbox/Config/presentation_dark.mplstyle")

ROOT = Path(__file__).resolve().parent

DT_CANDIDATES = [1.0, 2.0, 5.0, 10.0, 15.0, 20.0, 30.0, 60.0]


def main(results_dir: Path, slt_tag: str = "SLT"):
    npz_file = results_dir / f"efficiency_vs_time_SLT_{slt_tag}.npz"
    imgs_dir = results_dir / "imgs"

    if not npz_file.exists():
        print(f"Error: {npz_file} not found. Run plot_efficiency_vs_time.py first.")
        return

    data = np.load(npz_file)
    rel_time = data["rel_time_sec"]

    eff_keys = [k for k in data.keys() if k.startswith("eff_")]
    label_map = {
        "eff_gt100_PeV_(all)": ">100 PeV (all)",
        "eff_lt3_EeV": "<3 EeV",
        "eff_ge3_EeV": "≥3 EeV",
    }

    fig, ax = plt.subplots(figsize=(10, 5))
    fig_comp, ax_comp = plt.subplots(figsize=(12, 5))

    print(f"{'Energy Band':<20} | {'dt_down [s]':<12} | {'RMSE':<10} | {'NRMSE (vs std)':<15} | {'Rel Err (vs mean)':<18}")
    print("-" * 82)

    colors = ["#1f77b4", "#ff7f0e", "#2ca02c"]

    for idx, key in enumerate(eff_keys):
        eff_orig = data[key]
        if len(eff_orig) == 0:
            continue

        label = label_map.get(key, key)
        std_orig = np.std(eff_orig)
        mean_orig = np.mean(eff_orig)

        nrmse_list = []
        rel_err_list = []
        rmse_list = []

        for dt_down in DT_CANDIDATES:
            step = int(round(dt_down / 0.05))
            if step < 1:
                step = 1

            idx_sub = np.arange(0, len(rel_time), step)
            t_sub = rel_time[idx_sub]
            eff_sub = eff_orig[idx_sub]

            interp_func = interp1d(t_sub, eff_sub, kind="linear", fill_value="extrapolate")
            eff_interp = interp_func(rel_time)

            rmse = np.sqrt(np.mean((eff_orig - eff_interp) ** 2))
            nrmse = rmse / std_orig if std_orig > 0 else 0.0
            rel_err = rmse / mean_orig if mean_orig > 0 else 0.0

            nrmse_list.append(nrmse)
            rel_err_list.append(rel_err)
            rmse_list.append(rmse)

            if dt_down in [5.0, 10.0, 30.0]:
                print(f"{label:<20} | {dt_down:<12.1f} | {rmse:<10.5f} | {nrmse:<15.4f} | {rel_err:<18.4%}")

        ax.plot(DT_CANDIDATES, nrmse_list, marker="o", label=label, color=colors[idx % len(colors)])

        step_10s = int(round(10.0 / 0.05))
        idx_10s = np.arange(0, len(rel_time), step_10s)
        t_10s = rel_time[idx_10s]
        eff_10s = eff_orig[idx_10s]
        interp_10s = interp1d(t_10s, eff_10s, kind="linear", fill_value="extrapolate")(rel_time)

        ax_comp.plot(rel_time, eff_orig, alpha=0.5, label=f"{label} (Original 20Hz)", color=colors[idx % len(colors)])
        ax_comp.plot(rel_time, interp_10s, "--", label=f"{label} (Reconstructed 10s step)", color=colors[idx % len(colors)])

    ax.axvline(10.0, color="gray", linestyle=":", label="10s Target Sampling")
    ax.set_xlabel("Downsampling Interval $\\Delta t$ [s]")
    ax.set_ylabel("NRMSE (RMSE / $\\sigma_{\\epsilon}$)")
    ax.set_title("Quantification of Interpolation Error vs. Sampling Interval")
    ax.grid(True, which="both")
    ax.legend()
    fig.tight_layout()

    imgs_dir.mkdir(parents=True, exist_ok=True)
    out_nrmse_img = imgs_dir / "nrmse_vs_sampling_interval.png"
    fig.savefig(out_nrmse_img, dpi=300)
    print(f"\nSaved NRMSE plot: {out_nrmse_img}")

    ax_comp.set_xlabel("Relative Background Time [s]")
    ax_comp.set_ylabel("SLT Trigger Efficiency")
    ax_comp.set_title("Original 20Hz Efficiency vs. 10s Linearly Interpolated Reconstruction")
    ax_comp.grid(True, which="both")
    ax_comp.legend()
    fig_comp.tight_layout()

    out_comp_img = imgs_dir / "efficiency_reconstruction_10s.png"
    fig_comp.savefig(out_comp_img, dpi=300)
    print(f"Saved Reconstruction plot: {out_comp_img}")

    plt.show()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Quantify NRMSE vs downsampling interval.")
    parser.add_argument("--results-dir", type=Path, default=ROOT / "results",
                        help="path of the efficiency over time (default: ./results)")
    parser.add_argument("--slt-tag", type=str, default="SLT",
                        help="Suffix tag describing SLT condition (e.g. SLT, FLT0, etc.)")
    args = parser.parse_args()
    main(args.results_dir.resolve(), slt_tag=args.slt_tag)
