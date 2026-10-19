#!/usr/bin/env python3
"""
Lookup table management and query module for DU-dependent MD noise.

Provides utilities to:
1. Load a pre-generated lookup table from disk into memory.
2. Query the lookup table for the closest MD entry (or 2 entries for 512-sample traces)
   matching a target DU ID and timestamp.
"""

from pathlib import Path
from typing import Dict, List, Optional, Tuple, Union
import numpy as np
import pandas as pd
import logging

logger = logging.getLogger(__name__)


class MDLookupTable:
    """
    Indexed in-memory structure for fast per-FEB timestamp proximity queries.
    """

    def __init__(self, df: pd.DataFrame):
        """
        Initialize the lookup table from a DataFrame.
        Expected columns: ['feb_id', 'timestamp_sec', 'file_path', 'entry_index', 'trace_length']
        """
        # If older table uses 'du_id', alias it to 'feb_id'
        if 'du_id' in df.columns and 'feb_id' not in df.columns:
            df = df.rename(columns={'du_id': 'feb_id'})

        required_cols = {'feb_id', 'timestamp_sec', 'file_path', 'entry_index', 'trace_length'}
        missing = required_cols - set(df.columns)
        if missing:
            raise ValueError(f"Lookup table missing required columns: {missing}")

        self.df = df
        # Build per-FEB index: {feb_id: {'timestamps': np.ndarray, 'records': list}}
        self._feb_cache: Dict[int, Dict[str, Union[np.ndarray, List[dict]]]] = {}
        self._build_index()

    def _build_index(self):
        grouped = self.df.groupby('feb_id')
        for feb_id, group in grouped:
            sorted_group = group.sort_values('timestamp_sec')
            timestamps = sorted_group['timestamp_sec'].to_numpy(dtype=np.float64)
            records = sorted_group[['file_path', 'entry_index', 'trace_length']].to_dict('records')
            self._feb_cache[int(feb_id)] = {
                'timestamps': timestamps,
                'records': records,
            }

    @property
    def available_feb_ids(self) -> List[int]:
        """Return list of all FEB IDs present in the lookup table."""
        return list(self._feb_cache.keys())

    @property
    def available_du_ids(self) -> List[int]:
        """Alias for available_feb_ids for backward compatibility."""
        return self.available_feb_ids

    def get_md_entries_for_feb(
        self,
        feb_id: int,
        target_timestamp: float,
    ) -> Optional[Tuple[str, int, Optional[str], Optional[int]]]:
        """
        Find the MD entry (or 2 entries for 512 samples) closest in time to target_timestamp for feb_id.

        Parameters:
            feb_id: int
                Hardware FEB ID to search for.
            target_timestamp: float
                Target timestamp in seconds (typically from the simulation event).

        Returns:
            (fname_prev, i_prev, fname_next, i_next) if records found, or None if feb_id is not in table.
            - For 1024-sample traces: fname_next and i_next will be None.
            - For 512-sample traces: (fname_prev, i_prev) and (fname_next, i_next) will be
              the two entries closest in time, ordered chronologically.
        """
        feb_id = int(feb_id)
        if feb_id not in self._feb_cache:
            return None

        entry = self._feb_cache[feb_id]
        timestamps = entry['timestamps']
        records = entry['records']

        if len(timestamps) == 0:
            return None

        trace_length = records[0]['trace_length']
        abs_diff = np.abs(timestamps - float(target_timestamp))

        if trace_length == 1024:
            closest_idx = int(np.argmin(abs_diff))
            rec = records[closest_idx]
            return rec['file_path'], rec['entry_index'], None, None

        elif trace_length == 512:
            if len(timestamps) < 2:
                logger.warning(
                    f"DU {du_id} has trace_length=512 but only {len(timestamps)} entry in table. "
                    "Cannot concatenate 2 traces; returning None."
                )
                return None

            # Get the two indices with minimum absolute time difference
            closest_two = np.argsort(abs_diff)[:2]
            idx1, idx2 = int(closest_two[0]), int(closest_two[1])

            # Order them chronologically (earlier in time is prev, later is next)
            if timestamps[idx1] > timestamps[idx2]:
                idx1, idx2 = idx2, idx1

            rec_prev = records[idx1]
            rec_next = records[idx2]
            return rec_prev['file_path'], rec_prev['entry_index'], rec_next['file_path'], rec_next['entry_index']

        else:
            raise ValueError(f"Unsupported trace_length {trace_length} for FEB {feb_id}. Expected 512 or 1024.")

    def get_md_entries_for_du(
        self,
        du_id: int,
        target_timestamp: float,
    ) -> Optional[Tuple[str, int, Optional[str], Optional[int]]]:
        """Alias for get_md_entries_for_feb for backward compatibility."""
        return self.get_md_entries_for_feb(du_id, target_timestamp)


def load_lookup_table(table_path: Union[str, Path, pd.DataFrame, MDLookupTable]) -> MDLookupTable:
    """
    Load a lookup table from disk or wrap an existing DataFrame/MDLookupTable.

    Supported formats:
    - .csv
    - .parquet
    - .pkl / .pickle
    - pandas DataFrame
    - MDLookupTable instance
    """
    if isinstance(table_path, MDLookupTable):
        return table_path

    if isinstance(table_path, pd.DataFrame):
        return MDLookupTable(table_path)

    path = Path(table_path)
    if not path.exists():
        raise FileNotFoundError(f"Lookup table file not found: {path}")

    suffix = path.suffix.lower()
    if suffix == '.csv':
        df = pd.read_csv(path)
    elif suffix == '.parquet':
        df = pd.read_parquet(path)
    elif suffix in ('.pkl', '.pickle'):
        df = pd.read_pickle(path)
    else:
        # Fall back to trying CSV
        try:
            df = pd.read_csv(path, sep=r'\s+|,', engine='python')
        except Exception as e:
            raise ValueError(f"Unsupported lookup table format '{suffix}': {e}")

    return MDLookupTable(df)


def load_du_to_feb_map(map_source: Optional[Union[dict, str, Path]]) -> Optional[Dict[int, int]]:
    """
    Load a DU-to-FEB mapping dictionary from a dict or file.

    Supported file formats:
    - JSON: {"65001": 1032, ...}
    - CSV / TSV / whitespace-separated text: '650XX  feb_id' per line (e.g. GP65_DUID_FEBID.txt)

    Returns:
        Dict[int, int] mapping simulated DU ID -> hardware FEB ID.
        Dual-indexes both 650XX (e.g. 65001) and short station number (e.g. 1).
    """
    if map_source is None:
        return None

    if isinstance(map_source, dict):
        base_map = {int(k): int(v) for k, v in map_source.items()}
    else:
        path = Path(map_source)
        if not path.exists():
            raise FileNotFoundError(f"DU-to-FEB map file not found: {path}")

        suffix = path.suffix.lower()
        if suffix == '.json':
            import json
            with open(path, 'r') as f:
                raw = json.load(f)
            base_map = {int(k): int(v) for k, v in raw.items()}
        else:
            # Read whitespace- or comma-separated file without assuming headers
            df = pd.read_csv(path, sep=r'\s+|,', header=None, engine='python', comment='#')
            # Check if row 0 was a header
            first_val = str(df.iloc[0, 0]).strip()
            if not first_val.isdigit():
                df = df.iloc[1:].reset_index(drop=True)

            if len(df.columns) < 2:
                raise ValueError(f"DU-to-FEB map file must contain at least 2 columns: {path}")

            col_du, col_feb = df.columns[0], df.columns[1]
            base_map = dict(zip(df[col_du].astype(int), df[col_feb].astype(int)))

    # Dual-index: map both 650XX and short station index XX
    mapping: Dict[int, int] = {}
    for du_key, feb_val in base_map.items():
        mapping[du_key] = feb_val
        # For GP65 convention: 65001 -> 1, 65078 -> 78, 65116 -> 116
        if 65000 <= du_key < 66000:
            short_id = du_key - 65000
            mapping[short_id] = feb_val
        elif du_key < 1000:
            long_id = 65000 + du_key
            mapping[long_id] = feb_val

    return mapping
