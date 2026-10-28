#!/usr/bin/env python3
"""
Script to scan MD background traces across a uniform time grid for a fixed simulation event.

Given an MD lookup table and a uniform timestamp grid [t_start, t_end) with step t_step (in seconds),
iterates through each target timestamp `t_scan`, passes it as `sim_timestamp` to `process_event()`
from `run_full_pipeline.py` (which injects the temporally closest MD trace per FEB), and outputs
the trigger results along with `t_scan`.
"""

import sys
import os
from pathlib import Path
import numpy as np
import logging
import json

ROOT = Path(__file__).resolve().parent
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from run_full_pipeline import process_event, DEFAULT_SIM_FILE, DEFAULT_DISTANCES_FILE
from add_sim_MD import add_sim_MD
from add_sim_MD.lookup_table import MDLookupTable, load_lookup_table, load_du_to_feb_map
from FLT0.thresholds import load_flt0_thresholds
from SLT import run_SLT
from SLT.load_duplicate_history import DuplicateRejectHistory, load_duplicate_history


# Setup logging
logging.basicConfig(level=logging.INFO, format='%(asctime)s [%(levelname)s] %(message)s')
logging.getLogger('grand.dataio.data_tree').setLevel(logging.CRITICAL)

DEFAULT_CONFIG_FILE = ROOT / "test_data/pipeline_config.json"


def run_md_trace_scan(
    fname_sim: str,
    i_sim: int = 0,
    out_file: str = None,
    config: dict = None,
    md_lookup_table_file=None,
    duplicate_reject_log_file=None,
    t_start: float = None,
    t_end: float = None,
    t_step: float = None,
):
    """
    Run pipeline across a uniform time grid [t_start, t_end) with step t_step (s)
    for a given simulation event index.

    Parameters
    ----------
    fname_sim : str
        Path to simulation ROOT file.
    i_sim : int
        Simulation event entry index.
    out_file : str, optional
        Output file path (.npz or .txt).
    config : dict, optional
        Pipeline configuration dictionary (loaded from DEFAULT_CONFIG_FILE if None).
    md_lookup_table_file : str, Path, or MDLookupTable, optional
        Path to MD lookup table file (or preloaded MDLookupTable). Defaults to
        `config["input_sources"]["md_lookup_table"]`.
    duplicate_reject_log_file : str, Path, or DuplicateRejectHistory, optional
        Path to online DAQ DuplicateRejectLog file (or preloaded DuplicateRejectHistory).
        Defaults to `config["input_sources"]["duplicate_reject_log"]`. Used when
        `duplicate_filter_enable` is True to seed the past N events prior to each `t_scan`.
    t_start : float, optional
        Start of the scan in absolute Unix timestamp (seconds). Defaults to
        `config["scan_params"]["t_start"]`.
    t_end : float, optional
        End of the scan in absolute Unix timestamp (seconds). Defaults to
        `config["scan_params"]["t_end"]`.
    t_step : float, optional
        Step size of the scan in seconds. Defaults to
        `config["scan_params"]["t_step"]`.
    """
    if config is None:
        if DEFAULT_CONFIG_FILE.exists():
            with open(DEFAULT_CONFIG_FILE, "r") as f:
                config = json.load(f)
        else:
            raise ValueError(f"No config dict provided and default config not found at {DEFAULT_CONFIG_FILE}")

    input_sources = config.get("input_sources", {})
    scan_params = config.get("scan_params", {})

    if md_lookup_table_file is None:
        md_lookup_table_file = input_sources.get("md_lookup_table")
    if md_lookup_table_file is None:
        raise ValueError("MD lookup table must be provided via md_lookup_table_file or config['input_sources']['md_lookup_table'].")

    if isinstance(md_lookup_table_file, MDLookupTable):
        md_lookup_table = md_lookup_table_file
    else:
        lookup_path = Path(md_lookup_table_file)
        if not lookup_path.is_absolute():
            lookup_path = ROOT / lookup_path
        md_lookup_table = load_lookup_table(lookup_path)
        logging.info(f"Loaded MD lookup table from {lookup_path} ({len(md_lookup_table.available_feb_ids)} FEBs).")

    if t_start is None:
        t_start = scan_params.get("t_start")
    if t_end is None:
        t_end = scan_params.get("t_end")
    if t_step is None:
        t_step = scan_params.get("t_step")

    if t_start is None or t_end is None or t_step is None:
        raise ValueError(
            "t_start, t_end, and t_step (in seconds) are required for the uniform time grid scan "
            "(pass as arguments or set in config['scan_params'])."
        )

    t_start = float(t_start)
    t_end = float(t_end)
    t_step = float(t_step)
    if t_step <= 0:
        raise ValueError(f"t_step must be positive, got {t_step}.")
    if t_end <= t_start:
        raise ValueError(f"t_end ({t_end}) must be greater than t_start ({t_start}).")

    scan_timestamps = np.arange(t_start, t_end, t_step, dtype=np.float64)
    n_steps = len(scan_timestamps)
    if n_steps == 0:
        raise ValueError(f"Empty scan timestamp grid for t_start={t_start}, t_end={t_end}, t_step={t_step}.")

    logging.info(
        f"Uniform time grid: t_start={t_start:.6f}s, t_end={t_end:.6f}s, t_step={t_step:.6f}s ({n_steps} steps)."
    )
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

    # FEB-dependent FLT0 thresholds are mandatory (loaded once for the whole scan)
    thresholds_setting = flt0_params.get("thresholds_file")
    if thresholds_setting is None:
        raise ValueError("flt0_params.thresholds_file is mandatory in the config (FEB-dependent FLT0 thresholds).")
    thresholds_path = Path(thresholds_setting)
    if not thresholds_path.is_absolute():
        thresholds_path = ROOT / thresholds_path
    flt0_thresholds = load_flt0_thresholds(thresholds_path)
    logging.info(f"Loaded FLT0 thresholds for {len(flt0_thresholds)} FEBs from {thresholds_path}")

    # DU ID -> FEB ID mapping (loaded once for the whole scan; defaults to the shipped GP65 mapping)
    fname_DU_to_FEB = input_sources.get("du_to_feb_map")
    if fname_DU_to_FEB is not None and not Path(fname_DU_to_FEB).is_absolute():
        fname_DU_to_FEB = str(ROOT / fname_DU_to_FEB)
    du_to_feb = load_du_to_feb_map(fname_DU_to_FEB if fname_DU_to_FEB is not None else add_sim_MD.DEFAULT_MAPPING_FILE)

    antenna_dist_setting = slt_p.get("antenna_distances_file", str(DEFAULT_DISTANCES_FILE))
    antenna_dist_path = Path(antenna_dist_setting)
    if not antenna_dist_path.is_absolute():
        antenna_dist_path = ROOT / antenna_dist_setting

    duplicate_filter_enable = bool(slt_p.get("duplicate_filter_enable", False))
    duplicate_time_diff_ns = int(slt_p.get("duplicate_time_diff_ns", 100))
    duplicate_min_pair = int(slt_p.get("duplicate_min_pair", 2))
    duplicate_history_size = int(slt_p.get("duplicate_history_size", 10))

    # Preload DuplicateRejectLog history if duplicate_filter_enable is True (or explicitly provided)
    duplicate_history = None
    if duplicate_filter_enable or duplicate_reject_log_file is not None:
        if duplicate_reject_log_file is None:
            duplicate_reject_log_file = input_sources.get("duplicate_reject_log") or slt_p.get("duplicate_reject_log")
        if duplicate_reject_log_file is None:
            raise ValueError(
                "Duplicate reject log must be provided via duplicate_reject_log_file or "
                "config['input_sources']['duplicate_reject_log'] when duplicate_filter_enable is True."
            )
        if isinstance(duplicate_reject_log_file, DuplicateRejectHistory):
            duplicate_history = duplicate_reject_log_file
        else:
            dup_log_path = Path(duplicate_reject_log_file)
            if not dup_log_path.is_absolute():
                dup_log_path = ROOT / dup_log_path
            duplicate_history = load_duplicate_history(dup_log_path)
            logging.info(
                f"Loaded DuplicateRejectLog from {dup_log_path} ({len(duplicate_history)} events, "
                f"range=[{duplicate_history.min_timestamp}, {duplicate_history.max_timestamp}])."
            )

    # Initialize SLT pipeline dynamically from config
    slt_pipeline = run_SLT.SLTPipeline(
        trigger_type=slt_p.get("trigger_type", 2),
        nhit_threshold=slt_p.get("nhit_threshold", 5),
        time_window_ns=slt_p.get("time_window_ns", 15000),
        causal_window_enabled=slt_p.get("causal_window_enabled", False),
        causal_tolerance_ns=slt_p.get("causal_tolerance_ns", 100.0),
        antenna_distances_file=str(antenna_dist_path),
        template_match_filter_enable=slt_p.get("template_match_filter_enable", True),
        template_match_coefficient=slt_p.get("template_match_coefficient", 0.7),
        duplicate_filter_enable=duplicate_filter_enable,
        duplicate_time_diff_ns=duplicate_time_diff_ns,
        duplicate_min_pair=duplicate_min_pair,
        duplicate_history_size=duplicate_history_size,
    )

    results = []

    # Iterate through uniform timestamp grid
    for idx, t_scan in enumerate(scan_timestamps):
        logging.info(f"[{idx + 1}/{n_steps}] Scanning at t_scan={t_scan:.6f}s")

        res = process_event(
            fname_sim=str(fname_sim),
            i_sim=i_sim,
            slt_pipeline=slt_pipeline,
            fname_FLT0_thresholds=flt0_thresholds,
            fname_MD_lookup_table=md_lookup_table,
            sim_timestamp=float(t_scan),
            fname_DU_to_FEB=du_to_feb,
            fname_duplicate_log=duplicate_history,
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

        results.append((float(t_scan), int(trig), n_flt0, n_flt1))

    results_arr = np.array(results)

    if out_file:
        out_path = Path(out_file)
        out_path.parent.mkdir(parents=True, exist_ok=True)
        if out_path.suffix == ".npz":
            np.savez_compressed(
                out_path,
                t_scan=results_arr[:, 0],
                triggered=results_arr[:, 1].astype(bool),
                n_flt0_passed=results_arr[:, 2].astype(np.int16),
                n_flt1_passed=results_arr[:, 3].astype(np.int16),
            )
        else:
            np.savetxt(
                out_path,
                results_arr,
                fmt=["%.9f", "%d", "%d", "%d"],
                header="t_scan triggered n_flt0_passed n_flt1_passed",
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
