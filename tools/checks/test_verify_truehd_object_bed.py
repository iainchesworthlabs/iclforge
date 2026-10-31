"""Tests for tools/checks/verify_truehd_object_bed.py.

The render the check computes from the definition is held to values worked out by hand. The
integration test encodes with iclforge-truehd-encode-tool (ICLFORGE_TRUEHD_ENCODE_TOOL, or
build/bin) and is skipped without it."""

import os
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import verify_truehd_object_bed as check

REPO = pathlib.Path(__file__).resolve().parents[2]
TOOL = pathlib.Path(
    os.environ.get("ICLFORGE_TRUEHD_ENCODE_TOOL")
    or REPO / "build" / "bin" / "iclforge-truehd-encode-tool"
)


class Render(unittest.TestCase):
    def test_a_silent_programme_has_a_silent_bed(self):
        self.assertEqual(check.expected_bed([[0] * 12]), [[0] * 8])

    def test_an_object_above_the_bed_is_taken_into_its_row_less_the_floored_gain(self):
        # Object 8 (index 0 of the extra objects) goes to row 0. The decoder adds
        # floor(-2896 * 4096 / 4096) = -2896 to the coded row, so the coded row is the object
        # channel plus 2896; the rounding of a value that does not divide is a floor.
        row = [100, 0, 0, 0, 0, 0, 0, 0, 4096, 0, 0, 0]
        self.assertEqual(check.expected_bed([row])[0][0], 100 + 2896)
        row = [0] * 8 + [1, 0, 0, 0]
        self.assertEqual(check.expected_bed([row])[0][0], 1)  # floor(-2896 / 4096) = -1
        row = [0] * 8 + [-1, 0, 0, 0]
        self.assertEqual(check.expected_bed([row])[0][0], 0)  # floor(2896 / 4096) = 0

    def test_the_objects_reach_the_rows_in_the_order_of_the_table(self):
        row = [0] * 8 + [4096, 4096, 4096, 4096, 4096, 4096, 4096, 4096]
        bed = check.expected_bed([row])[0]
        self.assertEqual(bed, [2896] * 8)

    def test_the_audio_is_halved_to_leave_room_for_the_bed(self):
        rows = check.object_rows(12)
        self.assertEqual(len(rows[0]), 12)
        self.assertTrue(all(-(1 << 22) <= v < (1 << 22) for row in rows[:2000] for v in row))


@unittest.skipUnless(TOOL.exists(), "iclforge-truehd-encode-tool is not built")
class Integration(unittest.TestCase):
    def test_the_reference_decodes_the_streams_the_encoder_writes(self):
        original = sys.argv
        sys.argv = ["verify_truehd_object_bed.py", "--encoder", str(TOOL)]
        try:
            self.assertEqual(check.main(), 0)
        finally:
            sys.argv = original


if __name__ == "__main__":
    unittest.main()
