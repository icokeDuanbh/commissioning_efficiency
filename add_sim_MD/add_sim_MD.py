from pathlib import Path
from grand.dataio import DataFile
import numpy as np
import ROOT
import logging
from .lookup_table import MDLookupTable, load_lookup_table, load_du_to_feb_map

logging.basicConfig(level=logging.INFO)
logging.getLogger('grand.dataio.data_tree').setLevel(logging.CRITICAL)


DEFAULT_MAPPING_FILE = Path(__file__).resolve().parent / "GP65_DUID_FEBID_from_json.txt"
DEFAULT_CALIB_DIR = Path(__file__).resolve().parent / "calibration"


def load_calibration_curve(feb_id: int, pol: str, calib_dir: Path = DEFAULT_CALIB_DIR):
    """
    Load (freq_MHz, correction_DU_specific) for a given FEB ID and polarization ('X' or 'Y').
    Returns None if the calibration file does not exist.
    """
    pol_folder = f"{pol.upper()}raw"
    csv_path = Path(calib_dir) / pol_folder / f"DU{feb_id}_{pol_folder}_RFchain_from_sqrtG_detailed.csv"
    if not csv_path.exists():
        return None

    # Column 0: freq_MHz, Column 6: correction_DU_specific
    freq_mhz, corr_du = np.loadtxt(
        csv_path, delimiter=",", skiprows=1, usecols=(0, 6), unpack=True
    )
    return freq_mhz, corr_du


def calibrate_sim_trace(
    trace_sim: np.ndarray,
    freq_calib_mhz: np.ndarray,
    corr_calib: np.ndarray,
    fs_mhz: float = 500.0,
) -> np.ndarray:
    """
    Apply frequency-dependent calibration (30 - 250 MHz) to a simulated trace via FFT -> IFFT,
    clamping frequencies below 30 MHz to the 30 MHz calibration value and rounding to int.
    """
    n_samples = len(trace_sim)
    freqs_mhz = np.abs(np.fft.fftfreq(n_samples, d=1.0 / fs_mhz))

    gain = np.interp(freqs_mhz, freq_calib_mhz, corr_calib, left=corr_calib[0], right=corr_calib[-1])

    fft_sim = np.fft.fft(trace_sim)
    trace_calibrated = np.fft.ifft(fft_sim * gain).real

    return np.rint(trace_calibrated).astype(np.int32)


def add_sim_md(
    fname_sim,
    i_sim,
    fname_MD_lookup_table,
    sim_timestamp=None,
    fname_DU_to_FEB=None,
    calib_dir=DEFAULT_CALIB_DIR,
):
    """
    Add DU-dependent MD background noise to simulated traces using a lookup table.

    For each DU in the simulated event, maps its station DU ID (`du_id`) to the deployed
    hardware FEB ID (`feb_id`) via `fname_DU_to_FEB` (defaults to GP65_DUID_FEBID_from_json.txt),
    looks up the temporally closest MD entry (or 2 entries for 512-sample traces) for that FEB
    at `sim_timestamp`, and adds the MD trace to the simulated trace.
    If no FEB is deployed at that station, or if no MD data exists for that FEB, the trace is masked to zero.

    Parameters:
    -----------
    fname_sim : str or Path
        Path to the simulation ROOT file.
    i_sim : int
        Index of the simulation event to process.
    fname_MD_lookup_table : str, Path, pd.DataFrame, or MDLookupTable
        Path to pre-generated MD lookup table (or preloaded MDLookupTable instance).
    sim_timestamp : float, optional
        Target timestamp (in seconds) of the simulation event.
        If None, automatically extracted from the simulation event's du_seconds/du_nanoseconds.
    fname_DU_to_FEB : str, Path, or dict, optional
        Path to mapping file (or dict) mapping simulated DU ID to hardware FEB ID.
        Defaults to GP65_DUID_FEBID_from_json.txt if available.

    Returns:
    --------
    trace_added : numpy.ndarray
        Combined trace array of shape (n_du, 3, length_sim).
    n_du : int
        Number of simulated DUs in this event.
    """
    if fname_MD_lookup_table is None:
        raise ValueError("fname_MD_lookup_table is required for add_sim_md.")
    table = load_lookup_table(fname_MD_lookup_table)

    if fname_DU_to_FEB is None and DEFAULT_MAPPING_FILE.exists():
        fname_DU_to_FEB = DEFAULT_MAPPING_FILE
    feb_map = load_du_to_feb_map(fname_DU_to_FEB)

    file_sim = DataFile(str(fname_sim))
    file_sim.tadc.get_entry(i_sim)

    n_du = file_sim.tadc.du_count
    length_sim = len(file_sim.tadc.trace_ch[0][0])

    if sim_timestamp is None:
        if hasattr(file_sim.tadc, "du_seconds") and len(file_sim.tadc.du_seconds) > 0:
            sec = float(file_sim.tadc.du_seconds[0])
            nsec = float(file_sim.tadc.du_nanoseconds[0]) if hasattr(file_sim.tadc, "du_nanoseconds") and len(file_sim.tadc.du_nanoseconds) > 0 else 0.0
            sim_timestamp = sec + nsec / 1e9
        else:
            sim_timestamp = 0.0

    trace_added = np.zeros((n_du, 3, length_sim))
    file_cache = {}
    calib_cache = {}

    def _read_md_trace(path_str, entry_index):
        if path_str not in file_cache:
            md_file = ROOT.TFile.Open(str(path_str), "READ")
            if not md_file or md_file.IsZombie():
                raise OSError(f"Cannot open MD ROOT file: {path_str}")
            tree = md_file.Get("teventadc")
            split_traces = bool(tree)
            branches = ("trace_1", "trace_2", "trace_3") if split_traces else ("trace_ch",)
            if not split_traces:
                tree = md_file.Get("tadc")
            if (not tree or not tree.InheritsFrom("TTree")
                    or any(not tree.GetBranch(name) for name in branches)):
                md_file.Close()
                raise ValueError(f"Missing MD ADC tree or waveform branches {branches}: {path_str}")
            # Read the waveform without assigning unrelated GRANDlib fields.
            tree.SetBranchStatus("*", 0)
            for name in branches:
                tree.SetBranchStatus(name + "*", 1)
            file_cache[path_str] = (md_file, tree, split_traces)

        _, tree, split_traces = file_cache[path_str]
        entry_index = int(entry_index)
        if not 0 <= entry_index < tree.GetEntries() or tree.GetEntry(entry_index) <= 0:
            raise OSError(f"Cannot read MD entry {entry_index}: {path_str}")
        if split_traces:
            channels = (tree.trace_1, tree.trace_2, tree.trace_3)
            if any(len(channel) != 1 for channel in channels):
                raise ValueError(
                    f"Expected one DU per MD entry in teventadc. Entry {entry_index}: {path_str}"
                )
            return tuple(np.array(channel[0], copy=True) for channel in channels)
        traces = tree.trace_ch
        if len(traces) == 0 or len(traces[0]) < 4:
            raise ValueError(f"MD entry {entry_index} lacks channels 1–3: {path_str}")
        return tuple(np.array(traces[0][ch], copy=True) for ch in (1, 2, 3))

    def _get_calib(feb_id, pol):
        key = (feb_id, pol)
        if key not in calib_cache:
            calib_cache[key] = load_calibration_curve(feb_id, pol, calib_dir=calib_dir)
        return calib_cache[key]

    try:
        for i_du in range(n_du):
            sim_du_id = file_sim.tadc.du_id[i_du] 

            if feb_map is not None:
                target_feb_id = feb_map.get(sim_du_id, None)
                if target_feb_id is None:
                    logging.warning(f"No FEB deployed at DU #{i_du} (station {sim_du_id}); masking trace to zero.")
                    trace_added[i_du] = 0.0
                    continue
            else:
                logging.warning(f"No DU-to-FEB mapping available for DU #{i_du} (station {sim_du_id}); masking trace to zero.")
                trace_added[i_du] = 0.0
                continue

            calib_x = _get_calib(target_feb_id, "X")
            calib_y = _get_calib(target_feb_id, "Y")
            if calib_x is None or calib_y is None:
                logging.warning(
                    f"Missing X/Y calibration file for FEB ID {target_feb_id} (station {sim_du_id}); masking trace to zero."
                )
                trace_added[i_du] = 0.0
                continue

            md_info = table.get_md_entries_for_feb(target_feb_id, sim_timestamp)
            if md_info is None:
                logging.warning(f"FEB ID {target_feb_id} (station {sim_du_id}) not found in MD lookup table; masking trace to zeros.")
                trace_added[i_du] = 0.0
                continue

            fname_prev, i_MD_prev, fname_next, i_MD_next = md_info

            trace_md_x, trace_md_y, trace_md_z = _read_md_trace(fname_prev, i_MD_prev)
            length_MD = len(trace_md_x)

            if length_MD * 2 == length_sim:
                next_x, next_y, next_z = _read_md_trace(fname_next, i_MD_next)
                trace_md_x = np.concatenate((trace_md_x, next_x))
                trace_md_y = np.concatenate((trace_md_y, next_y))
                trace_md_z = np.concatenate((trace_md_z, next_z))
            if any(len(trace) != length_sim for trace in (trace_md_x, trace_md_y, trace_md_z)):
                raise ValueError(
                    f"Unsupported MD trace length ({length_MD}) for FEB {target_feb_id} with sim length ({length_sim})."
                )

            sim_x_cal = calibrate_sim_trace(np.asarray(file_sim.tadc.trace_ch[i_du][0]), calib_x[0], calib_x[1])
            sim_y_cal = calibrate_sim_trace(np.asarray(file_sim.tadc.trace_ch[i_du][1]), calib_y[0], calib_y[1])

            trace_added[i_du][0] = trace_md_x + sim_x_cal
            trace_added[i_du][1] = trace_md_y + sim_y_cal
            trace_added[i_du][2] = trace_md_z + file_sim.tadc.trace_ch[i_du][2]

    finally:
        for md_file, _, _ in file_cache.values():
            try:
                md_file.Close()
            except Exception:
                pass
        file_sim.close()

    logging.debug("DU-dependent addition finished.")
    return trace_added, n_du


if __name__ == "__main__":
    i_sim = 20
    test_file_sim = "test_data/sim_Dunhuang_20170331_220000_RUN1_CD_GP300-no-noise_0000/adc_0-99_L1_0000.root"
    test_lookup_table = "results/test_md_lookup_table.csv"

    trace_added, n_du = add_sim_md(test_file_sim, i_sim, test_lookup_table, sim_timestamp=1782725975.0)
    print(f"[DU-dependent Mode] {n_du} DUs in event. Shape: {trace_added.shape}")
