"""Python wrapper for the SLT T3 trigger pipeline via ctypes + numpy.

Designed for the single-event-per-call use case: each call to ``run()``
represents one time bucket containing at most one physical event.
The return value is simply True (triggered) or False (not triggered).

Usage
-----
    from run_SLT import SLTPipeline
    import numpy as np

    pipeline = SLTPipeline(
        trigger_type=2,
        nhit_threshold=5,
        time_window_ns=15_000,
        causal_window_enabled=True,
        antenna_distances_file="/path/to/detector_distances_May24_v2.txt",
    )

    du_ids         = np.array([103, 109, 1010, 1011, 1012], dtype=np.uint32)
    timestamps     = np.array([1_000_000_000, 1_000_003_000, 1_000_006_000,
                               1_000_009_000, 1_000_012_000], dtype=np.uint64)
    # related_values: out0 from call_scope_t2_py() for each DU hit (FLT1 score).
    # Pass np.zeros(n_hits, dtype=np.uint16) if template_match_filter_enable=False.
    related_values = np.array([score_du0, score_du1, ...], dtype=np.uint16)

    result = pipeline.run(du_ids, timestamps, related_values)
    print(result["triggered"])   # True / False
    print(result["du_ids"])      # post-filter DU IDs (numpy array)
"""
import sys
from pathlib import Path

SLT_DIR  = Path(__file__).resolve().parent
if str(SLT_DIR) not in sys.path:
    sys.path.insert(0, str(SLT_DIR))

from build_slt_pipeline import build_library
import ctypes
import numpy as np

LIB_PATH = SLT_DIR / "libslt_pipeline.so"

_LIB_INSTANCE = None


def _load_library() -> ctypes.CDLL:
    global _LIB_INSTANCE
    if _LIB_INSTANCE is None:
        build_library()
        _LIB_INSTANCE = ctypes.CDLL(str(LIB_PATH))
    return _LIB_INSTANCE


def _ptr(arr, ctype):
    """ctypes pointer to a numpy array, or None if arr is None."""
    if arr is None:
        return None
    return arr.ctypes.data_as(ctypes.POINTER(ctype))


class SLTPipeline:
    """Stateful wrapper around the C++ TriggerPipeline.

    The duplicate-filter history is preserved across calls to ``run()``,
    exactly as in real data-taking.  Destroy and recreate the object to
    reset all internal state.

    Parameters
    ----------
    trigger_type : int
        0 = passthrough, 1 = nhit + T3Filter, 2 = nhit + causal + T3Filter.
    nhit_threshold : int
        Minimum distinct DU count inside the sliding window.
    time_window_ns : int
        Width of the nhit sliding window in nanoseconds.
    duplicate_filter_enable : bool
    duplicate_time_diff_ns : int
    duplicate_min_pair : int
    duplicate_history_size : int
    causal_window_enabled : bool
        Requires trigger_type=2 and a valid antenna_distances_file.
    causal_tolerance_ns : float
    antenna_distances_file : str
        Path to "du_a du_b distance_m" text file.
    template_match_filter_enable : bool
    template_match_coefficient : float
        Threshold for avg(related_value / 65536).
    """

    def __init__(
        self,
        trigger_type: int = 2,
        nhit_threshold: int = 5,
        time_window_ns: int = 15_000,
        duplicate_filter_enable: bool = True,
        duplicate_time_diff_ns: int = 100,
        duplicate_min_pair: int = 2,
        duplicate_history_size: int = 10,
        causal_window_enabled: bool = False,
        causal_tolerance_ns: float = 100.0,
        antenna_distances_file: str = "",
        template_match_filter_enable: bool = False,
        template_match_coefficient: float = 0.0,
    ):
        self._lib = _load_library()
        self._setup_argtypes()

        self._handle = self._lib.slt_pipeline_create(
            ctypes.c_int(trigger_type),
            ctypes.c_int(nhit_threshold),
            ctypes.c_uint64(time_window_ns),
            ctypes.c_int(int(duplicate_filter_enable)),
            ctypes.c_int(duplicate_time_diff_ns),
            ctypes.c_int(duplicate_min_pair),
            ctypes.c_int(duplicate_history_size),
            ctypes.c_int(int(causal_window_enabled)),
            ctypes.c_double(causal_tolerance_ns),
            antenna_distances_file.encode() if antenna_distances_file else b"",
            ctypes.c_int(int(template_match_filter_enable)),
            ctypes.c_double(template_match_coefficient),
        )
        if not self._handle:
            raise RuntimeError("slt_pipeline_create returned nullptr — check config.")

    def run(
        self,
        du_ids,
        timestamps_ns,
        related_values,
        tm_id: int = 0,
        bucket_timestamp_ns: int = 0,
        max_items: int = 256,
    ) -> dict:
        """Run the trigger on one bucket of hits.

        Parameters
        ----------
        du_ids : array-like of uint32
        timestamps_ns : array-like of uint64
        related_values : array-like of uint16
            Waveform template-match scores from FLT1 (``out0`` of
            ``call_scope_t2_py``), stored as uint16 fixed-point values
            (physical score = value / 65536).  One entry per hit, same
            order as du_ids / timestamps_ns.  Pass an array of zeros if
            template_match_filter_enable=False and scores are unavailable.
        tm_id : int
        bucket_timestamp_ns : int
        max_items : int
            Output buffer capacity for per-result vectors.

        Returns
        -------
        dict with keys:
            triggered          bool   — True if nhit met and T3Filter passed
            du_ids             ndarray uint32 — post-filter DU IDs
            trigger_timestamps ndarray uint64 — post-filter timestamps (ns)
            window_du_ids      ndarray uint32 — pre-filter window DU IDs
            window_timestamps  ndarray uint64 — pre-filter window timestamps (ns)
        """
        du_arr = np.asarray(du_ids,        dtype=np.uint32)
        ts_arr = np.asarray(timestamps_ns,  dtype=np.uint64)
        rv_arr = np.asarray(related_values, dtype=np.uint16)

        if not (len(du_arr) == len(ts_arr) == len(rv_arr)):
            raise ValueError(
                f"Array length mismatch: du_ids ({len(du_arr)}), "
                f"timestamps_ns ({len(ts_arr)}), related_values ({len(rv_arr)}) "
                "must all have the same length."
            )

        I = max_items
        du_count    = ctypes.c_int(0)
        wdu_count   = ctypes.c_int(0)
        wts_count   = ctypes.c_int(0)
        du_ids_out  = np.zeros(I, dtype=np.uint32)
        trig_ts_out = np.zeros(I, dtype=np.uint64)
        wdu_out     = np.zeros(I, dtype=np.uint32)
        wts_out     = np.zeros(I, dtype=np.uint64)

        ret = self._lib.slt_pipeline_run(
            self._handle,
            _ptr(du_arr,  ctypes.c_uint32),
            _ptr(ts_arr,  ctypes.c_uint64),
            _ptr(rv_arr,  ctypes.c_uint16),
            ctypes.c_int(len(du_arr)),
            ctypes.c_uint64(tm_id),
            ctypes.c_uint64(bucket_timestamp_ns),
            ctypes.c_int(I),
            ctypes.byref(du_count),
            _ptr(du_ids_out,  ctypes.c_uint32),
            _ptr(trig_ts_out, ctypes.c_uint64),
            ctypes.byref(wdu_count),
            _ptr(wdu_out,     ctypes.c_uint32),
            ctypes.byref(wts_count),
            _ptr(wts_out,     ctypes.c_uint64),
        )

        if ret == -1:
            raise RuntimeError("slt_pipeline_run: bad arguments passed to C++")

        dc  = du_count.value
        wdc = wdu_count.value
        wtc = wts_count.value
        return {
            "triggered":          ret == 1,
            "du_ids":             du_ids_out[:dc].copy(),
            "trigger_timestamps": trig_ts_out[:dc].copy(),
            "window_du_ids":      wdu_out[:wdc].copy(),
            "window_timestamps":  wts_out[:wtc].copy(),
        }

    def __del__(self):
        if getattr(self, "_handle", None) and getattr(self, "_lib", None):
            self._lib.slt_pipeline_destroy(self._handle)
            self._handle = None

    def _setup_argtypes(self):
        lib = self._lib

        lib.slt_pipeline_create.restype  = ctypes.c_void_p
        lib.slt_pipeline_create.argtypes = [
            ctypes.c_int,    # trigger_type
            ctypes.c_int,    # nhit_threshold
            ctypes.c_uint64, # time_window_ns
            ctypes.c_int,    # duplicate_filter_enable
            ctypes.c_int,    # duplicate_time_diff_ns
            ctypes.c_int,    # duplicate_min_pair
            ctypes.c_int,    # duplicate_history_size
            ctypes.c_int,    # causal_window_enabled
            ctypes.c_double, # causal_tolerance_ns
            ctypes.c_char_p, # antenna_distances_file
            ctypes.c_int,    # template_match_filter_enable
            ctypes.c_double, # template_match_coefficient
        ]

        lib.slt_pipeline_destroy.restype  = None
        lib.slt_pipeline_destroy.argtypes = [ctypes.c_void_p]

        lib.slt_pipeline_run.restype  = ctypes.c_int
        lib.slt_pipeline_run.argtypes = [
            ctypes.c_void_p,                   # handle
            ctypes.POINTER(ctypes.c_uint32),   # du_ids
            ctypes.POINTER(ctypes.c_uint64),   # timestamps_ns
            ctypes.POINTER(ctypes.c_uint16),   # related_values (nullable)
            ctypes.c_int,                      # n_hits
            ctypes.c_uint64,                   # tm_id
            ctypes.c_uint64,                   # bucket_timestamp_ns
            ctypes.c_int,                      # max_items
            ctypes.POINTER(ctypes.c_int),      # out_du_count
            ctypes.POINTER(ctypes.c_uint32),   # out_du_ids
            ctypes.POINTER(ctypes.c_uint64),   # out_trigger_ts
            ctypes.POINTER(ctypes.c_int),      # out_window_du_count
            ctypes.POINTER(ctypes.c_uint32),   # out_window_du_ids
            ctypes.POINTER(ctypes.c_int),      # out_window_ts_count
            ctypes.POINTER(ctypes.c_uint64),   # out_window_ts
        ]


# ---------------------------------------------------------------------------
# Smoke test
# ---------------------------------------------------------------------------
if __name__ == "__main__":
    DISTANCES = str(
        SLT_DIR.parent /
        "/Users/xishui/Dropbox/Project/GRAND/Event_injector/grand_daq/grand-daq/cfgs/detector_distances_May24_v2.txt"
    )

    pipeline = SLTPipeline(
        trigger_type=1,
        nhit_threshold=5,
        time_window_ns=15_000,
        # Causality cut on the time difference
        causal_window_enabled=True,
        antenna_distances_file=DISTANCES,
        # NUTRIG template fitting cut
        template_match_filter_enable=True,
        template_match_coefficient=0.0,
        # "The fingerprint cut"
        duplicate_filter_enable=True,
        duplicate_history_size=20,
        duplicate_min_pair=2,
        duplicate_time_diff_ns=100,
    )

    du_ids         = np.array([103, 109, 1010, 1011, 1012], dtype=np.uint32)
    timestamps     = np.array([1_000_000_000, 1_000_003_000, 1_000_006_000,
                               1_000_009_000, 1_000_012_000], dtype=np.uint64)
    # related_values: out0 from call_scope_t2_py() for each DU hit.
    # Pass np.zeros(n_hits, dtype=np.uint16) if template_match_filter_enable=False.
    related_values = 65535 * np.ones(du_ids.size, dtype=np.uint16) # The (float) correlation is stored as a uint16.

    result = pipeline.run(du_ids, timestamps, related_values, bucket_timestamp_ns=10000)
    print(result["triggered"])   # True / False
    print(result["du_ids"])      # post-filter DU IDs (numpy array)

    print("=== Run 1: fresh event ===")
    # Add some out-of-window 1012s to test the sliding window
    # Change the DU id to pass the duplicate filter
    du_ids1 = np.array([1012, 1012, 1044, 1045, 1046, 1047, 1012, 1012, 1012, 1012,], dtype=np.uint32)
    timestamps1 = np.array([800_000, 900_000, 1_000_000_000, 1_000_003_000, 1_000_006_000,
                               1_000_009_000, 1_000_012_000, 2_000_012_000, 3_000_012_000, 4_000_012_000], dtype=np.uint64)
    related_values1 = 65535 * np.ones(du_ids1.size, dtype=np.uint16)
    r = pipeline.run(du_ids1, timestamps1, related_values1, tm_id=1000,
                     bucket_timestamp_ns=1_000_000_000)
    print(f"  triggered={r['triggered']}  du_ids={r['du_ids']}")

    print("=== Run 2: same hits → duplicate filter ===")
    r2 = pipeline.run(du_ids1, timestamps1, related_values1, tm_id=1001,
                      bucket_timestamp_ns=2_000_000_000)
    print(f"  triggered={r2['triggered']}  du_ids={r2['du_ids']}")
