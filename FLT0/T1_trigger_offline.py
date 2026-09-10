#!/pbs/home/x/xtian/.conda/envs/grandlib2304/bin/python3.9
"""
T1_trigger_offline.py
=====================
Offline FLT0 trigger — thin adapter over trigger_FLT0().

The core algorithm lives in offline_FLT0_trigger.py (canonical implementation,
shared with Clement's analysis chain). This module exposes
extract_trigger_parameters() as a compatibility shim so that run_FLT0.py
requires no changes.

NC bounds are exclusive on both sides (nc_min < NC < nc_max), matching the
FPGA firmware (sig_det.v line 261).
"""
import numpy as np
import sys
try:
    import grand.dataio.root_trees as rt
except ImportError:
    rt = None

from offline_FLT0_trigger import trigger_FLT0


def extract_trigger_parameters(trace, trigger_config, baseline=0):
    """
    Offline FLT0 trigger decision for a single trace.

    Wraps trigger_FLT0() and returns information for the *first* valid T1
    crossing found in the trace, preserving API compatibility with run_FLT0.py.

    Parameters
    ----------
    trace : numpy.ndarray
        ADC trace in ADC counts.
    trigger_config : dict
        Trigger parameters with keys:
          th1, th2, t_quiet, t_period, t_sepmax, nc_min, nc_max
    baseline : int, optional
        Unused — kept for API compatibility.

    Returns
    -------
    dict
        index_T1_crossing : int   – sample index of the first valid T1
        NC                : int   – T2 crossing count for that T1
        T1_amplitude      : float – ADC value at the T1 crossing

    Raises
    ------
    ValueError
        If no valid T1 crossing is found (no T1, quiet violation,
        Tsepmax violation, or NC out of range).
    """
    T1_indices, T1_amplitudes, NC_values = trigger_FLT0(trace, trigger_config)

    if not T1_indices:
        raise ValueError("No valid T1 crossing found.")

    return {
        "index_T1_crossing": T1_indices[0],
        "NC":                 NC_values[0],
        "T1_amplitude":       T1_amplitudes[0],
    }

dict_trigger_parameter = dict([
  ("t_quiet", 512),
  ("t_period", 512),
  ("t_sepmax", 10),
  ("nc_min", 2),
  ("nc_max", 8),
  ("q_min", 0),
  ("q_max", 255),
  ("th1", 100),
  ("th2", 50),
  # Configs of readout timewindow
  ("t_pretrig", 960),
  ("t_overlap", 64),
  ("t_posttrig", 1024)
  ])


if __name__ == "__main__":
  # Read the traces from experimental data
  fname = sys.argv[1]
  file = rt.DataFile(fname)
  n_entries = file.tadc.get_number_of_entries()
  # Pad zeros at the head of the trace to statify the Tquiet condition
  zero_head = np.zeros(dict_trigger_parameter["t_quiet"] // 2, dtype=int)

  trigger_index = []
  # Loop over all entries
  for k in range(n_entries):
    file.tadc.get_entry(k)
    # Loop over four channels
    for v in range(4):
      trace = file.tadc.trace_ch[0][v]
      # Zero padding at first
      # trace_padded = np.concatenate((zero_head, trace))
      try:
        # Check if trigger
        trigger_infos = extract_trigger_parameters(trace, dict_trigger_parameter)
        # Save the triggered traces
        if trigger_infos["NC"] >= dict_trigger_parameter["nc_min"] and trigger_infos["NC"] <= dict_trigger_parameter["nc_max"]:
          trigger_index.append(k)
          break
      except ValueError:
        # No T1 crossing, no trigger
        # print(k, ": No trigger.")
        pass

  print(f"{fname}: {len(trigger_index)} out of {n_entries} triggered.")
  if len(trigger_index) > 0:
    np.savetxt(f"./{fname.split('/')[-1]}.trigger.txt", trigger_index, delimiter=', ', fmt='%d', header=str(dict_trigger_parameter))
