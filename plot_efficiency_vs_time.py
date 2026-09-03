#!/usr/bin/env python3
"""
Plot SLT Trigger Efficiency vs. Relative Background Time.

Three energy bands are shown on the same axes:
  - All showers   : E >= 100 PeV  (>100 PeV)
  - Low band      : 100 PeV <= E <  3 EeV
  - High band     : E >= 3 EeV

SLT condition: n_flt0_passed >= 5

Usage:
    python plot_efficiency_vs_time.py [--results-dir /path/to/results]
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

# Energy band definitions (in eV): (label, min_eV, max_eV)
ENERGY_BANDS = [
    (">100 PeV (all)",  1e17,  np.inf),
    ("<3 EeV",          1e17,  3e18),
    ("≥3 EeV",          3e18,  np.inf),
]


def find_npz_file(results_dir: Path, i_dir: int, global_sim_id: int) -> Path:
    """Find the .npz result file, checking both possible directory layouts."""
    p1 = results_dir / f"sim_batch_{i_dir:04d}" / f"sim_{global_sim_id:05d}.npz"
    p2 = results_dir / "sim_batch" / f"sim_batch_{i_dir:04d}" / f"sim_{global_sim_id:05d}.npz"
    if p1.exists():
        return p1
    if p2.exists():
        return p2
    return None


def compute_efficiency(results_dir: Path,
                       df_manifest: pd.DataFrame,
                       primary_type: int,
                       min_energy_eV: float,
                       max_energy_eV: float):
    """
    Load npz files for showers matching (primary_type, energy range) and
    compute per-timestamp SLT efficiency (n_flt0_passed >= 5).
    """
    mask = (
        (df_manifest["primary_type"] == primary_type)
        & (df_manifest["energy_eV"] >= min_energy_eV)
        & (df_manifest["energy_eV"] < max_energy_eV)
    )
    df_sel = df_manifest[mask]

    shower_trig_arrays = []
    rel_time = None
    n_missing = 0

    for _, row in df_sel.iterrows():
        npz_path = find_npz_file(results_dir, int(row["i_dir"]), int(row["global_sim_id"]))
        if npz_path is None:
            n_missing += 1
            continue
        try:
            data = np.load(npz_path)
            trig = data["triggered"].astype(bool)
            if rel_time is None:
                rel_time = data["relative_time_sec"]
            shower_trig_arrays.append(trig)
        except Exception:
            n_missing += 1

    n_showers = len(shower_trig_arrays)
    if n_missing:
        print(f"  [{min_energy_eV:.0e}, {max_energy_eV:.0e}) eV: "
              f"{n_showers} loaded, {n_missing} missing")

    if n_showers == 0:
        return None, None, 0

    trig_matrix = np.stack(shower_trig_arrays, axis=0)  # (n_showers, T)
    efficiency  = trig_matrix.sum(axis=0) / n_showers   # (T,)
    return rel_time, efficiency, n_showers


def main(results_dir: Path, manifest_csv: Path, primary_type: int = 14, slt_tag: str = "SLT", output_path: Path = None):
    imgs_dir = results_dir / "imgs"

    if not manifest_csv.exists():
        print(f"Error: {manifest_csv} not found.")
        return

    # ── Load manifest once ────────────────────────────────────────────────────
    df = pd.read_csv(manifest_csv)
    df["energy_eV"]    = df["energy_primary"].astype(float) * 1e9   # GeV → eV
    df["primary_type"] = df["primary_type"].astype(int)

    p_name = PARTICLE_LABELS.get(primary_type, f"PDG {primary_type}")
    print(f"Primary: {p_name}")

    # ── Compute efficiency for each energy band ───────────────────────────────
    results = {}
    for label, e_lo, e_hi in ENERGY_BANDS:
        print(f"  Band '{label}' ...")
        rel_time, eff, n = compute_efficiency(results_dir, df, primary_type, e_lo, e_hi)
        results[label] = (rel_time, eff, n)

    # ── Save arrays for FFT analysis ──────────────────────────────────────────
    imgs_dir.mkdir(parents=True, exist_ok=True)
    npz_out = results_dir / f"efficiency_vs_time_SLT_{slt_tag}.npz"

    labels   = list(results.keys())
    rel_time = results[labels[0]][0]      # all bands share the same time axis
    save_kwargs = {"rel_time_sec": rel_time}
    for label, (_, eff, _) in results.items():
        key = "eff_" + label.replace(" ", "_").replace("≥", "ge").replace("<", "lt").replace(">", "gt")
        save_kwargs[key] = eff if eff is not None else np.array([])

    np.savez(npz_out, **save_kwargs)
    print(f"Saved arrays: {npz_out}")

    # ── Plot ──────────────────────────────────────────────────────────────────
    fig, ax = plt.subplots(figsize=(12, 5))

    for label, (rel_time_i, eff, n) in results.items():
        if eff is None:
            continue
        mean_eff = eff.mean()
        ax.plot(rel_time_i, eff,
                label=f"{label}  (N={n}, mean={mean_eff:.4f})")

    ax.set_xlabel("Relative Background Time [s]")
    ax.set_ylabel("SLT Trigger Efficiency")
    ax.set_title(f"SLT Trigger Efficiency vs. Background Noise Time  —  {p_name}\n"
                 f"SLT Condition: {slt_tag}")
    ax.grid(True, which="both")
    ax.legend()
    fig.tight_layout()

    if output_path is None:
        output_path = imgs_dir / f"efficiency_vs_time_SLT_{slt_tag}.png"

    fig.savefig(output_path, bbox_inches="tight")
    print(f"Saved plot: {output_path}")
    plt.show()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Plot SLT trigger efficiency vs. time.")
    parser.add_argument("--results-dir", type=Path, default=ROOT / "results",
                        help="path of the npz files of the trigger decision (default: ./results)")
    parser.add_argument("--manifest", type=Path, default=ROOT / "results" / "sim_manifest.csv",
                        help="path to global sim_manifest.csv (default: ./results/sim_manifest.csv)")
    parser.add_argument("--primary-type", type=int, default=14,
                        help="Primary particle type (default: 14 for Proton)")
    parser.add_argument("--slt-tag", type=str, default="SLT",
                        help="Suffix tag describing SLT condition (e.g. SLT, FLT0, etc.)")
    args = parser.parse_args()
    main(args.results_dir.resolve(), args.manifest.resolve(), primary_type=args.primary_type, slt_tag=args.slt_tag)
