#!/usr/bin/env python3
"""
Plot the FLT0 and FLT1 trigger statistics over time for a single simulation event.

Output: results/imgs/shower_trigger_scan_sim_<global_sim_id>.png
"""

import numpy as np
import pandas as pd
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
from pathlib import Path

ROOT = Path(__file__).resolve().parent
RESULTS_DIR = ROOT / "results"
MANIFEST_CSV = RESULTS_DIR / "sim_manifest.csv"


def find_npz_file(i_dir: int, global_sim_id: int) -> Path:
    """Check both results/sim_batch_XXXX and results/sim_batch/sim_batch_XXXX paths."""
    p1 = RESULTS_DIR / f"sim_batch_{i_dir:04d}" / f"sim_{global_sim_id:05d}.npz"
    p2 = RESULTS_DIR / "sim_batch" / f"sim_batch_{i_dir:04d}" / f"sim_{global_sim_id:05d}.npz"
    if p1.exists():
        return p1
    if p2.exists():
        return p2
    return None


def plot_shower_trigger(global_sim_id: int = 1218):
    # Load manifest info if available
    df = pd.read_csv(MANIFEST_CSV)
    match = df[df["global_sim_id"] == global_sim_id]

    if len(match) == 0:
        print(f"Shower {global_sim_id} not found in manifest.")
        return

    row = match.iloc[0]
    i_dir = int(row["i_dir"])
    energy_primary = float(row["energy_primary"])
    log10_E = np.log10(energy_primary)
    primary_type = int(row["primary_type"])
    p_name = "Proton" if primary_type == 14 else "Iron"

    npz_path = find_npz_file(i_dir, global_sim_id)
    if npz_path is None or not npz_path.exists():
        print(f"Could not find npz file for global_sim_id={global_sim_id}")
        return

    data = np.load(npz_path)
    rel_time = data["relative_time_sec"]
    n_flt0 = data["n_flt0_passed"]
    n_flt1 = data["n_flt1_passed"]
    slt_triggered = data["triggered"].astype(bool)

    print(f"Loaded shower {global_sim_id} from {npz_path.name}")
    print(f"  Energy: {energy_primary:.2e} eV (log10E = {log10_E:.2f}), Primary: {p_name}")
    print(f"  FLT0 max: {n_flt0.max()}, mean: {n_flt0.mean():.2f}")
    print(f"  FLT1 max: {n_flt1.max()}, mean: {n_flt1.mean():.2f}")
    print(f"  SLT triggers count: {slt_triggered.sum()}")

    # Figure creation
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(11, 6), sharex=True)

    # Top plot: FLT0 vs FLT1 counts over time
    ax1.plot(rel_time, n_flt0, color="#1f77b4", linewidth=1.0, alpha=0.8, label="FLT0 passed DUs")
    ax1.plot(rel_time, n_flt1, color="#ff7f0e", linewidth=1.2, alpha=0.9, label="FLT1 passed DUs")
    ax1.axhline(5, color="red", linestyle="--", linewidth=1.0, label="FLT1 Preselection Threshold (≥ 5 DUs)")

    ax1.set_ylabel("Number of Triggered DUs", fontsize=11)
    ax1.set_title(
        f"Trigger Performance Scan across Noise Background\n"
        f"Shower ID: {global_sim_id:05d} | {p_name} | Energy: 10^{log10_E:.2f} eV ({energy_primary/1e18:.2f} EeV)",
        fontsize=12,
    )
    ax1.grid(True, linestyle="--", alpha=0.3)
    ax1.legend(loc="upper right", fontsize=10, framealpha=0.9)

    # Bottom plot: Distribution / Histogram of triggered DU counts
    max_dus = max(n_flt0.max(), n_flt1.max()) + 1
    bins = np.arange(-0.5, max_dus + 0.5, 1)

    ax2.hist(n_flt0, bins=bins, color="#1f77b4", alpha=0.6, label="FLT0 count distribution", density=False)
    ax2.hist(n_flt1, bins=bins, color="#ff7f0e", alpha=0.8, label="FLT1 count distribution", density=False)
    ax2.axvline(4.5, color="red", linestyle="--", linewidth=1.0, label="FLT1 ≥ 5 threshold")

    ax2.set_xlabel("Number of Triggered DUs in Event", fontsize=11)
    ax2.set_ylabel("MD Trace Pair Count", fontsize=11)
    ax2.set_yscale("log")
    ax2.grid(True, linestyle="--", alpha=0.3)
    ax2.legend(loc="upper right", fontsize=10, framealpha=0.9)

    fig.tight_layout()
    out_img = RESULTS_DIR / "imgs" / f"shower_trigger_scan_sim_{global_sim_id:05d}.png"
    out_img.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_img, dpi=150, bbox_inches="tight")
    print(f"Saved plot to: {out_img}")
    plt.show()


if __name__ == "__main__":
    plot_shower_trigger(1218)
