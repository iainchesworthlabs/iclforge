"""Unit tests for export_diff.py's consolidation maps: the libraries a stage of
planning/consolidation.md merges are compared with the one they become, as a union.

stdlib `unittest`, on small `symbols` records built in memory: AC-4's three shared libraries
exporting between them what the one library exports is no change, and a name the merged library
lost or gained is listed.
"""

import contextlib
import io
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import export_diff

OTHERS = {"libiclforge_base.so": ["iclforge::base::f()"]}


def record(libraries: dict[str, list[str]]) -> dict:
    return {"libraries": {**OTHERS, **libraries}}


BEFORE = record(
    {
        "libiclforge_ac4.so": ["iclforge::ac4::scan()"],
        "libiclforge_ac4dec.so": ["iclforge::ac4::Decoder::Decoder()"],
        "libiclforge_ac4enc.so": ["iclforge::ac4::Encoder::Encoder()"],
    }
)


def run(old: dict, new: dict) -> tuple[int, str]:
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        code = export_diff.compare(old, new, "c1", set(), 30)
    return code, out.getvalue()


class Consolidation(unittest.TestCase):
    def test_the_merged_libraries_are_one_group(self) -> None:
        groups = export_diff.consolidation_map("c1", sorted(BEFORE["libraries"]))
        self.assertEqual(
            groups["libiclforge_ac4.so+libiclforge_ac4dec.so+libiclforge_ac4enc.so"],
            ["libiclforge_ac4.so"],
        )
        self.assertEqual(groups["libiclforge_base.so"], ["libiclforge_base.so"])

    def test_the_union_of_the_three_is_the_one(self) -> None:
        ac4 = [lib for lib in BEFORE["libraries"] if lib not in OTHERS]
        after = record({"libiclforge_ac4.so": [n for lib in ac4 for n in BEFORE["libraries"][lib]]})
        code, out = run(BEFORE, after)
        self.assertEqual(code, 0, out)
        self.assertIn("(3) <- libiclforge_ac4.so (3): same", out)

    def test_a_name_the_one_library_lost_is_listed(self) -> None:
        after = record({"libiclforge_ac4.so": ["iclforge::ac4::scan()"]})
        code, out = run(BEFORE, after)
        self.assertEqual(code, 1)
        self.assertIn("only in old: iclforge::ac4::Decoder::Decoder()", out)


if __name__ == "__main__":
    unittest.main()
