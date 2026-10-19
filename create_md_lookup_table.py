#!/usr/bin/env python3
"""
Scan MD ROOT files and generate a master lookup table mapping DU IDs and timestamps
to ROOT file paths and entry indices.

Output columns:
- feb_id: int
- timestamp_sec: float (exact second + nanosecond / 1e9)
- file_path: str
- entry_index: int
- trace_length: int (e.g. 512 or 1024)
"""

import sys
import argparse
import logging
from pathlib import Path
import pandas as pd
from grand.dataio import DataFile

logging.getLogger('grand.dataio.data_tree').setLevel(logging.CRITICAL)

ROOT = Path(__file__).resolve().parent


def build_md_lookup_dataframe(md_paths: list) -> pd.DataFrame:
    """
    Scan a list of MD ROOT files and return a pandas DataFrame of all entries.
    """
    records = []

    for file_path in md_paths:
        file_path = Path(file_path).resolve()
        if not file_path.exists():
            print(f"Warning: File {file_path} does not exist, skipping.")
            continue

        try:
            data_file = DataFile(str(file_path))
            n_entries = data_file.tadc.get_number_of_entries()
        except Exception as e:
            print(f"Error opening {file_path}: {e}, skipping.")
            continue

        print(f"Scanning {file_path.name} ({n_entries} entries)...")

        for i in range(n_entries):
            data_file.tadc.get_entry(i)

            feb_id = data_file.tadc.du_id[0] if hasattr(data_file.tadc, "du_id") and len(data_file.tadc.du_id) > 0 else None
            if feb_id is None:
                continue

            sec = int(data_file.tadc.du_seconds[0]) if hasattr(data_file.tadc, "du_seconds") and len(data_file.tadc.du_seconds) > 0 else 0
            nsec = int(data_file.tadc.du_nanoseconds[0]) if hasattr(data_file.tadc, "du_nanoseconds") and len(data_file.tadc.du_nanoseconds) > 0 else 0
            timestamp_sec = float(sec) + float(nsec) / 1e9

            # Determine trace length (checking channel 1 / X)
            trace_length = 0
            if hasattr(data_file.tadc, "trace_ch") and len(data_file.tadc.trace_ch) > 0:
                if len(data_file.tadc.trace_ch[0]) > 1:
                    trace_length = len(data_file.tadc.trace_ch[0][1])
                elif len(data_file.tadc.trace_ch[0]) > 0:
                    trace_length = len(data_file.tadc.trace_ch[0][0])

            records.append({
                "feb_id": int(feb_id),
                "timestamp_sec": timestamp_sec,
                "file_path": str(file_path),
                "entry_index": int(i),
                "trace_length": int(trace_length),
            })

        data_file.close()

    if not records:
        return pd.DataFrame(columns=["feb_id", "timestamp_sec", "file_path", "entry_index", "trace_length"])

    df = pd.DataFrame(records)
    # Sort globally by feb_id, then timestamp_sec
    df.sort_values(by=["feb_id", "timestamp_sec"], inplace=True)
    df.reset_index(drop=True, inplace=True)
    return df


def generate_lookup_table(
    md_source: Path,
    output_path: Path,
    file_pattern: str = "*.root",
):
    """
    Generate and save MD lookup table from a file, directory, or pattern.
    """
    md_source = Path(md_source)
    if md_source.is_file():
        files = [md_source]
    elif md_source.is_dir():
        files = sorted(list(md_source.glob(f"**/{file_pattern}")))
    else:
        # Try glob pattern
        parent = md_source.parent
        pattern = md_source.name
        files = sorted(list(parent.glob(pattern)))

    if not files:
        raise FileNotFoundError(f"No MD ROOT files found matching: {md_source}")

    print(f"Found {len(files)} MD ROOT file(s) to process.")
    df = build_md_lookup_dataframe(files)

    if df.empty:
        print("Warning: No valid MD entries found across the specified files.")
        return

    output_path = Path(output_path).resolve()
    output_path.parent.mkdir(parents=True, exist_ok=True)

    suffix = output_path.suffix.lower()
    if suffix == ".parquet":
        df.to_parquet(output_path, index=False)
    elif suffix in (".pkl", ".pickle"):
        df.to_pickle(output_path)
    else:
        # Default to CSV
        df.to_csv(output_path, index=False)

    print(f"\nLookup table successfully created with {len(df)} total entries.")
    print(f"Unique FEBs: {df['feb_id'].nunique()} (IDs: {sorted(df['feb_id'].unique().tolist())})")
    print(f"Saved to: {output_path}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Create master MD lookup table for DU-dependent noise injection.")
    parser.add_argument(
        "--md_source",
        type=str,
        default="/Users/xishui/Dropbox/Non_sense/202603_PMO_visit/comms/20260629_1744/data/root_data/",
        help="Path to an MD ROOT file or directory containing MD ROOT files.",
    )
    parser.add_argument(
        "--output",
        type=str,
        default="results/md_lookup_table.csv",
        help="Output lookup table file path (.csv, .parquet, or .pkl).",
    )
    parser.add_argument(
        "--pattern",
        type=str,
        default="*.root",
        help="Glob pattern to search for ROOT files within directories.",
    )

    args = parser.parse_args()
    generate_lookup_table(
        md_source=Path(args.md_source),
        output_path=ROOT / args.output if not Path(args.output).is_absolute() else Path(args.output),
        file_pattern=args.pattern,
    )
