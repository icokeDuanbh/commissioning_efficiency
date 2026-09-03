#!/usr/bin/env python3
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FLT1_DIR = Path(__file__).resolve().parent
LIB_PATH = FLT1_DIR / "libevent_initiator.so"

SOURCE_FILES = [
    FLT1_DIR / "event_initiator.cpp",
    ROOT / "grand_daq/du-daq/src/data_format.cpp",
    ROOT / "grand_daq/template_flt_online/template_FLT.cpp",
    ROOT / "grand_daq/template_flt_online/utils.cpp",
    ROOT / "grand_daq/template_flt_online/error_handling.cpp",
]


def needs_build() -> bool:
    if not LIB_PATH.exists():
        return True

    lib_time = LIB_PATH.stat().st_mtime
    return any(src.exists() and src.stat().st_mtime > lib_time for src in SOURCE_FILES)


def build_library() -> Path:
    if not needs_build():
        return LIB_PATH

    cmd = [
        "g++",
        "-std=c++17",
        "-shared",
        "-fPIC",
        "-I", str(ROOT / "grand_daq/du-daq/src"),
        "-I", str(ROOT / "grand_daq/du-daq/eigen-5.0.0"),
        "-I", str(ROOT / "grand_daq/template_flt_online"),
        *[str(path) for path in SOURCE_FILES],
        "-o", str(LIB_PATH),
    ]
    subprocess.run(cmd, cwd=ROOT, check=True)
    return LIB_PATH


if __name__ == "__main__":
    build_library()
    print(f"Built {LIB_PATH}")
