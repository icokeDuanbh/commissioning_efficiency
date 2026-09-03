import sys
from pathlib import Path

FLT1_DIR = Path(__file__).resolve().parent
if str(FLT1_DIR) not in sys.path:
    sys.path.insert(0, str(FLT1_DIR))

from build_event_initiator import build_library
import ctypes
from grand.dataio import DataFile
import numpy as np

LIB_PATH = FLT1_DIR / "libevent_initiator.so"


_LIB_INSTANCE = None


def get_library():
    global _LIB_INSTANCE
    if _LIB_INSTANCE is None:
        build_library()
        _LIB_INSTANCE = ctypes.CDLL(str(LIB_PATH))
    return _LIB_INSTANCE


def _as_c_int16_array(trace):
    if trace is None:
        return None
    arr = np.asarray(trace, dtype=np.int16)
    return arr.ctypes.data_as(ctypes.POINTER(ctypes.c_int16))


def call_scope_t2_py(
    trigger_pattern: int,
    t_pre_coincidence_ch2: int,
    t_pre_coincidence_ch3: int,
    t_period_reg_ch2: int,
    t_period_reg_ch3: int,
    custom_trace_ch1=None,
    custom_trace_ch2=None,
    custom_trace_ch3=None,
    custom_trace_ch4=None,
):
    """Call the C++ FLT1 trigger helper through a ctypes bridge.

    This wrapper builds the shared library when needed, passes the given ADC traces
    to the compiled C++ library, and returns the
    trigger results returned by the scope_t2() function in data_format.cpp.

    Parameters:
    -----------
        trigger_pattern: Trigger pattern value used to choose the channel for FLT. (512-ChX, 1024-ChY, Else-Both)
        t_pre_coincidence_ch2: Pre-coincidence window for channel 2, in ns.
        t_pre_coincidence_ch3: Pre-coincidence window for channel 3, in ns.
        t_period_reg_ch2: t_period value for channel 2, in ns.
        t_period_reg_ch3: t_period value for channel 3, in ns.
        custom_trace_ch1: trace values for channel 1 as a NumPy array.
        custom_trace_ch2: trace values for channel 2 as a NumPy array.
        custom_trace_ch3: trace values for channel 3 as a NumPy array.
        custom_trace_ch4: trace values for channel 4 as a NumPy array.

    Returns:
    --------
        ret: the exit code of the C function. 0 means success.
        out0: the correlation score (float) stored in uint_16.
        out1: which channel is used in FLT1. 2 is ChX, 3 is ChY.
        out_size: the size of the event in the buffer
    """
    lib = get_library()

    lib.call_scope_t2_c.argtypes = [
        ctypes.c_uint16,
        ctypes.c_uint16,
        ctypes.c_uint16,
        ctypes.c_uint16,
        ctypes.c_uint16,
        ctypes.POINTER(ctypes.c_int16),
        ctypes.c_int,
        ctypes.POINTER(ctypes.c_int16),
        ctypes.c_int,
        ctypes.POINTER(ctypes.c_int16),
        ctypes.c_int,
        ctypes.POINTER(ctypes.c_int16),
        ctypes.c_int,
        ctypes.POINTER(ctypes.c_uint16),
        ctypes.POINTER(ctypes.c_uint16),
        ctypes.POINTER(ctypes.c_int),
    ]
    lib.call_scope_t2_c.restype = ctypes.c_int

    out0 = ctypes.c_uint16()
    out1 = ctypes.c_uint16()
    out_size = ctypes.c_int()

    trace1_ptr = _as_c_int16_array(custom_trace_ch1)
    trace2_ptr = _as_c_int16_array(custom_trace_ch2)
    trace3_ptr = _as_c_int16_array(custom_trace_ch3)
    trace4_ptr = _as_c_int16_array(custom_trace_ch4)

    len1 = 0 if custom_trace_ch1 is None else len(custom_trace_ch1)
    len2 = 0 if custom_trace_ch2 is None else len(custom_trace_ch2)
    len3 = 0 if custom_trace_ch3 is None else len(custom_trace_ch3)
    len4 = 0 if custom_trace_ch4 is None else len(custom_trace_ch4)

    ret = lib.call_scope_t2_c(
        ctypes.c_uint16(trigger_pattern),
        ctypes.c_uint16(t_pre_coincidence_ch2),
        ctypes.c_uint16(t_pre_coincidence_ch3),
        ctypes.c_uint16(t_period_reg_ch2),
        ctypes.c_uint16(t_period_reg_ch3),
        trace1_ptr, ctypes.c_int(len1),
        trace2_ptr, ctypes.c_int(len2),
        trace3_ptr, ctypes.c_int(len3),
        trace4_ptr, ctypes.c_int(len4),
        ctypes.byref(out0),
        ctypes.byref(out1),
        ctypes.byref(out_size),
    )

    return ret, out0.value, out1.value, out_size.value


if __name__ == "__main__":
    
    fname_sim = "test_data/sim_Dunhuang_20170331_220000_RUN1_CD_GP300-no-noise_0000/adc_0-99_L1_0000.root"
    file_sim = DataFile(fname_sim)
    file_sim.tadc.get_entry(10)
    # TODO: In ADC, it's actually 14 bit
    custom_trace_ch1 = np.array(file_sim.tadc.trace_ch[23][0], dtype=np.int16) # X
    custom_trace_ch2 = np.array(file_sim.tadc.trace_ch[23][0], dtype=np.int16)
    custom_trace_ch3 = np.array(file_sim.tadc.trace_ch[23][1], dtype=np.int16) 
    custom_trace_ch4 = np.array(file_sim.tadc.trace_ch[23][2], dtype=np.int16)
    # custom_trace_ch1 = np.full(1024, 1000, dtype=np.int16)
    # custom_trace_ch2 = np.full(1024, 2000, dtype=np.int16)
    # custom_trace_ch3 = np.full(1024, 3000, dtype=np.int16)
    # custom_trace_ch4 = np.full(1024, 4000, dtype=np.int16)
    
    ret, out0, out1, out_size = call_scope_t2_py(
        1000,
        1036, # In simulation, the pulse is around ADC 430
        1036,
        10,
        10,
        custom_trace_ch1,
        custom_trace_ch2,
        custom_trace_ch3,
        custom_trace_ch4,
    )

    print(f"return_code={ret}")
    print(f"out0={out0}, out1={out1}, out_size={out_size}")
