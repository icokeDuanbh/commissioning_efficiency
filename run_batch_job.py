#!/usr/bin/env python3
"""
Batch execution worker script for Slurm array jobs.

Given a Slurm array task ID (0..2999) or explicit --job_id, reads the assigned 5 rows
from sim_manifest.csv, looks up adc_file_path and local_i_sim for each shower,
runs run_md_trace_scan, and saves the compressed .npz result into the 150 subfolders:
results/sim_batch_{i_dir:04d}/sim_{global_sim_id:05d}.npz
"""

import sys
import argparse
from pathlib import Path
import csv
import logging

ROOT = Path(__file__).resolve().parent
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from run_md_trace_scan import run_md_trace_scan
from add_sim_MD.lookup_table import load_lookup_table

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")


import json


def run_batch_job(
    job_id: int,
    output_dir: str,
    manifest_csv: str = None,
    md_lookup_table_file: str = None,
    t_start: float = None,
    t_end: float = None,
    t_step: float = None,
    showers_per_job: int = 5,
):
    out_base = Path(output_dir)

    # Read pipeline_config.json from output_dir
    config_path = out_base / "pipeline_config.json"
    if not config_path.exists():
        raise FileNotFoundError(
            f"Missing configuration file: '{config_path}'. "
            f"Please place 'pipeline_config.json' inside '{output_dir}/' before running batch jobs."
        )

    with open(config_path, "r") as f:
        config = json.load(f)

    input_sources = config.get("input_sources", {})
    if manifest_csv is None:
        manifest_csv = input_sources.get("sim_manifest_csv", "test_data/sim_manifest.csv")

    manifest_path = Path(manifest_csv)
    if not manifest_path.is_absolute():
        manifest_path = ROOT / manifest_path

    if not manifest_path.exists():
        raise FileNotFoundError(f"Manifest CSV not found: {manifest_path}")

    # Preload MD lookup table once for all showers in this batch job
    if md_lookup_table_file is None:
        md_lookup_table_file = input_sources.get("md_lookup_table")
    if md_lookup_table_file is None:
        raise ValueError("MD lookup table must be specified via --md_lookup_table_file or config['input_sources']['md_lookup_table'].")
    lookup_path = Path(md_lookup_table_file)
    if not lookup_path.is_absolute():
        lookup_path = ROOT / lookup_path
    md_lookup_table = load_lookup_table(lookup_path)
    logging.info(f"Preloaded MD lookup table from {lookup_path} ({len(md_lookup_table.available_feb_ids)} FEBs).")

    # Read manifest rows into a list of dicts
    rows = []
    with open(manifest_path, "r", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            rows.append(row)

    total_showers = len(rows)
    if total_showers == 0:
        raise ValueError(f"Manifest file {manifest_path} is empty.")

    start_idx = job_id * showers_per_job
    end_idx = min(start_idx + showers_per_job, total_showers)

    if start_idx >= total_showers:
        logging.info(f"Job ID {job_id}: start_idx {start_idx} exceeds total showers {total_showers}. Nothing to process.")
        return

    logging.info(f"--- Slurm Job {job_id} Processing Showers [{start_idx} .. {end_idx - 1}] (Total Manifest: {total_showers}) ---")

    for i in range(start_idx, end_idx):
        shower = rows[i]
        global_sim_id = int(shower["global_sim_id"])
        i_dir = int(shower["i_dir"])
        local_i_sim = int(shower["local_i_sim"])
        sim_file_path = shower["adc_file_path"]

        # Output folder per batch folder (150 subfolders): results/sim_batch_{i_dir:04d}/sim_{global_sim_id:05d}.npz
        shower_out_dir = out_base / f"sim_batch_{i_dir:04d}"
        shower_out_file = shower_out_dir / f"sim_{global_sim_id:05d}.npz"

        if shower_out_file.exists():
            logging.info(f"Shower {global_sim_id} already exists ({shower_out_file.name}). Skipping.")
            continue

        logging.info(f"Processing Global Shower {global_sim_id} (i_dir={i_dir}, local_i_sim={local_i_sim}) -> {shower_out_file.name}")

        run_md_trace_scan(
            fname_sim=sim_file_path,
            i_sim=local_i_sim,
            out_file=str(shower_out_file),
            config=config,
            md_lookup_table_file=md_lookup_table,
            t_start=t_start,
            t_end=t_end,
            t_step=t_step,
        )

    logging.info(f"--- Slurm Job {job_id} Finished Successfully ---")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Run batch MD trace scan for Slurm array task.")
    parser.add_argument(
        "--job_id",
        type=int,
        required=True,
        help="Slurm array task ID (0..2999).",
    )
    parser.add_argument(
        "--output_dir",
        type=str,
        default=str(ROOT / "results"),
        help="Root output directory for scan results.",
    )
    parser.add_argument(
        "--manifest_csv",
        type=str,
        default=None,
        help="Path to sim_manifest.csv (defaults to config input_sources).",
    )
    parser.add_argument(
        "--md_lookup_table_file",
        type=str,
        default=None,
        help="Path to MD lookup table file (defaults to config input_sources.md_lookup_table).",
    )
    parser.add_argument(
        "--t_start",
        type=float,
        default=None,
        help="Scan start Unix timestamp in seconds (defaults to config scan_params.t_start).",
    )
    parser.add_argument(
        "--t_end",
        type=float,
        default=None,
        help="Scan end Unix timestamp in seconds (defaults to config scan_params.t_end).",
    )
    parser.add_argument(
        "--t_step",
        type=float,
        default=None,
        help="Scan step size in seconds (defaults to config scan_params.t_step).",
    )
    parser.add_argument(
        "--showers_per_job",
        type=int,
        default=5,
        help="Number of showers per job (default: 5).",
    )

    args = parser.parse_args()
    run_batch_job(
        job_id=args.job_id,
        output_dir=args.output_dir,
        manifest_csv=args.manifest_csv,
        md_lookup_table_file=args.md_lookup_table_file,
        t_start=args.t_start,
        t_end=args.t_end,
        t_step=args.t_step,
        showers_per_job=args.showers_per_job,
    )
