"""Unit tests for FLT0/thresholds.py (no ROOT needed). Run: python3 -m unittest FLT0/test_thresholds.py"""
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from thresholds import load_flt0_thresholds  # noqa: E402

HEADER = "feb_id th1_x th2_x th1_y th2_y\n"


def _write(text):
    f = tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False)
    f.write(text)
    f.close()
    return f.name


class TestLoader(unittest.TestCase):
    def test_valid(self):
        t = load_flt0_thresholds(_write("# comment\n\n" + HEADER + "109 66 44 65 43\r\n110, 70, 50, 69, 49\n"))
        self.assertEqual(t.get(109), ((66.0, 44.0), (65.0, 43.0)))
        self.assertEqual(t.get(110), ((70.0, 50.0), (69.0, 49.0)))
        self.assertEqual(len(t), 2)

    def test_unknown_feb_is_none(self):
        t = load_flt0_thresholds(_write(HEADER + "109 66 44 65 43\n"))
        self.assertIsNone(t.get(1))
        self.assertIsNone(t.get(None))
        self.assertFalse(t.has(1))

    def test_equal_thresholds_allowed(self):
        t = load_flt0_thresholds(_write(HEADER + "109 50 50 50 50\n"))
        self.assertTrue(t.has(109))

    def test_missing_file(self):
        with self.assertRaises(FileNotFoundError):
            load_flt0_thresholds("/nonexistent/threshold.csv")

    def test_missing_header(self):
        with self.assertRaises(ValueError):
            load_flt0_thresholds(_write("109 66 44 65 43\n"))

    def test_wrong_column_name(self):
        with self.assertRaises(ValueError):
            load_flt0_thresholds(_write("feb_id th1_x th2_x th1_y th3_y\n109 66 44 65 43\n"))

    def test_wrong_field_count(self):
        with self.assertRaises(ValueError):
            load_flt0_thresholds(_write(HEADER + "109 66 44 65\n"))

    def test_non_numeric(self):
        with self.assertRaises(ValueError):
            load_flt0_thresholds(_write(HEADER + "109 66 abc 65 43\n"))

    def test_duplicate_feb(self):
        with self.assertRaises(ValueError):
            load_flt0_thresholds(_write(HEADER + "109 66 44 65 43\n109 60 40 60 40\n"))

    def test_th1_less_than_th2(self):
        with self.assertRaises(ValueError):
            load_flt0_thresholds(_write(HEADER + "109 40 44 65 43\n"))
        with self.assertRaises(ValueError):
            load_flt0_thresholds(_write(HEADER + "109 66 44 40 43\n"))

    def test_empty_and_header_only(self):
        with self.assertRaises(ValueError):
            load_flt0_thresholds(_write(""))
        with self.assertRaises(ValueError):
            load_flt0_thresholds(_write(HEADER))

    def test_repo_files_load(self):
        here = Path(__file__).resolve().parent
        for name in ("threshold_FEBs.csv", "threshold_FEBs_example.csv"):
            self.assertGreater(len(load_flt0_thresholds(here / name)), 0)


if __name__ == "__main__":
    unittest.main()
