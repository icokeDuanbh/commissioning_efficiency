#!/usr/bin/env python3
import argparse
import json
import struct
from pathlib import Path


FILE_HEADER_SIZE = 256
RECORD_HEADER_SIZE = 12


def extract_candidates(input_dir: Path, out_dir: Path, max_files: int, max_samples: int):
    files = sorted(p for p in input_dir.iterdir() if p.is_file())[:max_files]
    out_dir.mkdir(parents=True, exist_ok=True)

    summary = {
        "input_dir": str(input_dir),
        "scanned_files": [],
        "waveform_candidates": [],
        "t2_candidates": [],
    }

    candidate_idx = 0
    for path in files:
        file_meta = {"path": str(path), "records_scanned": 0}
        with path.open("rb") as f:
            header = f.read(FILE_HEADER_SIZE)
            if len(header) < FILE_HEADER_SIZE:
                file_meta["error"] = "short_header"
                summary["scanned_files"].append(file_meta)
                continue

            while candidate_idx < max_samples:
                rec_hdr = f.read(RECORD_HEADER_SIZE)
                if not rec_hdr:
                    break
                if len(rec_hdr) < RECORD_HEADER_SIZE:
                    file_meta["error"] = "short_record_header"
                    break
                rec_size, rec_type, rec_source = struct.unpack("<III", rec_hdr)
                if rec_size < RECORD_HEADER_SIZE:
                    file_meta["error"] = "invalid_record_size"
                    break
                payload = f.read(rec_size - RECORD_HEADER_SIZE)
                if len(payload) < rec_size - RECORD_HEADER_SIZE:
                    file_meta["error"] = "short_payload"
                    break
                file_meta["records_scanned"] += 1

                ff12 = payload.find(bytes.fromhex("ff12"))
                ee12 = payload.find(bytes.fromhex("ee12"))

                if ff12 >= 0 and candidate_idx < max_samples:
                    record_payload_path = out_dir / f"sample_{candidate_idx:02d}_record_payload.bin"
                    waveform_body_path = out_dir / f"sample_{candidate_idx:02d}_from_ff12.bin"
                    meta_path = out_dir / f"sample_{candidate_idx:02d}.json"
                    record_payload_path.write_bytes(payload)
                    waveform_body_path.write_bytes(payload[ff12:])
                    meta = {
                        "source_file": str(path),
                        "record_index": file_meta["records_scanned"] - 1,
                        "record_type": rec_type,
                        "record_source": rec_source,
                        "payload_size": len(payload),
                        "ff12_offset": ff12,
                        "candidate_size": len(payload[ff12:]),
                        "record_payload_path": str(record_payload_path),
                        "waveform_body_path": str(waveform_body_path),
                    }
                    meta_path.write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8")
                    summary["waveform_candidates"].append(meta)
                    candidate_idx += 1

                if ee12 >= 0:
                    summary["t2_candidates"].append(
                        {
                            "source_file": str(path),
                            "record_index": file_meta["records_scanned"] - 1,
                            "record_source": rec_source,
                            "ee12_offset": ee12,
                        }
                    )

                if candidate_idx >= max_samples:
                    break

        summary["scanned_files"].append(file_meta)
        if candidate_idx >= max_samples:
            break

    return summary


def main():
    ap = argparse.ArgumentParser(description="Extract replay candidates from legacy .dat files.")
    ap.add_argument("--input-dir", required=True)
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--summary-json", required=True)
    ap.add_argument("--max-files", type=int, default=8)
    ap.add_argument("--max-samples", type=int, default=8)
    args = ap.parse_args()

    summary = extract_candidates(
        Path(args.input_dir),
        Path(args.out_dir),
        args.max_files,
        args.max_samples,
    )
    summary_path = Path(args.summary_json)
    summary_path.parent.mkdir(parents=True, exist_ok=True)
    summary_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
