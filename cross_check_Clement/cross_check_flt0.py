"""
cross_check_flt0.py  --  FLT0 Python vs HDL (Verilog) cross-check
==================================================================
Compares two FLT0 trigger implementations on the same set of traces:

  Python : FLT0/offline_FLT0_trigger.py  --> trigger_FLT0()
  HDL    : FLT0/sig_det.v via cocotb     --> run_trigger_simulation()

Both are reduced to a single bool: triggered / not triggered.

Note on quiet-violation testing
--------------------------------
It is impossible to construct a test where a *non-T1* signal value
contaminates the quiet window of a target T1, because the quiet check is
`signal <= th1`. Any value that would fail the check (> th1) is itself a
T1 candidate. Case 02 therefore tests "double-pulse suppression".

Usage
-----
  python3 cross_check_flt0.py                   # synthetic only (HDL skipped by default)
  python3 cross_check_flt0.py --hdl             # synthetic + HDL simulation (slow)
  python3 cross_check_flt0.py --hdl --real      # + real traces
  python3 cross_check_flt0.py -v                # verbose (print every case)
  python3 cross_check_flt0.py --npz /path/to/traces.npz --hdl
"""

import sys, os, argparse
import numpy as np

THIS_DIR = os.path.dirname(os.path.abspath(__file__))
FLT0_DIR = os.path.join(THIS_DIR, "..", "FLT0")
if THIS_DIR not in sys.path:  sys.path.insert(0, THIS_DIR)
if FLT0_DIR not in sys.path:  sys.path.insert(0, FLT0_DIR)

from offline_FLT0_trigger import trigger_FLT0
from run_FLT0 import run_trigger_simulation


# ---------------------------------------------------------------------------
# Default parameters  (DAQ units: ns for times, ADC counts for thresholds)
# ---------------------------------------------------------------------------
DEFAULT_PARAMS = {
    "th1":      500,
    "th2":      100,
    "t_quiet":   20,   # ns  -> tprev  = t_quiet // 2  clock cycles
    "t_period":  40,   # ns  -> tperiod = t_period // 10 (then window = 5x that)
    "t_sepmax":  30,   # ns  between consecutive T2 crossings
    "nc_min":     2,
    "nc_max":     8,
}


# ---------------------------------------------------------------------------
# Wrappers: both return bool
# ---------------------------------------------------------------------------

def run_python(trace, p):
    """trigger_FLT0: True if any valid T1 with nc_min < NC < nc_max."""
    T1s, _, NCs = trigger_FLT0(trace, p)
    for nc in NCs:
        if p["nc_min"] < nc < p["nc_max"]:
            return True
    return False


def run_hdl(trace, p):
    """run_trigger_simulation: cocotb/Verilog sig_det.v."""
    return run_trigger_simulation(
        samples=trace,
        threshold_t1=p["th1"],
        threshold_t2=p["th2"],
        tprev=p["t_quiet"],
        tperiod=p["t_period"],
        tcmax=p["t_sepmax"],
        ncmax=p["nc_max"],
        ncmin=p["nc_min"],
    )


# ---------------------------------------------------------------------------
# Synthetic test cases
# ---------------------------------------------------------------------------

def build_cases(p):
    """Return list of (label, trace, expected_python, expected_hdl)."""
    th1   = p["th1"]
    th2   = p["th2"]
    tp    = p["t_period"] // 2   # samples in observation window
    tsep  = p["t_sepmax"]
    nc_min = p["nc_min"]
    nc_max = p["nc_max"]

    def Z(): return np.zeros(512, dtype=int)
    cases = []

    # 01. No signal
    cases.append(("01_no_signal", Z(), False, False))

    # 02. Double-pulse suppression.
    # T1_A at idx 130: quiet [120:130] clear -> fires (NC=3 with 2 T2 crossings).
    # T1_B at idx 140: quiet [130:140] sees T1_A spike (700 > th1) -> rejected.
    t = Z()
    t[130] = th1 + 200   # T1_A
    t[131] = th2 + 10    # T2 crossing 1
    t[133] = th2 + 10    # T2 crossing 2
    t[140] = th1 + 50    # T1_B: quiet violated by T1_A
    cases.append(("02_double_pulse_suppression", t, True, True))

    # 03. NC exactly nc_min -> excluded (exclusive lower bound)
    t = Z()
    t[150] = th1 + 200
    # Only one T2 crossing after T1 -> NC = 2 = nc_min -> excluded
    t[152] = th2 + 10
    cases.append(("03_nc_equals_nc_min_excluded", t, False, False))

    # 04. Valid trigger: NC = nc_min + 1 = 3
    t = Z()
    t[150] = th1 + 300
    t[152] = th2 + 10
    t[154] = th2 + 10
    cases.append(("04_nc_min_plus_1_valid", t, True, True))

    # 05. NC exactly nc_max -> excluded (exclusive upper bound)
    t = Z()
    t[150] = th1 + 300
    for k in range(nc_max - 1):   # nc_max-1 extra T2 -> NC = nc_max
        pos = 152 + k * 2
        if pos < 150 + tp:
            t[pos] = th2 + 10
    cases.append(("05_nc_equals_nc_max_excluded", t, False, False))

    # 06. NC > nc_max -> window closes early, no trigger
    t = Z()
    t[150] = th1 + 300
    for k in range(nc_max + 1):
        pos = 152 + k * 2
        if pos < 150 + tp:
            t[pos] = th2 + 10
    cases.append(("06_nc_exceeds_nc_max", t, False, False))

    # 07. Tsepmax violated: large gap between T2 crossings
    t = Z()
    t[150] = th1 + 300
    t[152] = th2 + 10
    t[152 + (tsep + 20) // 2 + 2] = th2 + 10   # gap >> tsep
    cases.append(("07_tsepmax_violated", t, False, False))

    # 08. T1 in first 100 samples: Python skips (index<=100 guard), HDL?
    # tc1_time_cnt starts at 0 after reset; cocotb waits tprev+5 cycles before
    # injecting, so the first sample is already past the Tprev blanking timer.
    # HDL should trigger; Python skips due to index<=100 guard.
    t = Z()
    t[50] = th1 + 300
    t[52] = th2 + 10
    t[54] = th2 + 10
    cases.append(("08_t1_at_idx50", t, False, None))  # HDL=None: expected to differ

    # 09. Two valid T1s separated by a long quiet gap: both should trigger
    t = Z()
    for start in [150, 350]:
        t[start] = th1 + 300
        t[start + 2] = th2 + 10
        t[start + 4] = th2 + 10
    cases.append(("09_two_valid_t1s", t, True, True))

    return cases


# ---------------------------------------------------------------------------
# Comparison helper
# ---------------------------------------------------------------------------

def compare(label, py_result, hdl_result, exp_py, exp_hdl, verbose):
    agree  = (hdl_result is None) or (py_result == hdl_result)
    py_ok  = (exp_py  is None) or (py_result  == exp_py)
    hdl_ok = (hdl_result is None) or (exp_hdl is None) or (hdl_result == exp_hdl)

    flag = "OK" if (agree and py_ok and hdl_ok) else "!!"
    if verbose or not agree or not py_ok or not hdl_ok:
        hdl_str = "SKIP" if hdl_result is None else str(hdl_result)
        note = "AGREE" if agree else "DIFFER"
        if not py_ok:  note += " [python unexpected]"
        if not hdl_ok: note += " [HDL unexpected]"
        print(f"  [{flag}] {label:<40}  python={str(py_result):<6}  hdl={hdl_str:<6}  [{note}]")

    return agree


# ---------------------------------------------------------------------------
# Test runners
# ---------------------------------------------------------------------------

def run_synthetic(params, run_hdl_flag, verbose):
    print("\n" + "=" * 72)
    print("SYNTHETIC TEST CASES")
    print("=" * 72)
    cases = build_cases(params)
    n_agree, n_total = 0, 0

    for label, trace, exp_py, exp_hdl in cases:
        py_r = run_python(trace, params)
        hdl_r = run_hdl(trace, params) if run_hdl_flag else None
        ok = compare(label, py_r, hdl_r, exp_py, exp_hdl, verbose)
        n_total += 1
        if ok: n_agree += 1

    print(f"\nSynthetic: {n_agree}/{n_total} agree.")
    return n_agree, n_total


def run_real_traces(npz_path, params, run_hdl_flag, verbose):
    print("\n" + "=" * 72)
    print(f"REAL TRACE TESTS  -->  {os.path.basename(npz_path)}")
    print("=" * 72)

    data = np.load(npz_path, allow_pickle=True)
    keys = list(data.files)
    print(f"  Keys: {keys}")

    groups = []
    for k in keys:
        arr = data[k]
        if not isinstance(arr, np.ndarray) or arr.ndim == 0 or arr.shape[-1] < 50:
            continue
        if arr.ndim == 1:   groups.append((k, arr[np.newaxis, :]))
        elif arr.ndim == 2: groups.append((k, arr))
        elif arr.ndim == 3:
            for i in range(arr.shape[0]):
                groups.append((f"{k}_e{i}", arr[i]))

    if not groups:
        print("  No suitable trace arrays found.")
        return 0, 0

    n_agree, n_total = 0, 0
    for arr_name, traces in groups:
        print(f"\n  [{arr_name}]  {traces.shape[0]} ch x {traces.shape[-1]} samples")
        for ch in range(traces.shape[0]):
            trace = traces[ch].astype(int)
            py_r  = run_python(trace, params)
            hdl_r = run_hdl(trace, params) if run_hdl_flag else None
            ok = compare(f"{arr_name}_ch{ch}", py_r, hdl_r, None, None, verbose)
            n_total += 1
            if ok: n_agree += 1

    print(f"\n  Real traces: {n_agree}/{n_total} agree.")
    return n_agree, n_total


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description="FLT0 cross-check: Python vs HDL (Verilog)")
    ap.add_argument("--hdl",    action="store_true", help="Run HDL simulation (slow, requires cocotb)")
    ap.add_argument("--real",   action="store_true", help="Also test real traces from .npz")
    ap.add_argument("--npz",    default=None,        help="Path to .npz file with real traces")
    ap.add_argument("-v", "--verbose", action="store_true")
    for k, v in DEFAULT_PARAMS.items():
        ap.add_argument(f"--{k.replace('_','-')}", type=int, default=v, dest=k)
    args = ap.parse_args()
    params = {k: getattr(args, k) for k in DEFAULT_PARAMS}

    print("=" * 72)
    print("FLT0 CROSS-CHECK: Python vs HDL")
    print("  Python : FLT0/offline_FLT0_trigger.py :: trigger_FLT0()")
    print("  HDL    : FLT0/sig_det.v via cocotb    :: run_trigger_simulation()")
    if not args.hdl:
        print("  [HDL simulation DISABLED  --  pass --hdl to enable]")
    print(f"  Params : {params}")
    print("=" * 72)

    ta, tt = 0, 0

    na, nt = run_synthetic(params, args.hdl, args.verbose)
    ta += na; tt += nt

    if args.real:
        npz = args.npz
        if npz is None:
            cand = os.path.join(THIS_DIR, "..", "test_data", "entry_100_du_1090.npz")
            npz = cand if os.path.exists(cand) else None
        if npz:
            na, nt = run_real_traces(npz, params, args.hdl, args.verbose)
            ta += na; tt += nt
        else:
            print("\nNo .npz file found. Use --npz to specify one.")

    print("\n" + "=" * 72)
    print(f"GRAND TOTAL: {ta}/{tt} cases agree.")
    print("=" * 72)

    print("""
Known Python vs HDL differences to watch for:
  1. Tprev/quiet : Python uses backward window (t_quiet//2 samples).
                   HDL uses a counter since the last T1 up-crossing.
                   Equivalent only when signal fully decays between T1s.
  2. ADC polarity: HDL takes absolute value and uses trig_opt to select
                   positive/negative half. Python (trigger_FLT0) only checks
                   positive crossings (> th1, > th2).
  3. Index<=100   : Python skips T1s in first 100 samples (notch-filter guard).
                   HDL has no such guard; relies on Tprev blanking instead.
  4. TPER window  : HDL fires trigger only after the full TPER_5X window elapses.
                   Python checks within t_period//2 samples and returns immediately.
""")


if __name__ == "__main__":
    main()
