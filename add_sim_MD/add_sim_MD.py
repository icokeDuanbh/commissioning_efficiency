from pathlib import Path
from grand.dataio import DataFile
import numpy as np
import logging
from .lookup_table import MDLookupTable, load_lookup_table, load_du_to_feb_map

logging.basicConfig(level=logging.INFO)
logging.getLogger('grand.dataio.data_tree').setLevel(logging.CRITICAL)


DEFAULT_MAPPING_FILE = Path(__file__).resolve().parent / "GP65_DUID_FEBID_from_json.txt"


def add_sim_md(
    fname_sim,
    i_sim,
    fname_MD_lookup_table,
    sim_timestamp=None,
    fname_DU_to_FEB=None,
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

    def _get_df(path_str):
        if path_str not in file_cache:
            file_cache[path_str] = DataFile(path_str)
        return file_cache[path_str]

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

            md_info = table.get_md_entries_for_feb(target_feb_id, sim_timestamp)
            if md_info is None:
                logging.warning(f"FEB ID {target_feb_id} (station {sim_du_id}) not found in MD lookup table; masking trace to zeros.")
                trace_added[i_du] = 0.0
                continue

            fname_prev, i_MD_prev, fname_next, i_MD_next = md_info

            df_prev = _get_df(fname_prev)
            df_prev.tadc.get_entry(i_MD_prev)
            length_MD = len(df_prev.tadc.trace_ch[0][1])

            if length_MD == length_sim:
                trace_md_x = np.array(df_prev.tadc.trace_ch[0][1])
                trace_md_y = np.array(df_prev.tadc.trace_ch[0][2])
                trace_md_z = np.array(df_prev.tadc.trace_ch[0][3])
            elif length_MD == length_sim // 2:
                trace_md_x_prev = np.array(df_prev.tadc.trace_ch[0][1])
                trace_md_y_prev = np.array(df_prev.tadc.trace_ch[0][2])
                trace_md_z_prev = np.array(df_prev.tadc.trace_ch[0][3])

                df_next = _get_df(fname_next)
                df_next.tadc.get_entry(i_MD_next)
                trace_md_x_next = np.array(df_next.tadc.trace_ch[0][1])
                trace_md_y_next = np.array(df_next.tadc.trace_ch[0][2])
                trace_md_z_next = np.array(df_next.tadc.trace_ch[0][3])

                trace_md_x = np.concatenate((trace_md_x_prev, trace_md_x_next), axis=0)
                trace_md_y = np.concatenate((trace_md_y_prev, trace_md_y_next), axis=0)
                trace_md_z = np.concatenate((trace_md_z_prev, trace_md_z_next), axis=0)
            else:
                raise ValueError(
                    f"Unsupported MD trace length ({length_MD}) for FEB {target_feb_id} with sim length ({length_sim})."
                )

            trace_added[i_du][0] = trace_md_x + file_sim.tadc.trace_ch[i_du][0]
            trace_added[i_du][1] = trace_md_y + file_sim.tadc.trace_ch[i_du][1]
            trace_added[i_du][2] = trace_md_z + file_sim.tadc.trace_ch[i_du][2]

    finally:
        for df in file_cache.values():
            try:
                df.close()
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