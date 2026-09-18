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
``parse_duplicate_reject_log(path, max_events=None)``
    Returns a list of events, each being a list of ``(du_id: int, ns: int)``
    tuples, in file order.

``compute_ptd(hits)``
    Mirrors ``T3Filter::calculatePairDifferences``.  Given a list of
    ``(du_id, ns)`` tuples, returns a ``dict`` mapping
    ``(min_id, max_id) -> delta_t_ns`` for every ordered DU pair.
"""

from __future__ import annotations

import ast
import re
from pathlib import Path
from typing import List, Optional, Tuple

# Type aliases
Hit = Tuple[int, int]          # (du_id, ns)
Event = List[Hit]              # list of hits for one event
PTD = dict                     # {(min_id, max_id): delta_t_ns}

# Pre-compiled regex: captures the hit-list substring (everything inside the
# outermost brackets), tolerates optional trailing whitespace / CR.
_LINE_RE = re.compile(r"^\d+\.\d+:\s*(\[.*\])\s*$")


def parse_duplicate_reject_log(
    path: "str | Path",
    max_events: Optional[int] = None,
) -> List[Event]:
    """Parse a DuplicateRejectLog file.

    Parameters
    ----------
    path:
        Path to the ``.log`` file produced by the online DAQ.
    max_events:
        If given, stop after reading this many events (first N lines).
        Pass ``None`` to read the entire file.

    Returns
    -------
    List[Event]
        Each element is a list of ``(du_id, ns)`` int tuples representing
        the DU hits of one rejected event, in file order (chronological).
    """
    events: List[Event] = []
    path = Path(path)
    with path.open("r", encoding="utf-8", errors="replace") as fh:
        for raw_line in fh:
            line = raw_line.strip()
            if not line:
                continue

            m = _LINE_RE.match(line)
            if m is None:
                # Malformed line -- skip silently
                continue

            hit_list_str = m.group(1)
            try:
                # ast.literal_eval safely parses a Python list of tuples.
                parsed = ast.literal_eval(hit_list_str)
            except (ValueError, SyntaxError):
                continue

            if not isinstance(parsed, list) or len(parsed) < 2:
                # Need at least 2 DUs to form one pair
                continue

            event: Event = []
            valid = True
            for item in parsed:
                if not (isinstance(item, (tuple, list)) and len(item) == 2):
                    valid = False
                    break
                du_id, ns = item
                event.append((int(du_id), int(ns)))

            if valid and len(event) >= 2:
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
