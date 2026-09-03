#!/usr/bin/env python3
"""
Plot SLT Trigger Efficiency vs. Primary Energy.

Accepts an optional --results-dir argument to specify the simulation results directory.

Usage:
    python plot_efficiency_vs_energy.py [--results-dir /path/to/results]
"""

import argparse
import numpy as np
import pandas as pd
import matplotlib.pyplot as plt
from pathlib import Path

plt.style.use("~/Dropbox/Config/presentation_dark.mplstyle")

ROOT = Path(__file__).resolve().parent

PARTICLE_LABELS = {
    14:   "Proton",
    5626: "Iron",
}

# 16 bin edges in logspace from 10^8 to 10^11 GeV (matching energy_primary in manifest)
BIN_EDGES = np.logspace(8, 11, 16)  # in GeV
BIN_CENTERS = np.sqrt(BIN_EDGES[:-1] * BIN_EDGES[1:])  # Geometric centers in GeV


def find_npz_file(results_dir: Path, i_dir: int, global_sim_id: int) -> Path:
    """Check both results_dir/sim_batch_XXXX and results_dir/sim_batch/sim_batch_XXXX paths."""
    p1 = results_dir / f"sim_batch_{i_dir:04d}" / f"sim_{global_sim_id:05d}.npz"
    p2 = results_dir / "sim_batch" / f"sim_batch_{i_dir:04d}" / f"sim_{global_sim_id:05d}.npz"
    if p1.exists():
        return p1
    if p2.exists():
        return p2
    return None


def compute_shower_slt_efficiency(results_dir: Path, i_dir: int, global_sim_id: int):
    """
    Returns (n_slt_passed, total_md_pairs) for a single shower.
    SLT condition: triggered == True (boolean array in npz)
    """
    npz_path = find_npz_file(results_dir, i_dir, global_sim_id)
    if npz_path is None:
        return None, 0

    try:
        data = np.load(npz_path)
        trig = data["triggered"].astype(bool)
        return int(trig.sum()), int(trig.size)
    except Exception:
        return None, 0


def make_efficiency_plot(results_dir: Path, manifest_csv: Path):
    imgs_dir = results_dir / "imgs"

    if not manifest_csv.exists():
        print(f"Error: {manifest_csv} not found.")
        return

    df = pd.read_csv(manifest_csv)
    df["energy_GeV"] = df["energy_primary"].astype(float)  # energy_primary is in GeV
    df["primary_type"] = df["primary_type"].astype(int)

    primary_types = sorted(df["primary_type"].unique())

    records = []
    for ptype in primary_types:
        p_name = PARTICLE_LABELS.get(ptype, f"PDG {ptype}")
        df_p = df[df["primary_type"] == ptype]

        for i in range(len(BIN_EDGES) - 1):
            e_low = BIN_EDGES[i]
            e_high = BIN_EDGES[i + 1]
            e_center = BIN_CENTERS[i]

            bin_mask = (df_p["energy_GeV"] >= e_low) & (df_p["energy_GeV"] < e_high)
            df_bin = df_p[bin_mask]

            if len(df_bin) == 0:
                continue

            total_trig = 0
            total_md_pairs = 0
            shower_effs = []

            for _, row in df_bin.iterrows():
                i_dir = int(row["i_dir"])
                global_sim_id = int(row["global_sim_id"])
                n_trig, n_md = compute_shower_slt_efficiency(results_dir, i_dir, global_sim_id)

                if n_trig is not None and n_md > 0:
                    total_trig += n_trig
                    total_md_pairs += n_md
                    shower_effs.append(n_trig / n_md)

            n_loaded_showers = len(shower_effs)
            if n_loaded_showers == 0 or total_md_pairs == 0:
                overall_eff = np.nan
                eff_err = np.nan
            else:
                overall_eff = total_trig / total_md_pairs
                eff_err = np.sqrt(overall_eff * (1.0 - overall_eff) / total_md_pairs)

            records.append({
                "primary_type": ptype,
                "primary_name": p_name,
                "bin_idx": i,
                "e_low": e_low,
                "e_high": e_high,
                "e_center": e_center,
                "n_showers": n_loaded_showers,
                "total_trig": total_trig,
                "total_md_pairs": total_md_pairs,
                "efficiency": overall_eff,
                "eff_err": eff_err,
            })

    if not records:
        print("No valid data loaded for plotting.")
        return

    res_df = pd.DataFrame(records)
    
    # ── Plotting ──────────────────────────────────────────────────────────────
    fig, ax = plt.subplots(figsize=(9, 5.5))

    offsets = {14: 0.97, 5626: 1.03}

    for ptype in primary_types:
        p_name = PARTICLE_LABELS.get(ptype, f"PDG {ptype}")
        offset = offsets.get(ptype, 1.0)

        sub = res_df[res_df["primary_type"] == ptype].sort_values("e_center")
        valid = sub[sub["efficiency"].notna()]

        x = valid["e_center"] * offset
        y = valid["efficiency"]
        yerr = valid["eff_err"]

        ax.errorbar(
            x,
            y,
            yerr=yerr,
            fmt="o",
            label=f"{p_name}",
        )

        for _, row in valid.iterrows():
            ax.annotate(
                f"N={int(row['n_showers'])}",
                xy=(row["e_center"] * offset, row["efficiency"]),
                xytext=(0, 5),
                textcoords="offset points",
                ha="center",
                va="bottom",
                fontsize=8,
            )

    ax.set_xscale("log")
    ax.set_yscale("log")

    ax.set_xlabel(r"Primary Energy $E_{\rm primary}$ [GeV]")
    ax.set_ylabel("SLT Trigger Efficiency")
    ax.set_title("CR Trigger Efficiency vs. Energy")

    ax.set_xlim(5e7, 2e11)
    ax.set_ylim(1e-5, 1.5)
    ax.grid(True, which="both")
    ax.legend()

    imgs_dir.mkdir(parents=True, exist_ok=True)
    out_img = imgs_dir / "efficiency_vs_energy_slt_logspace.png"
    fig.savefig(out_img, bbox_inches="tight")
    print(f"Saved: {out_img}")
    plt.show()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Plot SLT trigger efficiency vs. primary energy.")
    parser.add_argument("--results-dir", type=Path, default=ROOT / "results",
                        help="path of the npz files of the trigger decision (default: ./results)")
    parser.add_argument("--manifest", type=Path, default=ROOT / "results" / "sim_manifest.csv",
                        help="path to global sim_manifest.csv (default: ./results/sim_manifest.csv)")
    args = parser.parse_args()
    make_efficiency_plot(args.results_dir.resolve(), args.manifest.resolve())
