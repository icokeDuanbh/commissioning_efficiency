"""
FEB-dependent FLT0 thresholds.

The thresholds (and only the thresholds) of FLT0 depend on the FEB. They are read
from an external text file with a mandatory header and one row per FEB:

    feb_id th1_x th2_x th1_y th2_y
    109 66 44 65 43

- Fields are separated by whitespace (commas are also accepted).
- Blank lines and lines starting with '#' are ignored.
- Thresholds are in ADC counts. th1 >= th2 is required for both channels.
"""

from pathlib import Path
from typing import Dict, Optional, Tuple, Union

EXPECTED_HEADER = ["feb_id", "th1_x", "th2_x", "th1_y", "th2_y"]

# ((th1_x, th2_x), (th1_y, th2_y)); channel 0 is X and channel 1 is Y
ChannelThresholds = Tuple[Tuple[float, float], Tuple[float, float]]


class FLT0Thresholds:
    """Per-FEB FLT0 thresholds for both channels."""

    def __init__(self, table: Dict[int, ChannelThresholds], source: Optional[str] = None):
        self._table = table
        self.source = source

    def get(self, feb_id: int) -> Optional[ChannelThresholds]:
        """Return ((th1_x, th2_x), (th1_y, th2_y)) for feb_id, or None if the FEB is unknown."""
        if feb_id is None:
            return None
        return self._table.get(int(feb_id))

    def has(self, feb_id: int) -> bool:
        return self.get(feb_id) is not None

    @property
    def feb_ids(self):
        return sorted(self._table.keys())

    def __len__(self):
        return len(self._table)


def _number(text: str, fname, lineno: int, column: str) -> float:
    try:
        value = float(text)
    except ValueError:
        raise ValueError(f"{fname}:{lineno}: column '{column}' is not numeric: {text!r}")
    return value


def load_flt0_thresholds(fname: Union[str, Path]) -> FLT0Thresholds:
    """
    Load the FEB-dependent FLT0 thresholds from a header-based text file.

    Raises:
        FileNotFoundError: the file does not exist.
        ValueError: missing/misnamed header, wrong number of fields, non-numeric value,
            duplicate feb_id, th1 < th2 for a channel, or no data rows.
    """
    path = Path(fname)
    if not path.is_file():
        raise FileNotFoundError(f"FLT0 threshold file not found: {path}")

    table: Dict[int, ChannelThresholds] = {}
    header_seen = False

    with open(path, "r") as f:
        for lineno, raw in enumerate(f, start=1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            fields = line.replace(",", " ").split()

            if not header_seen:
                if [x.lower() for x in fields] != EXPECTED_HEADER:
                    raise ValueError(
                        f"{path}:{lineno}: expected header '{' '.join(EXPECTED_HEADER)}', got {line!r}"
                    )
                header_seen = True
                continue

            if len(fields) != len(EXPECTED_HEADER):
                raise ValueError(
                    f"{path}:{lineno}: expected {len(EXPECTED_HEADER)} fields "
                    f"({' '.join(EXPECTED_HEADER)}), got {len(fields)}: {line!r}"
                )

            feb_value = _number(fields[0], path, lineno, "feb_id")
            if feb_value != int(feb_value):
                raise ValueError(f"{path}:{lineno}: feb_id must be an integer: {fields[0]!r}")
            feb_id = int(feb_value)

            th1_x, th2_x, th1_y, th2_y = (
                _number(fields[i], path, lineno, EXPECTED_HEADER[i]) for i in range(1, 5)
            )

            if th1_x < th2_x:
                raise ValueError(f"{path}:{lineno}: FEB {feb_id} chX has th1 ({th1_x}) < th2 ({th2_x}); th1 >= th2 is required")
            if th1_y < th2_y:
                raise ValueError(f"{path}:{lineno}: FEB {feb_id} chY has th1 ({th1_y}) < th2 ({th2_y}); th1 >= th2 is required")

            if feb_id in table:
                raise ValueError(f"{path}:{lineno}: duplicate feb_id {feb_id}")
            table[feb_id] = ((th1_x, th2_x), (th1_y, th2_y))

    if not header_seen:
        raise ValueError(f"{path}: file is empty (no header found)")
    if not table:
        raise ValueError(f"{path}: header found but no data rows")

    return FLT0Thresholds(table, source=str(path))
