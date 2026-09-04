import csv
import glob as gb
import numpy as np
from scipy.signal import lfilter
from scipy.signal.windows import blackman
from scipy import integrate
from offline_FLT0_trigger import trigger_FLT0



def filter_traces_bandpass(traces,
                           coeff_file='./lowpass115MHz.txt'):
    '''
    Mimics the DIRECT form FIR band-pass filter < 115 MHz that is implemented on the online firmware.
    Filter coefficients are provided by Xing Xu.
    Implemented with help of ChatGPT. See also `./test_bandpass_filter.ipynb`.

    Arguments
    ---------
    - `traces`
        + type        : `np.ndarray[int]`
        + units       : ADC counts
        + description : Array of ADC traces, with shape `(N_traces,N_channels,N_samples)`.

    - `coeff_file`
        + type        : str
        + description : File containing the filter coefficients.
                                
    Returns
    -------
    - `traces_filtered`
        + type        : `np.ndarray[float]`
        + units       : "ADC counts"
        + description : Array of filtered ADC traces, with shape `(N_traces,N_channels,N_samples)`.
    '''

    coeff           = np.loadtxt(coeff_file,delimiter=',')
    traces_filtered = lfilter(coeff,1,traces)

    return traces_filtered.astype(int)



def filter_traces_bandpass_all_channels(trace_all_chnls,
                                        coeff_file='./lowpass115MHz.txt'):

    '''
    Apply a bandpass filter to all channels of the DU of interest
    Input parameters:
    trace_all_chnls (ndarray[n_channel][n_sample]): ADC time traces of all channels of the DU of interest
    coeff_file (Hz): coefficients of the filter
    '''
    return np.apply_along_axis(
        filter_traces_bandpass,
        axis=1,
        arr=trace_all_chnls,
        coeff_file=coeff_file
    )



def filter_traces_bandpass_all_dus(trace_all_dus,
                                   coeff_file='./lowpass115MHz.txt'):

    '''
    Apply a bandpass filter to all DUs including all channels
    Input parameters:
    trace_all_dus (ndarray[n_du][n_channel][n_sample]): ADC time traces of all DUs including all channels
    coeff_file (Hz): coefficients of the filter
    '''

    n_du, n_channel, n_sample = trace_all_dus.shape
    reshaped = np.reshape(trace_all_dus, [-1, n_sample])
    
    filtered = np.apply_along_axis(
        filter_traces_bandpass,
        axis=1,
        arr=reshaped,
        coeff_file=coeff_file
    )

    return filtered.reshape(n_du, n_channel, n_sample)



def notch_filter(trace, f_notch, r, f_sample):

    '''
    Apply notch filter with your desired notch frequency.

    Input parameters:
    trace (ndarray[n_sample]): Time trace of ADC counts
    f_notch  (Hz): Notch frequency
    r: r parameter of the notch filter
    f_sample (Hz): Sampling frequency (usually 500 MHz = 500e6 Hz)

    Here you have some examples of notch parameters:
    # Filter 1 parameters:
        f_notch = 39e6
        r = 0.90
    # Filter 2 parameters:
        f_notch = 119.4e6
        r = 0.94
    # Filter 3 parameters:
        f_notch = 132e6
        r = 0.95
    # Filter 4 parameters:
        f_notch = 137.8e6
        r = 0.98
    '''

    nu = 2. * np.pi * f_notch / f_sample

    ### Calculation of coefficients
    a1 = 2. * (r ** 4) * np.cos(4.*nu)
    a2 = - (r ** 8)
    b1 = - 2. * np.cos(nu)
    b2 = 1
    b3 = 2. * r * np.cos(nu)
    b4 = r * r
    b5 = 2. * r * r * np.cos(2.*nu)
    b6 = r ** 4

    ### Calculation of the trace after passing the digital notch filter
    ### Parameters:
    ### y[n_sample]: output trace
    ### y1[n_sample] & y2[n_sample]: intermediate variables
    y, y1, y2 = np.zeros(trace.shape[0]), np.zeros(trace.shape[0]), np.zeros(trace.shape[0])
    
    for n in range(trace.shape[0]):
        y1[n] = b2 * trace[n] + b1 * trace[n-1] + trace[n-2]
        y2[n] = y1[n] + b3 * y1[n-1] + b4 * y1[n-2]
        #y[n]  = a1 * y[n-4] + a2 * y[n-8] + y2[n-2] + b5 * y2[n-4] + b6 * y2[n-6]
        y[n]  = (int)(a1 * y[n-4] + a2 * y[n-8] + y2[n-2] + b5 * y2[n-4] + b6 * y2[n-6])

    return y



def notch_filter_all_channels(trace_all_chnls, f_notch, r, f_sample):

    '''
    Apply a notch filter to all channels of the DU of interest

    Input parameters:
    trace_all_chnls (ndarray[n_channel][n_sample]): ADC time traces of all channels of the DU of interest
    f_notch  (Hz): Notch frequency
    r: r parameter of the notch filter
    f_sample (Hz): Sampling frequency (usually 500 MHz = 500e6 Hz)
    '''

    return np.apply_along_axis(
        notch_filter,
        axis=1,
        arr=trace_all_chnls,
        f_notch=f_notch,
        r=r,
        f_sample=f_sample
    )



def notch_filter_all_dus(trace_all_dus, f_notch, r, f_sample):

    '''
    Apply a notch filter to all DUs including all channels

    Input parameters:
    trace_all_dus (ndarray[n_du][n_channel][n_sample]): ADC time traces of all DUs including all channels
    f_notch  (Hz): Notch frequency
    r: r parameter of the notch filter
    f_sample (Hz): Sampling frequency (usually 500 MHz = 500e6 Hz)
    '''

    n_du, n_channel, n_sample = trace_all_dus.shape
    reshape = np.reshape(trace_all_dus, [-1, n_sample])

    filtered = np.apply_along_axis(
        notch_filter,
        axis=1,
        arr=reshape,
        f_notch=f_notch,
        r=r,
        f_sample=f_sample
    )

    return filtered.reshape(n_du, n_channel, n_sample)



def calculate_relative_trace_start_time(tshower_l0, tadc_l1, t_res_ns):

    ### Calculate the relative start timing of each trace (the same for all channels of each DU)
    ### The unit is nanoseconds
    ### The value can be minus.

    ### Inputs:
    ### tshower_l0: tshower L0 object
    ### tadc_l1: tadc L1 object
    ### t_res_ns (ns): time resolution of ADC sampling
    ### (= 1 / fs, where fs is a sampling frequency)

    core_time_s, core_time_ns = tshower_l0.core_time_s, tshower_l0.core_time_ns
    du_s, du_ns = np.array(tadc_l1.du_seconds), np.array(tadc_l1.du_nanoseconds)
    trig_pos_ns = np.array(tadc_l1.trigger_position) * t_res_ns
    rel_trig_time_ns = ((du_s - core_time_s) * 1.e9 + (du_ns - core_time_ns)).astype(int)
    rel_trace_start_time_ns = (rel_trig_time_ns - trig_pos_ns).astype(int)

    return rel_trace_start_time_ns



def discuss_coincidence(trig_du_list, any_du, time_window, channels):

    ### Discuss a coincidence between DUs to judge a detection of an air shower event
    ### A condition to claim a coincidence is
    ### any any_du DUs or more within a time window of t_window nanoseconds
    
    ### Inputs:
    ### trig_du_list: list of the relative trigger timing information
    ### components: DU ID (du_id) & (t_trig, ns) & arm ('X', 'Y', 'Z')
    ### = [[du_id_0, t_trig_0, arm_0], [du_id_1, t_trig_1, arm_1], ..., [du_id_N, t_trig_N, arm_N]]
    ### any_du: minimum number of DUs required to issue a trigger
    ### time_window (ns): time window to discuss a coincidence
    ### channels: list of the channels used to dicsuss a trigger, such as
    ### = ['X', 'Y'], or ['X', 'Y', 'Z'], etc.
    
    ### Return:
    ### arry_trig: trigger flag, 0 = NOT triggered & 1 = successfully triggered
    ### arry_trig_du_list: list of [du_id, t_trig, arm]
    ### of the DUs which issued the trigger
    
    arry_trig, arry_trig_du_list = 0, []

    new_trig_du_list = [x for x in trig_du_list if x[2] in channels]
    
    if len(new_trig_du_list) >= any_du:

        new_trig_du_list.sort() # sort w/ t_trig in an ascending order

        for i, n in enumerate(new_trig_du_list):
            trig_time_list = [n]

            for m in new_trig_du_list[i:]:
                there_is_same_du = False

                for l in trig_time_list:
                    if l[0] == m[0]:
                        there_is_same_du = True
                        break
                    
                if there_is_same_du: continue
                else: trig_time_list.append(m)

                if len(trig_time_list) == any_du: break

            if len(trig_time_list) < any_du: continue
            else: # len() == any_du
                if trig_time_list[-1][1] - trig_time_list[0][1] < time_window:
                    arry_trig = 1
                    arry_trig_du_list = trig_time_list
                    break
                else: continue

    return arry_trig, arry_trig_du_list



def calculate_PAO_spectrum(e_eV):

    ### Calculate the CR flux measured by PAO in 10 ** 17 eV < E < 10 ** 20 eV
    ### with the SD-750 and SD-1500.
    ### The formula of the spectrum is Equation (13) of
    ### Abreu et al., Eur. Phys. J. C 81, 966 (2021)
    ### (https://doi.org/10.1140/epjc/s10052-021-09700-w)

    j_0 = 1.309e-18 # km^-2 yr^-1 sr^-1 eV^-1
    e_0 = 10 ** 18.5 # eV
    omega_01, omega_12, omega_23, omega_34 = 0.43, 0.05, 0.05, 0.05
    g_0, g_1, g_2, g_3, g_4 = 2.64, 3.298, 2.52, 3.08, 5.2
    e_01, e_12, e_23, e_34 = 1.24e17, 4.9e18, 1.4e19, 4.7e19 # eV

    j = j_0 * (e_eV/e_0) ** (-g_0) \
            * (1 + (e_eV/e_01) ** (1/omega_01)) ** ((g_0-g_1) * omega_01) \
            * (1 + (e_eV/e_12) ** (1/omega_12)) ** ((g_1-g_2) * omega_12) \
            * (1 + (e_eV/e_23) ** (1/omega_23)) ** ((g_2-g_3) * omega_23) \
            * (1 + (e_eV/e_34) ** (1/omega_34)) ** ((g_3-g_4) * omega_34) \
            / (1 + (e_0/e_01)  ** (1/omega_01)) ** ((g_0-g_1) * omega_01) \
            / (1 + (e_0/e_12)  ** (1/omega_12)) ** ((g_1-g_2) * omega_12) \
            / (1 + (e_0/e_23)  ** (1/omega_23)) ** ((g_2-g_3) * omega_23) \
            / (1 + (e_0/e_34)  ** (1/omega_34)) ** ((g_3-g_4) * omega_34)

    return j



def calculate_weighting_factor_energy_PAO_spectrum(e_eV, emin_eV, emax_eV):

    w_uni = 1 / e_eV / np.log(emax_eV/emin_eV)

    coeff, _ = integrate.quad(calculate_PAO_spectrum, emin_eV, emax_eV)
    w = calculate_PAO_spectrum(e_eV) / coeff

    return w / w_uni



def has_duplicates(seq):

    ### Judge if a list of list objects the same list objects ###
    ### For eaxmple,
    ### l1 = [[200, 'X'], [256, 'Y'], [300, 'X'], [120, 'Y']] -> 0 (False)
    ### l2 = [[200, 'X'], [256, 'Y'], [200, 'X'], [120, 'Y']] -> 1 (True)
    ### l3 = [[200, 'X'], [200, 'X'], [200, 'X'], [120, 'Y']] -> 1
    ### l4 = [[200, 'X'], [256, 'Y'], [200, 'X'], [256, 'Y']] -> 1
    ### etc.
    
    seen = []
    unique_list = [x for x in seq if x not in seen and not seen.append(x)]
    return int(len(seq) != len(unique_list))



def get_FLT0_trigger_parameters(FLT0_trig_params_file):

    ### Read trigger parameters for the FLT0 trigger
    ### Please refer to, e.g., dict_trig_params_fir.csv for the format
    
    dict_trig_params = csv.DictReader(open(FLT0_trig_params_file))
    for row in dict_trig_params: trigger_parameters = row
    trigger_parameters = {k: int(v) for k, v in trigger_parameters.items()}

    return trigger_parameters



def get_FLT0_trigger_time(trace, FLT0_trig_params, rel_trace_start_time_ns, t_res_ns):

    ### Return the relative FLT0 trigger time(s) of a channel of the DU of interest

    ### Input parameters:
    ### trace (ndarray[n_sample]): ADC trace
    ### FLT0_trig_params: dictionary of the FLT0 trigger parameters
    ###                   please refer to, e.g., dict_trig_params_fir.csv for the format
    ### rel_trace_start_time_ns (ns): relative start timing of the trace (the same for all channels)
    ### t_res_ns (ns): time resolution of ADC sampling
    ### (= 1 / fs, where fs is a sampling frequency)

    ### Return:
    ### a list of the relative FLT0 trigger time(s) (ns)

    rel_FLT0_trig_time_ns = []

    T1_idxs, T1_amps, NC_vals = trigger_FLT0(trace, FLT0_trig_params)
    
    for n in range(len(T1_idxs)):
        if NC_vals[n] >= FLT0_trig_params['nc_min'] and NC_vals[n] <= FLT0_trig_params['nc_max']:
            rel_FLT0_trig_time_ns.append(rel_trace_start_time_ns + T1_idxs[n] * t_res_ns)

    return rel_FLT0_trig_time_ns



def get_FLT0_trigger_time_all_channels(trace_all_chnls, FLT0_trig_params, rel_trace_start_time_ns, t_res_ns):

    ### Return the relative FLT0 trigger time(s) of all channels of the DU of interest

    ### Input parameters:
    ### trace (ndarray[n_channel][n_sample]): ADC trace of all channels of the DU of interest
    ### FLT0_trig_params: dictionary of the FLT0 trigger parameters
    ###                   please refer to, e.g., dict_trig_params_fir.csv for the format
    ### rel_trace_start_time_ns (ns): relative start timing of the trace (the same for all channels)
    ### t_res_ns (ns): time resolution of ADC sampling
    ### (= 1 / fs, where fs is a sampling frequency)

    ### Return:
    ### a list of the relative FLT0 trigger time(s) (ns) and the channel(s),
    ### i.e., [[trig_time_1, 'X'], [trig_time_2, 'Y'], ..., [trig_time_N, 'Z']]

    FLT0_trig_list_X, FLT0_trig_list_Y, FLT0_trig_list_Z = [], [], []
    n_channel = trace_all_chnls.shape[0]

    for n in range(n_channel):
        rel_FLT0_trig_time_ns = get_FLT0_trigger_time(trace_all_chnls[n], FLT0_trig_params,
                                                      rel_trace_start_time_ns, t_res_ns)
        if   n == 0: FLT0_trig_list_X = [[t, 'X'] for t in rel_FLT0_trig_time_ns]
        elif n == 1: FLT0_trig_list_Y = [[t, 'Y'] for t in rel_FLT0_trig_time_ns]
        elif n == 2: FLT0_trig_list_Z = [[t, 'Z'] for t in rel_FLT0_trig_time_ns]

    return FLT0_trig_list_X + FLT0_trig_list_Y + FLT0_trig_list_Z



def get_FLT0_trigger_time_all_dus(du_id, trace_all_dus, FLT0_trig_params, rel_trace_start_time_ns, t_res_ns):

    ### Return the relative FLT0 trigger time(s) of all DUs including all channels

    ### Input parameters:
    ### du_id (ndarray[n_du]): DU ID
    ### trace (ndarray[n_du][n_channel][n_sample]): ADC trace of all DUs including all channels
    ### FLT0_trig_params: dictionary of the FLT0 trigger parameters
    ###                   please refer to, e.g., dict_trig_params_fir.csv for the format
    ### rel_trace_start_time_ns(ndarray[n_du]) (ns): relative start timing of the trace (the same for all channels)
    ### t_res_ns (ns): time resolution of ADC sampling
    ### (= 1 / fs, where fs is a sampling frequency)

    ### Return:
    ### list of DU ID, the relative FLT0 trigger time(s) (ns) and the channel(s),
    ### i.e., [[DU_id_1, trig_time_1, 'X'], [DU_id_2, trig_time_2, 'Y'], ..., [DU_id_N, trig_time_N, 'Z']]

    FLT0_trig_list = []
    n_du = trace_all_dus.shape[0]

    for n in range(n_du):
        rel_FLT0_trig_list_du = get_FLT0_trigger_time_all_channels(trace_all_dus[n], FLT0_trig_params,
                                                                   rel_trace_start_time_ns[n], t_res_ns)
        FLT0_trig_list += [[du_id[n], *x] for x in rel_FLT0_trig_list_du if len(x)]
        
    return FLT0_trig_list



def read_data_trigger_information(files):

    data = np.array([[-1, -1, -1, -1, -1, -1, -1, -1, -1, -1]])

    for file in gb.glob(files):
        data = np.concatenate([data, np.loadtxt(file)], axis=0)
        
    data = np.delete(data, 0, axis=0)

    dict_data = {
        'run':  data[:,0].astype(int),
        'eve':  data[:,1].astype(int),
        'pid':  data[:,2].astype(int),
        'e_eV': (data[:,3] * 1.e9).astype(float),
        'zen':  data[:,4].astype(float),
        'azi':  data[:,5].astype(float),
        'corex': (data[:,6] * 1.e3).astype(float),
        'corey': (data[:,7] * 1.e3).astype(float),
        'corez': (data[:,8] * 1.e3).astype(float),
        'trig':  data[:,9].astype(int)
    }

    return dict_data
