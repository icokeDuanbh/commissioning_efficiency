#!/usr/bin/env python3
"""Incremental build script for libslt_pipeline.so.

Mirrors the pattern of FLT1/build_event_initiator.py.
Called automatically by run_SLT.py when sources are newer than the library.
"""
import subprocess
from pathlib import Path

ROOT    = Path(__file__).resolve().parent.parent
SLT_DIR = Path(__file__).resolve().parent
LIB_PATH = SLT_DIR / "libslt_pipeline.so"

SOURCE_FILES = [
    SLT_DIR / "t3pipeline_initiator.cpp",
    ROOT / "grand_daq/grand-daq/src/trigger_processor_pipeline.cpp",
    ROOT / "grand_daq/grand-daq/src/trigger_processor_gate_logic.cpp",
    ROOT / "grand_daq/grand-daq/src/duplicate_reject_logger.cpp",
    ROOT / "grand_daq/grand-daq/src/data_parser.cpp",
]


def needs_build() -> bool:
    if not LIB_PATH.exists():
        return True
    lib_mtime = LIB_PATH.stat().st_mtime
    return any(src.exists() and src.stat().st_mtime > lib_mtime for src in SOURCE_FILES)


def build_library() -> Path:
    if not needs_build():
        return LIB_PATH

    cmd = [
        "clang++",
        "-std=c++17",
        "-shared",
        "-fPIC",
        "-I", str(ROOT / "grand_daq/grand-daq/include"),
        "-Wl,-rpath,/Users/xishui/miniconda3/lib",
        *[str(p) for p in SOURCE_FILES],
        "-o", str(LIB_PATH),
    ]
    print(f"Building {LIB_PATH.name} ...")
    subprocess.run(cmd, cwd=ROOT, check=True)
    print("Done.")
    return LIB_PATH


if __name__ == "__main__":
    build_library()
    print(f"Library ready: {LIB_PATH}")
