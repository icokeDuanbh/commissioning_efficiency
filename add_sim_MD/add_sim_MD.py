from grand.dataio import DataFile
import numpy as np
import logging
logging.basicConfig(level=logging.INFO)
# logging.getLogger('grand.dataio.data_tree').setLevel(logging.CRITICAL)


def add_sim_md(fname_sim, fname_md_prev, i_sim, i_MD_prev, i_MD_next, fname_md_next=None):
  """Add an MD trace to a simulated trace and return the combined waveform and du_count.

  Parameters:
    fname_sim: str
      Path to the simulation ROOT file containing the simulated TADC traces.
    fname_md_prev: str
      Path to the MD ROOT file for the first trace segment (i_MD_prev).
    i_sim: int
      Index of the simulation event to use from the simulation file.
    i_MD_prev: int
      Index of the first half MD event trace.
    i_MD_next: int
      Index of the second half MD event trace.
    fname_md_next: str, optional
      Path to the MD ROOT file for the second trace segment (i_MD_next).
      If None, defaults to fname_md_prev (same file).

  Returns:
    trace_added: numpy.ndarray
      Combined trace array with shape (n_du, 3, length_sim) for the added traces.
    n_du: int
      Number of simulated DUs of this event.
  """
  if fname_md_next is None:
    fname_md_next = fname_md_prev

  file_sim = DataFile(fname_sim)
  file_sim.tadc.get_entry(0)
  length_sim = len(file_sim.tadc.trace_ch[0][0])

  file_MD_prev = DataFile(fname_md_prev)
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
      file_MD_next = DataFile(fname_md_next)

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

  # Close opened DataFiles to prevent ROOT memory leaks
  file_sim.close()
  file_MD_prev.close()

  logging.debug("Addition finished.")
  return trace_added, n_du

if __name__ == "__main__":
  i_sim = 20 # which sim to be added with MD
  i_MD = 0 # which MD to be added with sim
  test_file_sim = "test_data/sim_Dunhuang_20170331_220000_RUN1_CD_GP300-no-noise_0000/adc_0-99_L1_0000.root"
  test_file_md = "/Users/xishui/Dropbox/Non_sense/202603_PMO_visit/comms/20260629_1744/data/root_data/GP80_20260629_093930_RUN607_MD_20dB-GP65-60DUs-512trace-20Hz-2min-FY2Float-TEST-DUNHUANG-0001_dat.root"
  trace_added, n_du = add_sim_md(test_file_sim, test_file_md, i_sim, i_MD)
  print(f"{n_du} DUs in this event.")
  print(f"The returned array has shape {trace_added.shape}.")