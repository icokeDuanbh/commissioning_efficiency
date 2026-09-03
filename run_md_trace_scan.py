#!/usr/bin/env python3
"""
Script to scan MD background traces for a fixed simulation event.

Reads entry indices and relative timestamps from a specified text file,
iterates through consecutive pairs (i_md_prev, i_md_next), passes them to
process_event() from run_full_pipeline.py, and outputs the trigger results
along with relative timestamps.
"""

import sys
import os
from pathlib import Path
import numpy as np
import logging

ROOT = Path(__file__).resolve().parent
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from run_full_pipeline import process_event, DEFAULT_SIM_FILE, DEFAULT_DISTANCES_FILE
from SLT import run_SLT


# Setup logging
logging.basicConfig(level=logging.INFO, format='%(asctime)s [%(levelname)s] %(message)s')
logging.getLogger('grand.dataio.data_tree').setLevel(logging.CRITICAL)

import json

DEFAULT_CONFIG_FILE = ROOT / "test_data/pipeline_config.json"


def run_md_trace_scan(
    fname_sim: str,
    i_sim: int = 0,
    out_file: str = None,
    config: dict = None,
    entry_txt_file: str = None,
):
    """
    Run pipeline across consecutive MD trace pairs for a given simulation event index.
    """
    if config is None:
        if DEFAULT_CONFIG_FILE.exists():
            with open(DEFAULT_CONFIG_FILE, "r") as f:
                config = json.load(f)
        else:
            raise ValueError(f"No config dict provided and default config not found at {DEFAULT_CONFIG_FILE}")

    if entry_txt_file is None:
        input_sources = config.get("input_sources", {})
        entry_txt_file = input_sources.get("md_entry_file", "test_data/md_entries_global_1055.txt")

    entry_txt_path = Path(entry_txt_file)
    if not entry_txt_path.is_absolute():
        entry_txt_path = ROOT / entry_txt_path

    if not entry_txt_path.exists():
        raise FileNotFoundError(f"Entry text file not found: {entry_txt_path}")

    # Load 3-column entry list: file_path (str), entry_index (int), relative_time_sec (float)
    file_paths = []
    entries = []
    rel_times = []

    with open(entry_txt_path, "r") as f:
        header = f.readline()  # Skip header
        for line in f:
            line = line.strip()
            if not line:
                continue
            parts = line.split()
            if len(parts) >= 3:
                file_paths.append(parts[0])
                entries.append(int(parts[1]))
                rel_times.append(float(parts[2]))

    n_entries = len(entries)
    if n_entries < 2:
        raise ValueError(f"Need at least 2 entries in {entry_txt_path}, found {n_entries}")

    logging.info(f"Loaded {n_entries} global MD entries from {entry_txt_path.name}.")
    logging.info(f"Simulation File: {fname_sim}")
    logging.info(f"Evaluating simulation event i_sim = {i_sim}")

    # Extract configuration sections
    filter_params = config.get("filter_params", {})
    flt0_params = config.get("flt0_params", {})
    flt1_params = config.get("flt1_params", {})
    slt_p = config.get("slt_params", {})

    if_filter = filter_params.get("if_filter", True)
    flt1_score_min = flt1_params.get("flt1_score_min", 0.5)
    min_flt1_dus = flt1_params.get("min_flt1_dus", 1)
    use_python_flt0 = flt0_params.get("use_python_flt0", True)

    antenna_dist_setting = slt_p.get("antenna_distances_file", str(DEFAULT_DISTANCES_FILE))
    antenna_dist_path = Path(antenna_dist_setting)
    if not antenna_dist_path.is_absolute():
        antenna_dist_path = ROOT / antenna_dist_setting

    # Initialize SLT pipeline dynamically from config
    slt_pipeline = run_SLT.SLTPipeline(
        trigger_type=slt_p.get("trigger_type", 2),
        nhit_threshold=slt_p.get("nhit_threshold", 5),
        time_window_ns=slt_p.get("time_window_ns", 15000),
        causal_window_enabled=slt_p.get("causal_window_enabled", False),
        antenna_distances_file=str(antenna_dist_path),
        template_match_filter_enable=slt_p.get("template_match_filter_enable", True),
        template_match_coefficient=slt_p.get("template_match_coefficient", 0.7),
        duplicate_filter_enable=slt_p.get("duplicate_filter_enable", False),
    )

    results = []

    # Iterate through consecutive pairs
    for idx in range(n_entries - 1):
        fname_md_prev = file_paths[idx]
        fname_md_next = file_paths[idx + 1]
        i_md_prev = entries[idx]
        i_md_next = entries[idx + 1]
        rel_time = rel_times[idx]

        logging.info(f"[{idx + 1}/{n_entries - 1}] Running pair ({Path(fname_md_prev).name}:{i_md_prev}, {Path(fname_md_next).name}:{i_md_next}) at t_rel={rel_time:.6f}s")

        res = process_event(
            fname_sim=str(fname_sim),
            fname_md_prev=str(fname_md_prev),
            i_sim=i_sim,
            i_md_prev=i_md_prev,
            i_md_next=i_md_next,
            slt_pipeline=slt_pipeline,
            fname_md_next=str(fname_md_next),
            flt0_params=flt0_params,
            flt1_score_min=flt1_score_min,
            min_flt1_dus=min_flt1_dus,
            use_python_flt0=use_python_flt0,
            if_filter=if_filter,
        )

        trig = res["triggered"]
        n_flt0 = res["n_flt0_passed"]
        n_flt1 = res["n_flt1_passed"]

        logging.info(f"  -> Triggered: {trig} (FLT0: {n_flt0}, FLT1: {n_flt1})")

        results.append((rel_time, int(trig), n_flt0, n_flt1))

    results_arr = np.array(results)

    if out_file:
        out_path = Path(out_file)
        out_path.parent.mkdir(parents=True, exist_ok=True)
        if out_path.suffix == ".npz":
            np.savez_compressed(
                out_path,
                relative_time_sec=results_arr[:, 0],
                triggered=results_arr[:, 1].astype(bool),
                n_flt0_passed=results_arr[:, 2].astype(np.int16),
                n_flt1_passed=results_arr[:, 3].astype(np.int16),
            )
        else:
            np.savetxt(
                out_path,
                results_arr,
                fmt=["%.9f", "%d", "%d", "%d"],
                header="relative_time_sec triggered n_flt0_passed n_flt1_passed",
                comments="",
            )
        logging.info(f"Saved results to {out_path}")

    return results_arr


if __name__ == "__main__":
    sim_file = DEFAULT_SIM_FILE
    out_file = ROOT / "test_data/md_scan_results_sim0.txt"

    run_md_trace_scan(
        fname_sim=str(sim_file),
        i_sim=0,
        out_file=str(out_file),
    )
