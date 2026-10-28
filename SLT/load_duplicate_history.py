"""Parser for the online DAQ DuplicateRejectLog format.

Each line of the log represents one event rejected by the online duplicate
cut.  Format (example)::

    1788329348.040512147: [(103, 40517423), (1017, 40512147), (1059, 40512148)]

Fields
------
``1788329348``
    GPS second (integer part of the timestamp, 9 digits).
``.040512147``
    Nanosecond sub-second offset (fractional part, 9 digits).  This value
    coincides with one of the DU ns values (the "reference DU").
``(DU_id, ns)``
    Raw intra-second nanosecond timestamp for each DU that contributed to
    the event.  The DU ns values are *independent per-DU* sub-second
    values -- they are not all equal to the event fractional part.  The
    pairwise differences encode the inter-DU propagation delays used by the
    duplicate fingerprint.

Public API
----------
``DuplicateRejectHistory``
    In-memory indexed structure of timestamped rejected events from one or more
    ``DuplicateRejectLog`` files, supporting fast binary-search queries for the
    past N events preceding a target simulation timestamp.

``load_duplicate_history(source)``
    Load a ``DuplicateRejectHistory`` from a file path, directory, list of
    paths, or return an existing ``DuplicateRejectHistory`` instance.

``parse_duplicate_reject_log(path, max_events=None, target_timestamp=None)``
    Returns a list of events, each being a list of ``(du_id: int, ns: int)``
    tuples, in chronological order.  When ``target_timestamp`` is given, returns
    the past ``max_events`` events at or before ``target_timestamp`` (raising
    ``ValueError`` if fewer than ``max_events`` events exist before it).

``compute_ptd(hits)``
    Mirrors ``T3Filter::calculatePairDifferences``.  Given a list of
    ``(du_id, ns)`` tuples, returns a ``dict`` mapping
    ``(min_id, max_id) -> delta_t_ns`` for every ordered DU pair.
"""

from __future__ import annotations

import ast
import re
from pathlib import Path
from typing import List, Optional, Sequence, Tuple, Union
import numpy as np

# Type aliases
Hit = Tuple[int, int]          # (du_id, ns)
Event = List[Hit]              # list of hits for one event
PTD = dict                     # {(min_id, max_id): delta_t_ns}

# Pre-compiled regex: captures the event timestamp (group 1) and the hit-list
# substring inside the outermost brackets (group 2), tolerating optional whitespace.
_LINE_RE = re.compile(r"^\s*(\d+\.\d+):\s*(\[.*\])\s*$")


def _parse_line(raw_line: str) -> Optional[Tuple[float, Event]]:
    """Parse a single line of a DuplicateRejectLog file into (timestamp_sec, hits)."""
    line = raw_line.strip()
    if not line:
        return None

    m = _LINE_RE.match(line)
    if m is None:
        return None

    try:
        timestamp_sec = float(m.group(1))
        parsed = ast.literal_eval(m.group(2))
    except (ValueError, SyntaxError):
        return None

    if not isinstance(parsed, list) or len(parsed) < 2:
        # Need at least 2 DUs to form one pair
        return None

    event: Event = []
    for item in parsed:
        if not (isinstance(item, (tuple, list)) and len(item) == 2):
            return None
        du_id, ns = item
        event.append((int(du_id), int(ns)))

    if len(event) < 2:
        return None

    return timestamp_sec, event


class DuplicateRejectHistory:
    """In-memory lookup table for online DAQ DuplicateRejectLog events.

    Stores all valid rejected events sorted chronologically by timestamp so
    that querying the past N events for any simulated timestamp ``t_sim`` is an
    O(log M) binary search without re-reading the log file.
    """

    def __init__(self, timestamps: np.ndarray, events: List[Event]):
        if len(timestamps) != len(events):
            raise ValueError(
                f"Length mismatch: timestamps ({len(timestamps)}) != events ({len(events)})"
            )
        self.timestamps = np.asarray(timestamps, dtype=np.float64)
        self.events: List[Event] = list(events)

        # Ensure chronological sorting
        if len(self.timestamps) > 1 and np.any(np.diff(self.timestamps) < 0):
            order = np.argsort(self.timestamps, kind="mergesort")
            self.timestamps = self.timestamps[order]
            self.events = [self.events[int(i)] for i in order]

    def __len__(self) -> int:
        return len(self.events)

    @property
    def min_timestamp(self) -> Optional[float]:
        return float(self.timestamps[0]) if len(self.timestamps) > 0 else None

    @property
    def max_timestamp(self) -> Optional[float]:
        return float(self.timestamps[-1]) if len(self.timestamps) > 0 else None

    @classmethod
    def from_files(cls, paths: Union[str, Path, Sequence[Union[str, Path]]]) -> "DuplicateRejectHistory":
        """Load and merge one or more DuplicateRejectLog files into memory."""
        if isinstance(paths, (str, Path)):
            p = Path(paths)
            if p.is_dir():
                file_list = sorted(p.glob("*.log"))
                if not file_list:
                    raise FileNotFoundError(f"No .log files found in directory: {p}")
            else:
                file_list = [p]
        else:
            file_list = [Path(x) for x in paths]

        if not file_list:
            raise ValueError("No DuplicateRejectLog files specified.")

        timestamps: List[float] = []
        events: List[Event] = []

        for path in file_list:
            if not path.exists():
                raise FileNotFoundError(f"DuplicateRejectLog file not found: {path}")
            with path.open("r", encoding="utf-8", errors="replace") as fh:
                for raw_line in fh:
                    parsed_line = _parse_line(raw_line)
                    if parsed_line is not None:
                        ts, ev = parsed_line
                        timestamps.append(ts)
                        events.append(ev)

        return cls(np.array(timestamps, dtype=np.float64), events)

    def get_past_events(
        self,
        target_timestamp: float,
        n_events: int,
    ) -> List[Event]:
        """Return the ``n_events`` events immediately preceding (or at) ``target_timestamp``.

        Parameters
        ----------
        target_timestamp:
            Timestamp (in seconds) of the simulated event.
        n_events:
            Number of past events required before ``target_timestamp``.

        Returns
        -------
        List[Event]
            List of ``n_events`` events in chronological order.

        Raises
        ------
        ValueError
            If fewer than ``n_events`` events exist in the log with
            ``timestamp <= target_timestamp``, or if ``target_timestamp`` is
            beyond the end of the log.
        """
        if n_events < 0:
            raise ValueError(f"n_events must be non-negative, got {n_events}.")
        if n_events == 0:
            return []
        if len(self.timestamps) == 0:
            raise ValueError("DuplicateRejectHistory is empty (0 valid events loaded).")

        target_ts = float(target_timestamp)
        if target_ts > self.timestamps[-1]:
            raise ValueError(
                f"target_timestamp ({target_ts:.9f}) is after the end of the duplicate log "
                f"(max timestamp: {self.timestamps[-1]:.9f})."
            )

        # Find index of first event strictly greater than target_ts;
        # events[0:idx] all have timestamp <= target_ts.
        idx = int(np.searchsorted(self.timestamps, target_ts, side="right"))

        if idx < n_events:
            raise ValueError(
                f"Insufficient duplicate history before target_timestamp={target_ts:.9f}: "
                f"need {n_events} past events, but only {idx} event(s) exist at or before "
                f"this timestamp (log range: [{self.timestamps[0]:.9f}, {self.timestamps[-1]:.9f}])."
            )

        return self.events[idx - n_events : idx]


def _is_duplicate_history_obj(obj) -> bool:
    return isinstance(obj, DuplicateRejectHistory) or (
        hasattr(obj, "get_past_events") and hasattr(obj, "timestamps") and hasattr(obj, "events")
    )


def load_duplicate_history(
    source: Union[str, Path, Sequence[Union[str, Path]], DuplicateRejectHistory]
) -> DuplicateRejectHistory:
    """Load a ``DuplicateRejectHistory`` from a path/list of paths or return an existing instance."""
    if _is_duplicate_history_obj(source):
        return source
    return DuplicateRejectHistory.from_files(source)


def parse_duplicate_reject_log(
    path: Union[str, Path, DuplicateRejectHistory],
    max_events: Optional[int] = None,
    target_timestamp: Optional[float] = None,
) -> List[Event]:
    """Parse a DuplicateRejectLog file (or query a preloaded history).

    Parameters
    ----------
    path:
        Path to the ``.log`` file produced by the online DAQ, or a preloaded
        ``DuplicateRejectHistory`` object.
    max_events:
        - When ``target_timestamp`` is ``None``: stop after reading this many
          events from the beginning of the file (pass ``None`` to read all).
        - When ``target_timestamp`` is provided: number of past events to take
          immediately prior to (or at) ``target_timestamp``.
    target_timestamp:
        If provided, select the past ``max_events`` events with
        ``timestamp <= target_timestamp``.  Raises ``ValueError`` if fewer than
        ``max_events`` events exist prior to ``target_timestamp``.

    Returns
    -------
    List[Event]
        Each element is a list of ``(du_id, ns)`` int tuples representing
        the DU hits of one rejected event, in chronological order.
    """
    if target_timestamp is not None:
        if max_events is None:
            raise ValueError("max_events (N) must be specified when target_timestamp is provided.")
        history = load_duplicate_history(path)
        return history.get_past_events(target_timestamp=target_timestamp, n_events=max_events)

    if _is_duplicate_history_obj(path):
        if max_events is None:
            return list(path.events)
        return list(path.events[:max_events])

    events: List[Event] = []
    path = Path(path)
    with path.open("r", encoding="utf-8", errors="replace") as fh:
        for raw_line in fh:
            parsed_line = _parse_line(raw_line)
            if parsed_line is not None:
                _, event = parsed_line
                events.append(event)
                if max_events is not None and len(events) >= max_events:
                    break

    return events


def compute_ptd(hits: Event) -> PTD:
    """Compute the pair-time-difference fingerprint for one event.

    Mirrors ``T3Filter::calculatePairDifferences`` exactly:
    - Key   : ``(min(id_i, id_j), max(id_i, id_j))``  -- ordered DU-id pair
    - Value : ``t_i - t_j``  (ns, signed)

    When there are multiple hits per DU, all (i, j) combinations are
    included (same as the C++ implementation which iterates over the raw
    detector list without deduplication).

    Parameters
    ----------
    hits:
        List of ``(du_id, ns)`` tuples for one event.

    Returns
    -------
    dict mapping ``(min_id, max_id) -> delta_t_ns``.
    """
    ptd: PTD = {}
    n = len(hits)
    for i in range(n):
        for j in range(i + 1, n):
            id_i, t_i = hits[i]
            id_j, t_j = hits[j]
            key = (min(id_i, id_j), max(id_i, id_j))
            ptd[key] = t_i - t_j
    return ptd


# ---------------------------------------------------------------------------
# Quick self-test
# ---------------------------------------------------------------------------
if __name__ == "__main__":
    import sys

    if len(sys.argv) < 2:
        print("Usage: python load_duplicate_history.py <log_file> [max_events]")
        sys.exit(1)

    log_path = sys.argv[1]
    n_max = int(sys.argv[2]) if len(sys.argv) > 2 else 5

    events = parse_duplicate_reject_log(log_path, max_events=n_max)
    print(f"Parsed {len(events)} events from {log_path}")
    for idx, ev in enumerate(events):
        ptd = compute_ptd(ev)
        print(f"  Event {idx}: {len(ev)} hits, {len(ptd)} pairs")
        for (a, b), dt in list(ptd.items())[:3]:
            print(f"    DU({a},{b}) dt={dt} ns")
        if len(ptd) > 3:
            print(f"    ... ({len(ptd) - 3} more pairs)")
