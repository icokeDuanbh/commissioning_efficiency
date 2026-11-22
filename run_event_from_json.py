"""Match reference JSON events to simulation ROOT entries and append trigger results."""

import argparse
import json
from pathlib import Path
import re
from tempfile import NamedTemporaryFile
from time import perf_counter


PROJECT = Path(__file__).resolve().parent
DATA = Path("/sharegp/home/duanbh/sdb2/duanbh/comissioning_efficiency/data")
JSON_FILE = DATA / "lyon/json/ana/Efficiency_ArseneEmulator-no-noise_0001_9.0-9.2_67-72_th30.json"
EVENT_START = 100
EVENT_STOP = 200
SIM_TIMESTAMP = 1788245469
EVENT_TIME_STEP_SECONDS = 10
MD_LOOKUP = DATA / "202609/MD_root/md_lookup_table_root.csv"
DUPLICATE_LOG = DATA / "202609/fingerprint/anaComm/DuplicateRejectLog_10438_20260901_084336.log"


def match_root(reference, root_dir):
    match = re.fullmatch(r"Efficiency_.+?_(\d+_.+)_th[^_]+", reference.stem)
    if match is None:
        raise ValueError(f"Cannot infer simulation filename from {reference.name}")
    name = f"TTrees_from_parameters_{match[1]}.root"
    candidates = [reference.parent / name, root_dir / name]
    for path in candidates:
        if path.is_file():
            return path.resolve()
    raise FileNotFoundError(f"{name} not found in {reference.parent} or {root_dir}")


def index_entries(root_path):
    import ROOT

    root_file = ROOT.TFile.Open(str(root_path), "READ")
    if not root_file or root_file.IsZombie():
        raise OSError(f"Cannot open {root_path}")
    try:
        tree = root_file.Get("tadc")
        if not tree or not tree.InheritsFrom("TTree"):
            raise ValueError(f"Missing tadc tree in {root_path}")
        branch = tree.GetBranch("event_number")
        leaf = tree.GetLeaf("event_number")
        if not branch or not leaf:
            raise ValueError(f"Missing tadc/event_number in {root_path}")
        entries = {}
        for entry in range(int(tree.GetEntries())):
            if branch.GetEntry(entry) <= 0:
                raise OSError(f"Cannot read tadc/event_number at entry {entry}: {root_path}")
            number = int(leaf.GetValueLong64())
            if number in entries:
                raise ValueError(f"Ambiguous event_number {number} in {root_path}")
            entries[number] = entry
        return entries
    finally:
        root_file.Close()


def encode_numpy(value):
    import numpy as np

    if isinstance(value, np.ndarray):
        return value.tolist()
    if isinstance(value, np.generic):
        return value.item()
    raise TypeError(f"Cannot encode {type(value).__name__}")


def write_output(path, records):
    with NamedTemporaryFile("w", encoding="utf-8", dir=path.parent, delete=False) as stream:
        temp = Path(stream.name)
        try:
            json.dump(records, stream, indent=2, default=encode_numpy, allow_nan=False)
            stream.write("\n")
        except BaseException:
            stream.close()
            temp.unlink()
            raise
    try:
        temp.replace(path)
    finally:
        temp.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path, nargs="?", default=JSON_FILE)
    parser.add_argument("--root-dir", type=Path, default=DATA / "lyon/simulation_data_20261120/ana")
    parser.add_argument("--output", type=Path)
    selection = parser.add_mutually_exclusive_group()
    selection.add_argument("--event", type=int, help="Run one event number from fixed[1]")
    selection.add_argument("--all", action="store_true", help="Process all reference events")
    selection.add_argument("--event-range", type=int, nargs=2, metavar=("START", "STOP"),
                           help="Process event numbers in [START, STOP)")
    parser.add_argument("--match-only", action="store_true", help="Check matching without running triggers")
    args = parser.parse_args()
    reference = args.reference.resolve()
    output = (args.output or reference.with_name(reference.stem + "_pipeline.json")).resolve()
    if output == reference:
        parser.error("Output must be different from the reference JSON")

    records = json.loads(reference.read_text(encoding="utf-8-sig"))
    if not isinstance(records, list) or not records:
        raise ValueError("Expected a nonempty JSON list of event records")
    target = args.event
    event_range = args.event_range
    if event_range is None and args.event is None and not args.all:
        event_range = (EVENT_START, EVENT_STOP)
    if event_range is not None:
        start, stop = event_range
        if start >= stop:
            parser.error("--event-range requires START < STOP")
        selected = [record for record in records if start <= record["fixed"][1] < stop]
        found = {record["fixed"][1] for record in selected}
        missing = [number for number in range(start, stop) if number not in found]
        if missing:
            raise ValueError(f"Event numbers missing from reference JSON: {missing[:10]}")
        selected.sort(key=lambda record: record["fixed"][1])
    else:
        selected = [record for record in records if args.all or record["fixed"][1] == target]
    if not selected:
        raise ValueError(f"No reference record has fixed[1] == {target}")
    numbers = [record["fixed"][1] for record in selected]
    if len(set(numbers)) != len(numbers):
        raise ValueError("Selected JSON records contain duplicate event numbers")

    root_path = match_root(reference, args.root_dir)
    entries = index_entries(root_path)
    missing = [number for number in numbers if number not in entries]
    if missing:
        raise ValueError(f"{len(missing)} JSON events missing from tadc/event_number: {missing[:10]}")
    print(f"{root_path}: matched {len(numbers)} event(s)")
    for number in numbers[:10]:
        print(f"event {number} -> tadc entry {entries[number]}", flush=True)
    if args.match_only:
        return

    from run_full_pipeline import DEFAULT_DISTANCES_FILE, process_event
    from SLT.run_SLT import SLTPipeline
    from FLT0.thresholds import load_flt0_thresholds
    from add_sim_MD.lookup_table import load_lookup_table, load_du_to_feb_map
    from SLT.load_duplicate_history import load_duplicate_history

    pipeline = SLTPipeline(
        trigger_type=2, nhit_threshold=2, time_window_ns=15_000,
        causal_window_enabled=True, antenna_distances_file=str(DEFAULT_DISTANCES_FILE),
        template_match_filter_enable=True, duplicate_filter_enable=True,
        duplicate_history_size=10, duplicate_min_pair=2, duplicate_time_diff_ns=100,
    )
    thresholds = load_flt0_thresholds(PROJECT / "FLT0/threshold_FEBs_October.csv")
    lookup = load_lookup_table(MD_LOOKUP)
    mapping = load_du_to_feb_map(PROJECT / "add_sim_MD/GP65_DUID_FEBID_september.txt")
    history = load_duplicate_history(DUPLICATE_LOG)
    for offset, (record, number) in enumerate(zip(selected, numbers)):
        entry = entries[number]
        timestamp = SIM_TIMESTAMP + offset * EVENT_TIME_STEP_SECONDS
        print(f"Event {number}: sim_timestamp={timestamp}", flush=True)
        started = perf_counter()
        result = process_event(
            fname_sim=str(root_path), i_sim=entry, slt_pipeline=pipeline,
            fname_FLT0_thresholds=thresholds, fname_MD_lookup_table=lookup,
            sim_timestamp=timestamp, fname_DU_to_FEB=mapping,
            fname_duplicate_log=history,
            flt0_params=dict(t_quiet=200, t_period=500, t_sepmax=25, nc_min=2, nc_max=9),
            flt1_score_min=0.1, min_flt1_dus=2, use_python_flt0=True,
        )
        print(f"Event {number}: process_event took {perf_counter() - started:.3f} s", flush=True)
        record["flt_results"] = result["flt_results"]
        record["slt_result"] = result["slt_result"]
    write_output(output, records)
    print(f"Saved {len(selected)} result(s) to {output}")


if __name__ == "__main__":
    main()
