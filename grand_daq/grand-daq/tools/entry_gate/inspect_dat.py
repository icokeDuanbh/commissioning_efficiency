#!/usr/bin/env python3
import argparse
import json
import os
import re
import struct
from pathlib import Path


FILE_HEADER_SIZE = 256
RECORD_HEADER_SIZE = 12


def u32le(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


def parse_file_header(header_bytes):
    return {
        "file_head_size": u32le(header_bytes, 0),
        "data_version": u32le(header_bytes, 4),
        "run_number": u32le(header_bytes, 8),
        "open_time": u32le(header_bytes, 12),
        "close_time": u32le(header_bytes, 16),
        "filename_size": u32le(header_bytes, 20),
        "filename_raw": header_bytes[24:152].split(b"\x00", 1)[0].decode("utf-8", errors="replace"),
    }


def find_tag_offsets(payload):
    ff12 = payload.find(bytes.fromhex("ff12"))
    ee12 = payload.find(bytes.fromhex("ee12"))
    return ff12, ee12


def inspect_file(path: Path, max_records: int):
    result = {
        "path": str(path),
        "size_bytes": path.stat().st_size,
        "header": None,
        "records": [],
        "record_count_scanned": 0,
    }

    with path.open("rb") as f:
        header = f.read(FILE_HEADER_SIZE)
        if len(header) < FILE_HEADER_SIZE:
            result["error"] = "file too small for header"
            return result
        result["header"] = parse_file_header(header)

        record_idx = 0
        while record_idx < max_records:
            hdr = f.read(RECORD_HEADER_SIZE)
            if not hdr:
                break
            if len(hdr) < RECORD_HEADER_SIZE:
                result["error"] = "truncated record header"
                break
            rec_size, rec_type, rec_source = struct.unpack("<III", hdr)
            if rec_size < RECORD_HEADER_SIZE:
                result["error"] = "invalid record size"
                break
            payload_size = rec_size - RECORD_HEADER_SIZE
            payload = f.read(payload_size)
            if len(payload) < payload_size:
                result["error"] = "truncated payload"
                break
            ff12_off, ee12_off = find_tag_offsets(payload)
            result["records"].append(
                {
                    "index": record_idx,
                    "record_size": rec_size,
                    "record_type": rec_type,
                    "record_source": rec_source,
                    "payload_size": payload_size,
                    "payload_prefix_hex": payload[:16].hex(),
                    "ff12_offset": ff12_off,
                    "ee12_offset": ee12_off,
                }
            )
            record_idx += 1

        result["record_count_scanned"] = record_idx
    return result


def summarize_directory(input_dir: Path, max_files: int, max_records: int):
    files = sorted(p for p in input_dir.iterdir() if p.is_file())
    selected = files[:max_files]
    inspected = [inspect_file(path, max_records) for path in selected]

    file_id_re = re.compile(r".*-(\d+)\.dat$")
    ids = []
    for path in files:
        m = file_id_re.match(path.name)
        if m:
            ids.append(int(m.group(1)))

    return {
        "input_dir": str(input_dir),
        "file_count_total": len(files),
        "file_count_inspected": len(selected),
        "first_file": files[0].name if files else None,
        "last_file": files[-1].name if files else None,
        "first_file_id": min(ids) if ids else None,
        "last_file_id": max(ids) if ids else None,
        "files": inspected,
    }


def main():
    ap = argparse.ArgumentParser(description="Inspect legacy .dat files for entry-gate work.")
    ap.add_argument("--input-dir", required=True)
    ap.add_argument("--out-json", required=True)
    ap.add_argument("--max-files", type=int, default=20)
    ap.add_argument("--max-records", type=int, default=4)
    args = ap.parse_args()

    summary = summarize_directory(Path(args.input_dir), args.max_files, args.max_records)
    out_path = Path(args.out_json)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
