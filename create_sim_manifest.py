#!/usr/bin/env python3
"""
Script to pre-generate the central simulation manifest (sim_manifest.csv).

Scans all 150 simulation directories and files, computes global_sim_id (0..14999),
extracts metadata (energy, zenith, azimuth, core_x, core_y) for each shower,
and outputs a single consolidated sim_manifest.csv file.
"""

import sys
import argparse
from pathlib import Path
import csv
from grand.dataio import DataFile

ROOT = Path(__file__).resolve().parent

# Default base directory format template
DEFAULT_SIM_BASE_DIR = "/sps/grand/DC2_Coreas/Coreas_nonoise"


def create_sim_manifest(
    sim_base_dir: str,
    output_csv: str,
    num_folders: int = 150,
    events_per_file: int = 100,
):
    sim_base_path = Path(sim_base_dir)
    out_csv_path = Path(output_csv)
    out_csv_path.parent.mkdir(parents=True, exist_ok=True)

    fieldnames = [
        "global_sim_id",
        "i_dir",
        "local_i_sim",
        "adc_file_path",
        "shower_file_path",
        "primary_type",
        "energy_primary",
        "energy_em",
        "zenith",
        "azimuth",
        "core_x",
        "core_y",
        "core_z"
    ]

    total_showers = 0

    with open(out_csv_path, "w", newline="") as csvfile:
        writer = csv.DictWriter(csvfile, fieldnames=fieldnames)
        writer.writeheader()

        for i_dir in range(num_folders):
            dir_name = f"sim_Dunhuang_20170331_220000_RUN1_CD_GP300-no-noise_{i_dir:04d}"
            folder_path = sim_base_path / dir_name

            if not folder_path.exists():
                print(f"Warning: Directory not found: {folder_path}. Skipping.")
                continue

            # Dynamically find the single adc*.root file inside this folder
            shower_files = list(folder_path.glob("shower_*.root"))
            adc_files = list(folder_path.glob("adc_*.root"))
            if len(shower_files) == 0:
                print(f"Warning: No shower_*.root file found in: {folder_path}. Skipping.")
                continue

            shower_path = shower_files[0]  # Take the single adc*.root file in this directory
            adc_path = adc_files[0]

            # Read simulation file metadata using grand.dataio.DataFile
            data_file = DataFile(str(shower_path))
            n_events = data_file.tshower.get_number_of_entries()

            for local_i in range(n_events):
                global_sim_id = total_showers

                data_file.tshower.get_entry(local_i)

                # Safely extract metadata fields from GRAND tree objects if present
                primary = getattr(data_file.tshower, "primary_type", 0) if hasattr(data_file.tshower, "primary_type") else 0
                energy = getattr(data_file.tshower, "energy_primary", 0.0) if hasattr(data_file.tshower, "energy_primary") else 0.0
                energy_em = getattr(data_file.tshower, "energy_em", 0.0) if hasattr(data_file.tshower, "energy_em") else 0.0
                zenith = getattr(data_file.tshower, "zenith", 0.0) if hasattr(data_file.tshower, "zenith") else 0.0
                azimuth = getattr(data_file.tshower, "azimuth", 0.0) if hasattr(data_file.tshower, "azimuth") else 0.0
                core = getattr(data_file.tshower, "shower_core_pos", 0.0) if hasattr(data_file.tshower, "shower_core_pos") else (0, 0, 0)
                core_x, core_y, core_z = core

                writer.writerow({
                    "global_sim_id": global_sim_id,
                    "i_dir": i_dir,
                    "local_i_sim": local_i,
                    "adc_file_path": str(adc_path),
                    "shower_file_path": str(shower_path),
                    "primary_type": int(float(primary)),
                    "energy_primary": float(energy),
                    "energy_em": float(energy_em),
                    "zenith": float(zenith),
                    "azimuth": float(azimuth),
                    "core_x": float(core_x),
                    "core_y": float(core_y),
                    "core_z": float(core_z),
                })
                total_showers += 1

            data_file.close()
            print(f"[{i_dir + 1}/{num_folders}] Processed {shower_path.name} ({n_events} events)")

    print(f"\nManifest successfully created with {total_showers} entries at: {out_csv_path}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Pre-generate central simulation manifest sim_manifest.csv")
    parser.add_argument(
        "--sim_base_dir",
        type=str,
        default=DEFAULT_SIM_BASE_DIR,
        help="Base directory containing simulation subfolders.",
    )
    parser.add_argument(
        "--out_csv",
        type=str,
        default=str(ROOT / "test_data/sim_manifest.csv"),
        help="Output CSV path for sim_manifest.csv.",
    )
    parser.add_argument(
        "--num_folders",
        type=int,
        default=150,
        help="Number of simulation folders (default 150).",
    )
    parser.add_argument(
        "--events_per_file",
        type=int,
        default=100,
        help="Events per ROOT file (default 100).",
    )

    args = parser.parse_args()
    create_sim_manifest(
        sim_base_dir=args.sim_base_dir,
        output_csv=args.out_csv,
        num_folders=args.num_folders,
        events_per_file=args.events_per_file,
    )
