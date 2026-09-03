#!/usr/bin/env python3
"""
Script to scan all MD ROOT files in a specified directory for a target DU ID,
collect entry indices and exact nanosecond timestamps across ALL files,
sort them globally in chronological order, compute global relative times
with respect to the earliest event across all files, and save the master index list.

Output format (3 columns):
fname entry_index relative_time_sec
"""

import sys
import argparse
from pathlib import Path
import numpy as np
from grand.dataio import DataFile

ROOT = Path(__file__).resolve().parent


def generate_global_md_entries(
    md_dir: Path,
    output_txt: Path = None,
    target_id: int = 1055,
):
    md_dir = Path(md_dir)
    if not md_dir.exists():
        raise FileNotFoundError(f"MD directory not found: {md_dir}")

    if output_txt is None:
        output_txt = ROOT / f"results/md_entries_global_{target_id}.txt"
    else:
        output_txt = Path(output_txt)

    # Find all .root files in md_dir
    root_files = sorted(list(md_dir.glob("**/*.root")))
    if len(root_files) == 0:
        raise FileNotFoundError(f"No .root files found in {md_dir}")

    print(f"Found {len(root_files)} ROOT files in '{md_dir}'. Scanning for DU_ID={target_id}...")

    all_entries = []

    for file_path in root_files:
        data_file = DataFile(str(file_path))
        n_entries = data_file.tadc.get_number_of_entries()

        file_matches = 0
        for i in range(n_entries):
            data_file.tadc.get_entry(i)
            du_id = data_file.tadc.du_id[0] if hasattr(data_file.tadc, "du_id") else None

            if du_id == target_id:
                sec = int(data_file.tadc.du_seconds[0]) if hasattr(data_file.tadc, "du_seconds") else 0
                nsec = int(data_file.tadc.du_nanoseconds[0]) if hasattr(data_file.tadc, "du_nanoseconds") else 0

                # 64-bit nanoseconds timestamp since epoch
                timestamp_ns = sec * 1_000_000_000 + nsec

                all_entries.append({
                    "fname": file_path.name,
                    "file_path": str(file_path),
                    "entry_index": i,
                    "timestamp_ns": timestamp_ns,
                })
                file_matches += 1

        print(f"  [{file_path.name}] Found {file_matches} entries for DU {target_id}")

    if len(all_entries) == 0:
        print(f"No entries found for target DU ID {target_id} across any files.")
        return

    # Sort all entries globally by timestamp_ns
    all_entries.sort(key=lambda x: x["timestamp_ns"])

    # Compute global baseline t0_ns
    t0_ns = all_entries[0]["timestamp_ns"]

    # Build data lines
    lines = []
    lines.append("file_path entry_index relative_time_sec\n")

    for item in all_entries:
        rel_time_sec = (item["timestamp_ns"] - t0_ns) / 1e9
        lines.append(f"{item['file_path']} {item['entry_index']} {rel_time_sec:.9f}\n")

    output_txt.parent.mkdir(parents=True, exist_ok=True)
    with open(output_txt, "w") as f:
        f.writelines(lines)

    print(f"\nSuccessfully saved {len(all_entries)} global sorted entries across {len(root_files)} files.")
    print(f"Global T0: {t0_ns} ns")
    print(f"Output saved to: {output_txt}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Generate global MD entry index list across ROOT files.")
    parser.add_argument(
        "--md_dir",
        type=str,
        default="/Users/xishui/Dropbox/Non_sense/202603_PMO_visit/comms/20260629_1744/data/root_data/",
        help="Directory containing MD ROOT files.",
    )
    parser.add_argument(
        "--out_txt",
        type=str,
        default=None,
        help="Output text file path. Defaults to results/md_entries_global_{target_id}.txt if not specified.",
    )
    parser.add_argument(
        "--target_id",
        type=int,
        default=1055,
        help="Target DU ID to collect entries for.",
    )

    args = parser.parse_args()
    out_txt_path = Path(args.out_txt) if args.out_txt is not None else ROOT / f"results/md_entries_global_{args.target_id}.txt"
    generate_global_md_entries(
        md_dir=Path(args.md_dir),
        output_txt=out_txt_path,
        target_id=args.target_id,
    )
