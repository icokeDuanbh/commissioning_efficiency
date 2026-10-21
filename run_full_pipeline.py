#!/usr/bin/env python3
"""
Full FLT0 -> FLT1 -> Preselection -> SLT pipeline script.

Pipeline steps for each event:
1. Read an event from simulation ROOT file and add background noise using add_sim_MD.
2. Filter the traces (notch + FIR filter) and apply FLT0 to each DU/channel.
3. For FLT0 survivors (DUs that passed FLT0), evaluate FLT1 using `call_scope_t2_py`.
4. Perform preselection: Only proceed if 5 or more DUs pass FLT1 with a correlation score > 0.5 (where out0 is normalized).
5. Apply the SLT pipeline to the surviving event.
6. Return the final trigger decision.
"""

import sys
import os
from pathlib import Path
import numpy as np
import logging

# Ensure top-level directory is in sys.path
ROOT = Path(__file__).resolve().parent
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

# Import project modules
from grand.dataio import DataFile
from filters import filters
from add_sim_MD import add_sim_MD
from add_sim_MD.lookup_table import load_du_to_feb_map
from FLT0 import run_FLT0
from FLT0.thresholds import FLT0Thresholds, load_flt0_thresholds
from FLT1 import run_FLT1
from SLT import run_SLT

logging.basicConfig(level=logging.DEBUG)
logging.getLogger('grand.dataio.data_tree').setLevel(logging.CRITICAL)

# Path configuration
DEFAULT_SIM_FILE = ROOT / "test_data/sim_Dunhuang_20170331_220000_RUN1_CD_GP300-no-noise_0000/adc_0-99_L1_0000.root"
DEFAULT_MD_FILE  = ROOT / "test_data/GP80_20260629_094124_RUN607_MD_20dB-GP65-60DUs-512trace-20Hz-2min-FY2Float-TEST-DUNHUANG-0004.dat" # Placeholder or path provided by environment
DEFAULT_DISTANCES_FILE = ROOT / "grand_daq/grand-daq/cfgs/detector_distances_May24_v2.txt"


def process_event(
    fname_sim: str,
    i_sim: int,
    slt_pipeline: run_SLT.SLTPipeline,
    fname_FLT0_thresholds,
    fname_MD_lookup_table,
    sim_timestamp: float = None,
    fname_DU_to_FEB=None,
    flt0_params: dict = None,
    flt1_score_min: float = 0.0,
    min_flt1_dus: int = 1,
    use_python_flt0: bool = True,
    if_filter: bool = True,
):
    """
    Process a single event through the full FLT0-FLT1-SLT pipeline.

    Parameters: 
    -----------
    fname_sim: str
        The filename of the simulation.
    i_sim: int
        The index of the event to be simulated.
    slt_pipeline: run_SLT.SLTPipeline
        The SLT trigger configurations.
    fname_FLT0_thresholds: str, Path or FLT0Thresholds
        MANDATORY. The FEB-dependent FLT0 threshold file (header: feb_id th1_x th2_x th1_y th2_y),
        or an already loaded FLT0Thresholds object. A DU whose FEB is unknown or has no
        threshold entry is skipped in FLT0.
    fname_MD_lookup_table: str, Path or MDLookupTable
        MANDATORY. The MD lookup table for DU-dependent MD addition (each DU gets the
        MD of its own FEB, closest in time to sim_timestamp).
    sim_timestamp: float, optional
        Timestamp (s) of the simulated event for the MD lookup.
    fname_DU_to_FEB: str, Path or dict, optional
        The DU ID -> FEB ID mapping (defaults to add_sim_MD/GP65_DUID_FEBID_from_json.txt).
        Used for both the MD addition and the FLT0 thresholds.
    flt0_params: dict
        The FLT0 timing/crossing parameters (th1/th2 are NOT taken from here).
    flt1_score_min: float
        The minimum correlation score for FLT1 at the DU level.
    min_flt1_dus: int
        The minimum number of DUs in an event.
    use_python_flt0: bool
        If True, uses fast Python FLT0.
    if_filter: bool
        If True, applies notch + FIR filter. If False, skips filtering.
    """
    if fname_FLT0_thresholds is None:
        raise ValueError("fname_FLT0_thresholds is mandatory: the FLT0 thresholds are FEB-dependent and come from a file.")
    if isinstance(fname_FLT0_thresholds, FLT0Thresholds):
        flt0_thresholds = fname_FLT0_thresholds
    else:
        flt0_thresholds = load_flt0_thresholds(fname_FLT0_thresholds)

    if fname_MD_lookup_table is None:
        raise ValueError("fname_MD_lookup_table is mandatory: MD addition is DU-dependent and requires a lookup table.")

    if flt0_params is None:
        flt0_params = {
            "t_quiet": 200,
            "t_period": 500,
            "t_sepmax": 25,
            "nc_min": 2,
            "nc_max": 9,
        }
    if "th1" in flt0_params or "th2" in flt0_params:
        logging.warning("flt0_params['th1'/'th2'] are ignored: the FLT0 thresholds come from the FEB threshold file.")

    # DU ID -> FEB ID mapping (the same one is used by the MD addition)
    du_to_feb = load_du_to_feb_map(fname_DU_to_FEB if fname_DU_to_FEB is not None else add_sim_MD.DEFAULT_MAPPING_FILE)

    # =========================================================================
    # Step 1 & 2: Read event and add MD background noise
    # =========================================================================
    logging.info(f"--- Event {i_sim}: Injecting MD noise ---")
    trace_added, n_du = add_sim_MD.add_sim_md(
        str(fname_sim),
        i_sim,
        fname_MD_lookup_table,
        sim_timestamp=sim_timestamp,
        fname_DU_to_FEB=du_to_feb,
    )

    # Read the per-DU metadata of the simulated event once (DU IDs may be discontinuous)
    file_sim = DataFile(str(fname_sim))
    file_sim.tadc.get_entry(i_sim)
    sim_du_ids = [int(x) for x in file_sim.tadc.du_id] if hasattr(file_sim.tadc, "du_id") else list(range(n_du))
    sim_du_nsec = [int(x) for x in file_sim.tadc.du_nanoseconds] if hasattr(file_sim.tadc, "du_nanoseconds") else [0] * n_du
    file_sim.close()
    if len(sim_du_ids) != n_du:
        raise RuntimeError(f"Event {i_sim}: {len(sim_du_ids)} DU IDs in the sim file but {n_du} DUs in the traces.")

    # Filter traces: Notch + FIR filter (if enabled)
    if if_filter:
        fn = 39e6  # Notch frequency (Hz)
        r = 0.9    # Notch filter radius
        trace_filtered = np.zeros_like(trace_added)
        for i_du in range(n_du):
            for i_ch in range(3):
                trace = trace_added[i_du][i_ch]
                _notched = filters.apply_notch_filter(trace, fn, r)
                trace_filtered[i_du][i_ch] = filters.apply_fir_filter(_notched)
    else:
        logging.info(f"--- Event {i_sim}: Skipping Notch+FIR filtering (if_filter=False) ---")
        trace_filtered = trace_added

    # =========================================================================
    # Step 3: Apply FLT0 (First Level Trigger)
    # =========================================================================
    logging.info(f"--- Event {i_sim}: Running FLT0 on {n_du} DUs (Python backend: {use_python_flt0}) ---")
    flt0_triggered = np.zeros(n_du, dtype=bool)

    # Loop over the simulated DU IDs (they may be discontinuous, e.g. 0, 2, 10, 20).
    # i_du indexes the trace array; du_id is the real DU ID used to find the FEB.
    for i_du, du_id in enumerate(sim_du_ids):
        feb_id = du_to_feb.get(du_id)  # None -> no FEB deployed at this DU ID
        du_thresholds = flt0_thresholds.get(feb_id) if feb_id is not None else None
        if du_thresholds is None:
            if feb_id is None:
                cause = f"no FEB is mapped to DU ID {du_id}"
            else:
                cause = f"FEB {feb_id} has no entry in the threshold file"
            logging.warning(
                f"FLT0: skipping DU ID {du_id} (index {i_du}): "
                f"No FEB at the DU ID or no threshold is given to the FEB. ({cause})"
            )
            continue

        triggered_du = False
        # Evaluate channels (X and Y are always channels 0 and 1)
        for i_ch in range(2):
            th1, th2 = du_thresholds[i_ch]
            trace = trace_filtered[i_du][i_ch]
            logging.debug(f"  DU{du_id} (FEB {feb_id}) ch{i_ch}: trace max {np.max(trace)}, th1={th1}, th2={th2}")
            
            if np.max(trace) <= th1:
                # Preselection check: Skip FLT0 simulation if peak value doesn't reach threshold_t1
                logging.debug(f"  FLT0: Skipping DU{i_du}-Ch{i_ch}/{n_du}")
                continue

            if use_python_flt0:
                trig = run_FLT0.run_trigger_py(
                    trace=trace,
                    threshold_t1=th1,
                    threshold_t2=th2,
                    tprev=flt0_params["t_quiet"],
                    tperiod=flt0_params["t_period"],
                    tcmax=flt0_params["t_sepmax"],
                    ncmin=flt0_params["nc_min"],
                    ncmax=flt0_params["nc_max"],
                )
            else:
                trig = run_FLT0.run_trigger_simulation(
                    samples=trace,
                    threshold_t1=th1,
                    threshold_t2=th2,
                    tprev=flt0_params["t_quiet"],
                    tperiod=flt0_params["t_period"],
                    tcmax=flt0_params["t_sepmax"],
                    ncmin=flt0_params["nc_min"],
                    ncmax=flt0_params["nc_max"],
                    verbose=False
                )

            logging.debug(f"  FLT0: Finished DU{i_du}-Ch{i_ch}/{n_du}, trigger {trig}")
            if trig:
                triggered_du = True
                break
        flt0_triggered[i_du] = triggered_du

    flt0_survivor_indices = np.where(flt0_triggered)[0]
    n_flt0_passed = len(flt0_survivor_indices)
    logging.info(f"FLT0 passed for {n_flt0_passed} / {n_du} DUs.")

    # =========================================================================
    # Step 3 (cont): Apply FLT1 only to FLT0 survivors
    # =========================================================================
    flt1_dus = []
    flt1_scores = []
    flt1_timestamps = []

    for i_du in flt0_survivor_indices:
        i_du = int(i_du)
        du_id = sim_du_ids[i_du]
        ts_ns = sim_du_nsec[i_du]  # Only take nanoseconds for 1-second bucket granularity

        ch1_trace = trace_filtered[i_du][0].astype(np.int16)
        ch2_trace = trace_filtered[i_du][1].astype(np.int16)
        ch3_trace = trace_filtered[i_du][2].astype(np.int16)
        ch4_trace = np.zeros_like(ch1_trace, dtype=np.int16)

        ret, out0, out1, out_size = run_FLT1.call_scope_t2_py(
            trigger_pattern=1000,
            t_pre_coincidence_ch2=1036,
            t_pre_coincidence_ch3=1036,
            t_period_reg_ch2=10,
            t_period_reg_ch3=10,
            custom_trace_ch1=ch1_trace,
            custom_trace_ch2=ch2_trace,
            custom_trace_ch3=ch3_trace,
            custom_trace_ch4=ch4_trace,
        )

        # Scale out0 uint16 score to float (0.0 to 1.0)
        norm_score = float(out0) / 65535.0 if out0 is not None else 0.0

        if norm_score > flt1_score_min:
            flt1_dus.append(du_id)
            flt1_scores.append(out0) # Keep uint16 score for SLT input
            flt1_timestamps.append(ts_ns)

    n_flt1_passed = len(flt1_dus)
    logging.info(f"FLT1 passed (correlation > {flt1_score_min}) for {n_flt1_passed} DUs.")

    # =========================================================================
    # Step 3.5: Preselection Cut (min_flt1_dus required for SLT)
    # =========================================================================
    if n_flt1_passed < min_flt1_dus:
        logging.info(f"Preselection FAILED: Only {n_flt1_passed} DUs passed FLT1 (minimum required: {min_flt1_dus}). Skipping SLT.")
        return {
            "triggered": False,
            "n_du_total": n_du,
            "n_flt0_passed": n_flt0_passed,
            "n_flt1_passed": n_flt1_passed,
            "slt_result": None,
        }

    logging.info(f"Preselection PASSED ({n_flt1_passed} DUs >= {min_flt1_dus}). Proceeding to SLT.")

    # =========================================================================
    # Step 4: Apply SLT (Second Level Trigger) Pipeline
    # =========================================================================
    du_ids_arr = np.array(flt1_dus, dtype=np.uint32)
    timestamps_arr = np.array(flt1_timestamps, dtype=np.uint64)
    scores_arr = np.array(flt1_scores, dtype=np.uint16)

    slt_result = slt_pipeline.run(
        du_ids=du_ids_arr,
        timestamps_ns=timestamps_arr,
        related_values=scores_arr,
    )

    final_triggered = bool(slt_result.get("triggered", False))
    logging.info(f"SLT Pipeline Final Decision: Triggered = {final_triggered}")

    return {
        "triggered": final_triggered,
        "n_du_total": n_du,
        "n_flt0_passed": n_flt0_passed,
        "n_flt1_passed": n_flt1_passed,
        "slt_result": slt_result,
    }


if __name__ == "__main__":
    distances_file = str(DEFAULT_DISTANCES_FILE)

    # Initialize SLT pipeline
    slt_pipeline = run_SLT.SLTPipeline(
        trigger_type=2,               # nhit + causal window + T3Filter
        nhit_threshold=5,
        time_window_ns=15_000,
        causal_window_enabled=True,
        antenna_distances_file=distances_file,
        template_match_filter_enable=True,
        duplicate_filter_enable=True,
        duplicate_history_size=10,
        duplicate_min_pair=2,
        duplicate_time_diff_ns=100,
    )

    # Optionally warm up the duplicate-filter history from a reject log.
    # Set duplicate_history_file to the path of a DuplicateRejectLog_*.log
    # file downloaded from the online DAQ for the run being simulated.
    duplicate_history_file = str(ROOT / "test_data/duplicate_library/DuplicateRejectLog_10439_20260902_064452.log")
    if duplicate_history_file:
        n_seeded = slt_pipeline.preload_duplicate_history(duplicate_history_file)
        print(f"Seeded {n_seeded} events into the duplicate-cut history.")

    print("Full FLT0 -> FLT1 -> SLT pipeline script ready.")
