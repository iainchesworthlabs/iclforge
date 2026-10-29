"""The 256-entry noise table of TH spec 17.4 in the C++ decoder and in the Python reference.

The decoder's table (libs/truehd/src/core/noise.hpp) and the reference's
(tools/references/truehd_syntax.py) were typed from the specification separately, so that a typing
mistake in one shows as a difference here and in the hand-built streams' decode, which uses entries
of the table at random."""

import pathlib
import re
import sys
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "references"))
import truehd_syntax as ref  # noqa: E402

HEADER = REPO / "libs" / "truehd" / "src" / "core" / "noise.hpp"


def header_table():
    text = HEADER.read_text()
    body = re.search(r"kNoiseTable\{\{(.*?)\}\};", text, re.S).group(1)
    body = re.sub(r"//.*", "", body)
    return [int(value) for value in re.findall(r"-?\d+", body)]


class NoiseTableTest(unittest.TestCase):
    def test_the_tables_agree(self):
        self.assertEqual(len(ref.NOISE_TABLE), 256)
        self.assertEqual(header_table(), list(ref.NOISE_TABLE))

    def test_the_values_are_int8_in_the_range_the_specification_gives(self):
        # TH spec 17.4: values range from -28 to +92.
        self.assertEqual((min(ref.NOISE_TABLE), max(ref.NOISE_TABLE)), (-28, 92))


if __name__ == "__main__":
    unittest.main()
