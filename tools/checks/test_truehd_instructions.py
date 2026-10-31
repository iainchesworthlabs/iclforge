"""Unit tests for truehd_instructions.py: the callgrind parser and the ceiling gate."""

import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import truehd_instructions as ti

CALLGRIND = """\
version: 1
creator: callgrind-3.22.0
cmd: iclforge-bench --only truehd_51_decode
events: Ir
fl=/src/decoder.cpp
fn=iclforge::truehd::Decoder::decode(std::span<std::byte const, 18446744073709551615ul>)
12 5000
cfn=iclforge::truehd::substream_crc(std::span<std::byte const, 18446744073709551615ul>)
calls=3 0
12 300
fn=main
40 10
cfn=iclforge::truehd::Decoder::decode(std::span<std::byte const, 18446744073709551615ul>)
calls=1000 0
41 900000
cfn=iclforge::truehd::Decoder::decode(std::span<std::byte const, 18446744073709551615ul>)
calls=24 0
42 21600
cfn=iclforge::truehd::Decoder::decode_extra(int)
calls=7 0
43 7
totals: 990000
"""


class ParseCallgrind(unittest.TestCase):
    def test_the_total_and_the_calls_of_the_unit_function(self):
        total, calls = ti.parse_callgrind(CALLGRIND, "iclforge::truehd::Decoder::decode")
        self.assertEqual(total, 990000)
        # 1000 and 24 from the two callers; the substream_crc call and decode_extra are other
        # functions and are not counted.
        self.assertEqual(calls, 1024)

    def test_a_function_that_is_not_called_has_no_calls(self):
        _, calls = ti.parse_callgrind(CALLGRIND, "iclforge::truehd::Encoder::encode")
        self.assertEqual(calls, 0)


class Gate(unittest.TestCase):
    def run_main(self, ceiling):
        with tempfile.TemporaryDirectory() as scratch:
            table = Path(scratch) / "ceilings.json"
            table.write_text(json.dumps({"ceilings": {"truehd_51_decode": ceiling}}))
            row = {
                "workload": "truehd_51_decode",
                "calls": 10,
                "instructions_per_unit": 60000,
                "unit_ms": 0.8333,
                "instructions_per_second_of_audio": 72000000,
            }
            argv = [
                "truehd_instructions.py",
                "--bench",
                "x",
                "--workload",
                "truehd_51_decode",
                "--ceilings",
                str(table),
            ]
            with (
                mock.patch.object(sys, "argv", argv),
                mock.patch.object(ti, "run_workload", return_value=row),
            ):
                return ti.main()

    def test_a_workload_over_its_ceiling_fails(self):
        self.assertEqual(self.run_main(59999), 1)

    def test_a_workload_at_its_ceiling_passes(self):
        self.assertEqual(self.run_main(60000), 0)


if __name__ == "__main__":
    unittest.main()
