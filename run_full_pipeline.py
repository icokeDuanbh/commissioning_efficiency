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
from FLT0 import run_FLT0
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
    fname_md_prev: str,
    i_sim: int,
    i_md_prev: int,
    i_md_next: int,
    slt_pipeline: run_SLT.SLTPipeline,
    fname_md_next: str = None,
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
    fname_md_prev: str
        The filename of the first MD file.
    i_sim: int
        The index of the event to be simulated.
    i_md_prev: int
        The index of the first half the background trace.
    i_md_next: int
        The index of the second half the background trace.
    slt_pipeline: run_SLT.SLTPipeline
        The SLT trigger configurations.
    fname_md_next: str, optional
        The filename of the second MD file (defaults to fname_md_prev).
    flt0_params: dict
        The FLT0 parameters with threshold and time settings.
    flt1_score_min: float
        The minimum correlation score for FLT1 at the DU level.
    min_flt1_dus: int
        The minimum number of DUs in an event.
    use_python_flt0: bool
        If True, uses fast Python FLT0.
    if_filter: bool
        If True, applies notch + FIR filter. If False, skips filtering.
    """
    if flt0_params is None:
        flt0_params = {
            "th1": 60,
            "th2": 45,
            "t_quiet": 200,
            "t_period": 500,
            "t_sepmax": 25,
            "nc_min": 2,
            "nc_max": 9,
        }

    # =========================================================================
    # Step 1 & 2: Read event and add MD background noise
    # =========================================================================
    logging.info(f"--- Event {i_sim}: Injecting MD noise ---")
    trace_added, n_du = add_sim_MD.add_sim_md(
        str(fname_sim), str(fname_md_prev), i_sim, i_md_prev, i_md_next, fname_md_next=str(fname_md_next) if fname_md_next else None
    )

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

    for i_du in range(n_du):
        triggered_du = False
        # Evaluate channels (X and Y)
        for i_ch in range(2):
            trace = trace_filtered[i_du][i_ch]
            logging.debug(f"  trace max {np.max(trace)}, th1={flt0_params['th1']}")
            
            if np.max(trace) <= flt0_params["th1"]:
                # Preselection check: Skip FLT0 simulation if peak value doesn't reach threshold_t1
                logging.debug(f"  FLT0: Skipping DU{i_du}-Ch{i_ch}/{n_du}")
                continue

            if use_python_flt0:
                trig = run_FLT0.run_trigger_py(
                    trace=trace,
                    threshold_t1=flt0_params["th1"],
                    threshold_t2=flt0_params["th2"],
                    tprev=flt0_params["t_quiet"],
                    tperiod=flt0_params["t_period"],
                    tcmax=flt0_params["t_sepmax"],
                    ncmin=flt0_params["nc_min"],
                    ncmax=flt0_params["nc_max"],
                )
            else:
                trig = run_FLT0.run_trigger_simulation(
                    samples=trace,
                    threshold_t1=flt0_params["th1"],
                    threshold_t2=flt0_params["th2"],
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

    file_sim = DataFile(str(fname_sim))
    file_sim.tadc.get_entry(i_sim)

    for i_du in flt0_survivor_indices:
        i_du = int(i_du)
        du_id = file_sim.tadc.du_id[i_du] if hasattr(file_sim.tadc, "du_id") else i_du
        du_sec = file_sim.tadc.du_seconds[i_du] if hasattr(file_sim.tadc, "du_seconds") else 1_000_000_000
        du_nsec = file_sim.tadc.du_nanoseconds[i_du] if hasattr(file_sim.tadc, "du_nanoseconds") else 0
        ts_ns = int(du_nsec)  # Only take nanoseconds for 1-second bucket granularity

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

    file_sim.close()

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
    )

    print("Full FLT0 -> FLT1 -> SLT pipeline script ready.")
