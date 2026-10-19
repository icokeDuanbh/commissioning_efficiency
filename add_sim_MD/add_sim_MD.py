from pathlib import Path
from grand.dataio import DataFile
import numpy as np
import logging
from .lookup_table import MDLookupTable, load_lookup_table, load_du_to_feb_map

logging.basicConfig(level=logging.INFO)
logging.getLogger('grand.dataio.data_tree').setLevel(logging.CRITICAL)


DEFAULT_MAPPING_FILE = Path(__file__).resolve().parent / "GP65_DUID_FEBID.txt"


def _add_sim_md_du_dependent(
    fname_sim,
    i_sim,
    fname_MD_lookup_table,
    sim_timestamp=None,
    fname_DU_to_FEB=None,
):
    """
    Internal helper: Add DU-dependent MD noise using a lookup table.
    For each DU in the simulated event, looks up the temporally closest MD entry (or 2 entries
    for 512-sample traces) from that DU's recorded background.
    Maps simulated DU position ID to deployed hardware FEB ID via fname_DU_to_FEB (defaults to GP65_DUID_FEBID.txt).
    If no FEB is deployed at that station, or if no MD data exists for that FEB, the trace is masked to zero.
    """
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

            md_info = table.get_md_entries_for_du(target_feb_id, sim_timestamp)
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
                    f"Unsupported MD trace length ({length_MD}) for DU {target_feb_id} with sim length ({length_sim})."
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


def _add_sim_md_legacy(fname_sim, fname_md_prev, i_sim, i_MD_prev, i_MD_next, fname_md_next=None):
    """
    Legacy implementation: Apply a single scalar MD entry uniformly to all DUs in the event.
    """
    if fname_md_next is None:
        fname_md_next = fname_md_prev

    file_sim = DataFile(str(fname_sim))
    file_sim.tadc.get_entry(0)
    length_sim = len(file_sim.tadc.trace_ch[0][0])

    file_MD_prev = DataFile(str(fname_md_prev))
    file_MD_prev.tadc.get_entry(0)
    length_MD = len(file_MD_prev.tadc.trace_ch[0][0])

    logging.debug(f"Checking the trace length in MD ({length_MD}) and simulation ({length_sim}).")

    if length_MD == 1024:
        logging.debug("MD has the same length as the simulation. No need for concatenating.")
        file_MD_prev.tadc.get_entry(i_MD_prev)
        trace_MD_x = np.array(file_MD_prev.tadc.trace_ch[0][1])
        trace_MD_y = np.array(file_MD_prev.tadc.trace_ch[0][2])
        trace_MD_z = np.array(file_MD_prev.tadc.trace_ch[0][3])
    elif length_MD == 512:
        logging.debug("Concatenating two MD into one because MD length is not the same as simulation.")
        file_MD_prev.tadc.get_entry(i_MD_prev)
        trace_MD_x_prev = np.array(file_MD_prev.tadc.trace_ch[0][1])
        trace_MD_y_prev = np.array(file_MD_prev.tadc.trace_ch[0][2])
        trace_MD_z_prev = np.array(file_MD_prev.tadc.trace_ch[0][3])

        if fname_md_next == fname_md_prev:
            file_MD_next = file_MD_prev
        else:
            file_MD_next = DataFile(str(fname_md_next))

        file_MD_next.tadc.get_entry(i_MD_next)
        trace_MD_x_next = np.array(file_MD_next.tadc.trace_ch[0][1])
        trace_MD_y_next = np.array(file_MD_next.tadc.trace_ch[0][2])
        trace_MD_z_next = np.array(file_MD_next.tadc.trace_ch[0][3])

        trace_MD_x = np.concatenate((trace_MD_x_prev, trace_MD_x_next), axis=0)
        trace_MD_y = np.concatenate((trace_MD_y_prev, trace_MD_y_next), axis=0)
        trace_MD_z = np.concatenate((trace_MD_z_prev, trace_MD_z_next), axis=0)

        if fname_md_next != fname_md_prev:
            file_MD_next.close()

    logging.debug("Adding the (concatenating) MD with the simulation.")
    file_sim.tadc.get_entry(i_sim)
    n_du = file_sim.tadc.du_count
    trace_added = np.zeros((n_du, 3, length_sim))
    for i in range(n_du):
        trace_added[i][0] = trace_MD_x + file_sim.tadc.trace_ch[i][0]
        trace_added[i][1] = trace_MD_y + file_sim.tadc.trace_ch[i][1]
        trace_added[i][2] = trace_MD_z + file_sim.tadc.trace_ch[i][2]

    file_sim.close()
    file_MD_prev.close()

    logging.debug("Addition finished.")
    return trace_added, n_du


def add_sim_md(
    fname_sim,
    i_sim,
    fname_MD_lookup_table,
    sim_timestamp=None,
    fname_DU_to_FEB=None,
    # Legacy arguments for backwards compatibility
    i_MD_prev=None,
    i_MD_next=None,
    fname_md_next=None,
):
    """
    Add MD background noise to simulated traces.

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
        Defaults to GP65_DUID_FEBID.txt if available. If a DU has no FEB mapped or
        the FEB has no MD data, its trace is masked to zero.

    Returns:
    --------
    trace_added : numpy.ndarray
        Combined trace array of shape (n_du, 3, length_sim).
    n_du : int
        Number of simulated DUs in this event.
    """
    # Backward compatibility: check if caller used legacy signature:
    # add_sim_md(fname_sim, fname_md_prev, i_sim, i_MD_prev, i_MD_next, fname_md_next)
    if isinstance(i_sim, (str, Path)) and isinstance(fname_MD_lookup_table, (int, np.integer)):
        fname_md_prev = i_sim
        legacy_i_sim = fname_MD_lookup_table
        legacy_i_MD_prev = sim_timestamp
        legacy_i_MD_next = fname_DU_to_FEB if fname_DU_to_FEB is not None else i_MD_prev
        return _add_sim_md_legacy(
            fname_sim,
            fname_md_prev,
            legacy_i_sim,
            legacy_i_MD_prev,
            legacy_i_MD_next,
            fname_md_next=fname_md_next,
        )

    # DU-Dependent Mode
    return _add_sim_md_du_dependent(
        fname_sim=fname_sim,
        i_sim=i_sim,
        fname_MD_lookup_table=fname_MD_lookup_table,
        sim_timestamp=sim_timestamp,
        fname_DU_to_FEB=fname_DU_to_FEB,
    )


if __name__ == "__main__":
    i_sim = 20
    test_file_sim = "test_data/sim_Dunhuang_20170331_220000_RUN1_CD_GP300-no-noise_0000/adc_0-99_L1_0000.root"
    test_file_md = "/Users/xishui/Dropbox/Non_sense/202603_PMO_visit/comms/20260629_1744/data/root_data/GP80_20260629_093930_RUN607_MD_20dB-GP65-60DUs-512trace-20Hz-2min-FY2Float-TEST-DUNHUANG-0001_dat.root"

    # Test legacy uniform mode
    i_MD = 0
    trace_added, n_du = add_sim_md(test_file_sim, test_file_md, i_sim, i_MD, i_MD)
    print(f"[Legacy Mode] {n_du} DUs in event. Shape: {trace_added.shape}")